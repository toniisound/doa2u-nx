// SPDX-License-Identifier: GPL-3.0-or-later
// Frame-time profiler for the Switch bring-up.
//
// Every hand-written adapter the game reaches (D3D draws and state, DirectSound,
// CRI, XInput...) and every Xbox kernel function is resolved through
// recomp_lookup_manual / recomp_lookup_kernel. Those lookups are wrapped at
// link time to return a timing thunk per target, so each adapter's exclusive
// time is measured without touching the runtime. The guest thread's idle
// sleeps (nanosleep in the scheduler) and the indirect-call lookups are
// timed as well; what remains of the wall time is the lifted game code.
//
// Guest threads are fibers on one host thread. Each fiber keeps its own scope
// stack, and time spent switched out is excluded from the scopes left open.
//
// A stall watcher on another core reports when no frame has been presented
// for 3 s: where the time went since the last frame, the scopes open on the
// guest thread and the indirect-call targets the guest keeps resolving, which
// name the loop it is stuck in.
#include <switch.h>

#include <time.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <utility>
#include <vector>

typedef void (*RecompFunction)(void);
extern "C" void *recomp_native_fiber_current(void);

namespace {

constexpr int kSlots = 384;
constexpr int kDepth = 64;
constexpr int kFibers = 32;

enum Kind : uint8_t { KIND_ADAPTER, KIND_KERNEL };

struct Slot {
    uint32_t address = 0;
    Kind kind = KIND_ADAPTER;
    RecompFunction target = nullptr;
    uint64_t ticks = 0;  // exclusive, this second
    uint64_t calls = 0;  // this second
};

struct Scope {
    uint64_t start;
    uint64_t child;
    int slot; // -1 idle, -2 lookup, otherwise slot index
};

struct FiberStack {
    void *fiber = nullptr;
    int depth = 0;
    Scope scopes[kDepth];
    uint64_t resumed = 0;   // when this fiber last got the guest thread
    uint64_t run_ticks = 0; // wall time on the guest thread this second
    uint64_t switches = 0;  // times switched away from this second
};

Slot slots[kSlots];
int slot_count = 0;
int16_t slot_index[2048]; // open addressing on the address, -1 empty
bool tables_ready = false;

FiberStack fiber_stacks[kFibers];
FiberStack *current_stack = &fiber_stacks[0];
Handle guest_thread = INVALID_HANDLE;

uint64_t idle_ticks = 0;
uint64_t lookup_ticks = 0;
uint64_t lookup_calls = 0;
uint64_t lost_scopes = 0;

// Indirect-call targets seen since the last frame (stall diagnosis).
constexpr int kTargets = 1024;
struct TargetCount { uint32_t address; uint32_t count; };
TargetCount targets[kTargets];
uint64_t target_overflow = 0;
volatile uint64_t last_present_tick = 0;
uint64_t frame_lookups = 0;  // lookups since the last frame
Thread stall_thread;

void noteTarget(uint32_t address)
{
    uint32_t h = (address * 2654435761u) >> 22; // 10 bits
    for (int probe = 0; probe < 16; ++probe, h = (h + 1) & (kTargets - 1)) {
        if (targets[h].address == address) { ++targets[h].count; return; }
        if (targets[h].count == 0) { targets[h].address = address; targets[h].count = 1; return; }
    }
    ++target_overflow;
}

inline uint64_t now() { return armGetSystemTick(); }

void initTables()
{
    if (tables_ready) return;
    std::memset(slot_index, 0xff, sizeof slot_index);
    tables_ready = true;
}

bool onGuestThread()
{
    return guest_thread != INVALID_HANDLE && threadGetCurHandle() == guest_thread;
}

FiberStack *stackFor(void *fiber)
{
    for (auto &stack : fiber_stacks) {
        if (stack.fiber == fiber) return &stack;
    }
    for (auto &stack : fiber_stacks) {
        if (stack.fiber == nullptr) {
            stack.fiber = fiber;
            stack.depth = 0;
            return &stack;
        }
    }
    return &fiber_stacks[kFibers - 1];
}

inline void enter(int slot)
{
    FiberStack *stack = current_stack;
    if (stack->depth >= kDepth) {
        ++lost_scopes;
        return;
    }
    stack->scopes[stack->depth++] = Scope{now(), 0, slot};
}

inline void leave()
{
    FiberStack *stack = current_stack;
    if (stack->depth == 0) {
        ++lost_scopes;
        return;
    }
    const Scope scope = stack->scopes[--stack->depth];
    const uint64_t elapsed = now() - scope.start;
    const uint64_t exclusive = elapsed > scope.child ? elapsed - scope.child : 0;
    if (scope.slot >= 0) {
        slots[scope.slot].ticks += exclusive;
        ++slots[scope.slot].calls;
    } else if (scope.slot == -1) {
        idle_ticks += exclusive;
    } else {
        lookup_ticks += exclusive;
        ++lookup_calls;
    }
    if (stack->depth != 0) stack->scopes[stack->depth - 1].child += elapsed;
}

template <int N>
void thunk()
{
    if (!onGuestThread()) {
        slots[N].target();
        return;
    }
    enter(N);
    slots[N].target();
    leave();
}

template <int... I>
constexpr std::array<RecompFunction, sizeof...(I)> makeThunks(std::integer_sequence<int, I...>)
{
    return {{&thunk<I>...}};
}

const auto thunks = makeThunks(std::make_integer_sequence<int, kSlots>{});

RecompFunction instrument(uint32_t address, RecompFunction target, Kind kind)
{
    if (target == nullptr) return nullptr;
    initTables();
    uint32_t h = (address * 2654435761u) >> 21; // 11 bits
    for (int probe = 0; probe < 2048; ++probe, h = (h + 1) & 2047u) {
        const int index = slot_index[h];
        if (index < 0) {
            if (slot_count >= kSlots) return target; // table full: untimed
            const int slot = slot_count++;
            slots[slot].address = address;
            slots[slot].kind = kind;
            slots[slot].target = target;
            slot_index[h] = (int16_t)slot;
            return thunks[slot];
        }
        if (slots[index].address == address) {
            slots[index].target = target;
            return thunks[index];
        }
    }
    return target;
}

} // namespace

extern "C" {

// dsound_buffer_adapter.c under RECOMP_DSOUND_LOCK_STATS.
__attribute__((weak)) void recomp_dsound_lock_stats_take(uint64_t *wait_ns, uint64_t *waits, uint64_t *pump_ns,
    uint64_t *pump_max_ns, uint64_t *passes);

RecompFunction __real_recomp_lookup_manual(uint32_t guest_address);
RecompFunction __wrap_recomp_lookup_manual(uint32_t guest_address)
{
    return instrument(guest_address, __real_recomp_lookup_manual(guest_address), KIND_ADAPTER);
}

RecompFunction __real_recomp_lookup_kernel(uint32_t guest_address);
RecompFunction __wrap_recomp_lookup_kernel(uint32_t guest_address)
{
    return instrument(guest_address, __real_recomp_lookup_kernel(guest_address), KIND_KERNEL);
}

// Indirect calls in the lifted code resolve their target here first.
RecompFunction __real_recomp_program_lookup(uint32_t guest_address);
RecompFunction __wrap_recomp_program_lookup(uint32_t guest_address)
{
    if (!onGuestThread()) return __real_recomp_program_lookup(guest_address);
    noteTarget(guest_address);
    ++frame_lookups;
    enter(-2);
    RecompFunction function = __real_recomp_program_lookup(guest_address);
    leave();
    return function;
}

void __real_recomp_native_fiber_switch(void *target);
void __wrap_recomp_native_fiber_switch(void *target)
{
    FiberStack *own = current_stack;
    if (own->fiber == nullptr) own->fiber = recomp_native_fiber_current();
    const uint64_t out = now();
    if (own->resumed != 0) own->run_ticks += out - own->resumed;
    ++own->switches;
    current_stack = stackFor(target);
    current_stack->resumed = out;
    __real_recomp_native_fiber_switch(target);
    current_stack = own;
    const uint64_t back = now();
    own->resumed = back;
    const uint64_t away = back - out;
    if (own->depth != 0) own->scopes[own->depth - 1].child += away;
}

/* The x87 transcendentals the lifted code calls (musl_math under
   RECOMP_PORTABLE_MATH): calls and time per frame. */
uint64_t math_calls = 0, math_ticks = 0;
#define DOA2U_NX_TIMED_MATH1(name) \
    double __real_##name(double); \
    double __wrap_##name(double x) { const uint64_t t = now(); const double r = __real_##name(x); \
        math_ticks += now() - t; ++math_calls; return r; }
#define DOA2U_NX_TIMED_MATH2(name) \
    double __real_##name(double, double); \
    double __wrap_##name(double y, double x) { const uint64_t t = now(); const double r = __real_##name(y, x); \
        math_ticks += now() - t; ++math_calls; return r; }
DOA2U_NX_TIMED_MATH1(recomp_sin)
DOA2U_NX_TIMED_MATH1(recomp_cos)
DOA2U_NX_TIMED_MATH1(recomp_tan)
DOA2U_NX_TIMED_MATH2(recomp_atan2)
DOA2U_NX_TIMED_MATH1(recomp_log2)
DOA2U_NX_TIMED_MATH1(recomp_log1p)
DOA2U_NX_TIMED_MATH1(recomp_expm1)

int __real_nanosleep(const struct timespec *request, struct timespec *remaining);
int __wrap_nanosleep(const struct timespec *request, struct timespec *remaining)
{
    if (!onGuestThread()) return __real_nanosleep(request, remaining);
    enter(-1);
    const int rc = __real_nanosleep(request, remaining);
    leave();
    return rc;
}

uint64_t doa2u_nx_kernel_calls(void); // boot_trace.cpp
// d3d_backend_gl.cpp; absent from the probe build.
__attribute__((weak)) const char *doa2u_nx_render_stage(uint64_t *since_ms, uint64_t *count);

// The presenter calls this at each present: the stall watcher's clock and
// the start of a new window of indirect-call targets.
void doa2u_nx_profile_note_present(void)
{
    last_present_tick = now();
    std::memset(targets, 0, sizeof targets);
    target_overflow = 0;
    frame_lookups = 0;
}

static void stallWatcher(void *)
{
    const double tick_ms = 1000.0 / armGetSystemTickFreq();
    uint64_t reported_for = 0, last_report = 0;
    uint64_t kernel_base = 0, idle_base = 0, lookup_base = 0;
    static uint64_t slot_base[kSlots];
    for (;;) {
        svcSleepThread(1'000'000'000);
        const uint64_t present = last_present_tick;
        if (present == 0) continue;
        const uint64_t t = now();
        const double since_ms = (t - present) * tick_ms;
        if (since_ms < 3000.0) continue;
        // One report when a stall starts, then one every 10 s while it lasts.
        if (reported_for == present && (t - last_report) * tick_ms < 10000.0) continue;
        const bool first = reported_for != present;
        reported_for = present;
        last_report = t;
        if (first) {
            kernel_base = 0;
            idle_base = 0;
            lookup_base = 0;
            std::memset(slot_base, 0, sizeof slot_base);
        }

        // Racy snapshot of the guest thread's counters: diagnosis only.
        const uint64_t kernel = doa2u_nx_kernel_calls();
        const uint64_t idle = idle_ticks, lookups = frame_lookups;
        int order[kSlots];
        uint64_t delta[kSlots];
        const int count = slot_count;
        for (int i = 0; i < count; ++i) {
            order[i] = i;
            delta[i] = slots[i].ticks - slot_base[i];
            slot_base[i] = slots[i].ticks;
        }
        std::sort(order, order + count, [&](int a, int b) { return delta[a] > delta[b]; });
        char top[384];
        int used = 0;
        for (int i = 0; i < count && i < 6 && used < (int)sizeof top; ++i) {
            const Slot &sl = slots[order[i]];
            if (delta[order[i]] == 0) break;
            used += std::snprintf(top + used, sizeof top - used, " %s%08x=%.0fms",
                sl.kind == KIND_KERNEL ? "k" : "", (unsigned)sl.address, delta[order[i]] * tick_ms);
        }

        TargetCount hot[8] = {};
        for (const TargetCount &tc : targets) {
            if (tc.count == 0) continue;
            for (int i = 0; i < 8; ++i) {
                if (tc.count > hot[i].count) {
                    for (int j = 7; j > i; --j) hot[j] = hot[j - 1];
                    hot[i] = tc;
                    break;
                }
            }
        }
        char hot_text[256];
        int hot_used = 0;
        for (const TargetCount &tc : hot) {
            if (tc.count == 0 || hot_used >= (int)sizeof hot_text) break;
            hot_used += std::snprintf(hot_text + hot_used, sizeof hot_text - hot_used,
                " %08x/%u", (unsigned)tc.address, (unsigned)tc.count);
        }

        char open[256];
        int open_used = 0;
        const FiberStack *stack = current_stack;
        const int depth = stack->depth < kDepth ? stack->depth : kDepth;
        for (int i = 0; i < depth && open_used < (int)sizeof open; ++i) {
            const int slot = stack->scopes[i].slot;
            if (slot >= 0) {
                open_used += std::snprintf(open + open_used, sizeof open - open_used, " %s%08x",
                    slots[slot].kind == KIND_KERNEL ? "k" : "", (unsigned)slots[slot].address);
            } else {
                open_used += std::snprintf(open + open_used, sizeof open - open_used,
                    slot == -1 ? " idle" : " lookup");
            }
        }

        if (doa2u_nx_render_stage != nullptr) {
            uint64_t since_ms = 0u, count = 0u;
            const char *name = doa2u_nx_render_stage(&since_ms, &count);
            const uint64_t now_ms = static_cast<uint64_t>(armTicksToNs(armGetSystemTick()) / 1000000u);
            (void)now_ms;
            std::fprintf(stderr, "[stall] render thread: %s (step %llu)\n", name ? name : "?",
                (unsigned long long)count);
        }
        std::fprintf(stderr,
            "[stall] no frame for %.1f s: kernel_calls+%llu lookups=%llu idle=%.0fms "
            "fiber=%p open:%s top:%s hot:%s%s\n",
            since_ms / 1000.0, (unsigned long long)(kernel - kernel_base),
            (unsigned long long)(lookups - lookup_base), (idle - idle_base) * tick_ms,
            stack->fiber, depth ? open : " (none)", used ? top : " (none)",
            hot_used ? hot_text : " (none)", target_overflow ? " (+overflow)" : "");
        kernel_base = kernel;
        idle_base = idle;
        lookup_base = lookups;
    }
}

/* Sampling profiler of the lifted code. Once a millisecond, from core 2, the
   guest thread is paused for an instant and its program counter read; each
   sample is charged to the lifted function it falls in, found among the
   program's entry points (recomp_lookup over the guest code range, built once
   before sampling starts). Every 5 s the busiest functions are logged by guest
   address; samples outside the program are "host" code, logged as offsets in
   the .nro for doa2u-nx.map. Nothing that can take a lock runs while the
   guest is paused. */
RecompFunction recomp_lookup(uint32_t guest_address);  // generated dispatch
extern char __start__[];                               // the .nro's first byte

namespace {
struct Entry { uintptr_t host; uint32_t guest; };
std::vector<Entry> entries;
Thread sampler_thread;

void samplerLoop(void *)
{
    std::fprintf(stderr, "recomp sampler: indexing the lifted functions\n");
    // Entry points: every lifted function the dispatch table knows.
    for (uint32_t va = 0x00010000u; va < 0x00400000u; ++va) {
        if (RecompFunction f = recomp_lookup(va)) entries.push_back({reinterpret_cast<uintptr_t>(f), va});
    }
    std::sort(entries.begin(), entries.end(), [](const Entry &a, const Entry &b) { return a.host < b.host; });
    if (entries.empty()) return;
    const uintptr_t lifted_low = entries.front().host, lifted_high = entries.back().host + 0x10000u;
    std::fprintf(stderr, "recomp sampler: %zu lifted functions at nro+0x%zx..0x%zx\n", entries.size(),
        static_cast<size_t>(lifted_low - reinterpret_cast<uintptr_t>(__start__)),
        static_cast<size_t>(lifted_high - reinterpret_cast<uintptr_t>(__start__)));

    constexpr int kHot = 2048;
    struct Hot { uintptr_t key; uint32_t count; };
    static Hot lifted[kHot], host[kHot];
    uint32_t total = 0, missed = 0, host_total = 0;
    const auto charge = [](Hot *table, uintptr_t key) {
        uint32_t h = static_cast<uint32_t>((key >> 2) * 2654435761u) & (kHot - 1);
        for (int probe = 0; probe < 64; ++probe, h = (h + 1) & (kHot - 1)) {
            if (table[h].count == 0) { table[h].key = key; table[h].count = 1; return; }
            if (table[h].key == key) { ++table[h].count; return; }
        }
    };
    uint64_t window_start = now();
    const uint64_t window = armGetSystemTickFreq() * 5;
    for (;;) {
        svcSleepThread(1'000'000);
        const Handle target = guest_thread;
        if (target == INVALID_HANDLE) continue;
        static Result first_error = 0;
        const Result paused = svcSetThreadActivity(target, ThreadActivity_Paused);
        ThreadContext context;
        const Result got = R_SUCCEEDED(paused) ? svcGetThreadContext3(&context, target) : paused;
        if (R_SUCCEEDED(paused)) svcSetThreadActivity(target, ThreadActivity_Runnable);
        if (R_FAILED(got)) {
            if (first_error == 0) {
                first_error = got;
                std::fprintf(stderr, "recomp sampler: cannot read the guest thread (0x%x)\n", (unsigned)got);
            }
            ++missed;
            if (now() - window_start >= window) {
                window_start = now();
                std::fprintf(stderr, "recomp sampler: 0 samples (%u missed)\n", missed);
                missed = 0;
            }
            continue;
        }
        const uintptr_t pc = context.pc.x;
        ++total;
        if (pc >= lifted_low && pc < lifted_high) {
            auto it = std::upper_bound(entries.begin(), entries.end(), pc,
                [](uintptr_t value, const Entry &e) { return value < e.host; });
            charge(lifted, it == entries.begin() ? 0u : (it - 1)->guest);
        } else {
            ++host_total;
            // 256-byte buckets of host code, as .nro offsets.
            charge(host, (pc - reinterpret_cast<uintptr_t>(__start__)) & ~static_cast<uintptr_t>(0xff));
        }
        if (now() - window_start < window) continue;
        window_start = now();
        const auto top = [](Hot *table, int n, Hot *out) {
            for (int i = 0; i < n; ++i) out[i] = Hot{0, 0};
            for (int k = 0; k < kHot; ++k) {
                if (table[k].count == 0) continue;
                for (int i = 0; i < n; ++i) {
                    if (table[k].count > out[i].count) {
                        for (int j = n - 1; j > i; --j) out[j] = out[j - 1];
                        out[i] = table[k];
                        break;
                    }
                }
            }
            std::memset(table, 0, sizeof(Hot) * kHot);
        };
        Hot best[16], best_host[8];
        top(lifted, 16, best);
        top(host, 8, best_host);
        char text[640];
        int used = 0;
        for (const Hot &h : best) {
            if (h.count == 0 || used >= (int)sizeof text) break;
            used += std::snprintf(text + used, sizeof text - used, " %08x=%.1f%%", (unsigned)h.key, 100.0 * h.count / total);
        }
        char host_text[320];
        int host_used = 0;
        for (const Hot &h : best_host) {
            if (h.count == 0 || host_used >= (int)sizeof host_text) break;
            host_used += std::snprintf(host_text + host_used, sizeof host_text - host_used, " +%zx=%.1f%%",
                static_cast<size_t>(h.key), 100.0 * h.count / total);
        }
        std::fprintf(stderr, "recomp sampler: %u samples (%u missed), host %.1f%% | lifted:%s | host:%s\n",
            total, missed, total ? 100.0 * host_total / total : 0.0, text, host_text);
        total = missed = host_total = 0;
    }
}
} // namespace

// The guest runs on the thread that calls recomp_runner_main.
void doa2u_nx_profile_start(void)
{
    initTables();
    guest_thread = threadGetCurHandle();
    fiber_stacks[0].fiber = nullptr;
    current_stack = &fiber_stacks[0];
    // Core 2 hosts the audio mixer and the log flusher, both light.
    if (R_SUCCEEDED(threadCreate(&stall_thread, stallWatcher, nullptr, nullptr, 0x8000, 0x2E, 2))) {
        threadStart(&stall_thread);
    }
    // Core 1 at a high priority: the renderer there waits for the game half
    // of each frame, and nothing on core 2 can starve the samples.
    Result sampler = threadCreate(&sampler_thread, samplerLoop, nullptr, nullptr, 0x10000, 0x28, 1);
    if (R_SUCCEEDED(sampler)) sampler = threadStart(&sampler_thread);
    std::fprintf(stderr, "recomp sampler: thread %s (0x%x)\n", R_SUCCEEDED(sampler) ? "started" : "failed",
        (unsigned)sampler);
}

// Called once a second by the presenter with the frames and wall time of
// that second. Prints the breakdown per frame and resets the counters.
void doa2u_nx_profile_report(uint32_t frames, double wall_ms, int console_row)
{
    if (frames == 0) return;
    const double tick_ms = 1000.0 / armGetSystemTickFreq();
    const double per_frame = 1.0 / frames;

    int order[kSlots];
    double adapter_ms = 0.0, kernel_ms = 0.0;
    for (int i = 0; i < slot_count; ++i) {
        order[i] = i;
        const double ms = slots[i].ticks * tick_ms;
        (slots[i].kind == KIND_KERNEL ? kernel_ms : adapter_ms) += ms;
    }
    std::sort(order, order + slot_count, [](int a, int b) { return slots[a].ticks > slots[b].ticks; });
    const double idle_ms = idle_ticks * tick_ms;
    const double lookup_ms = lookup_ticks * tick_ms;
    const double game_ms = wall_ms - idle_ms - adapter_ms - kernel_ms - lookup_ms;

    char top[512];
    int used = 0;
    int shown = 0;
    for (int i = 0; i < slot_count && shown < 6; ++i) {
        const Slot &s = slots[order[i]];
        if (s.ticks == 0) break;
        used += std::snprintf(top + used, sizeof top - used, " %s%08x=%.2fms/%llu",
            s.kind == KIND_KERNEL ? "k" : "", (unsigned)s.address,
            s.ticks * tick_ms * per_frame, (unsigned long long)(s.calls / frames));
        if (used >= (int)sizeof top) break;
        ++shown;
    }

    // Guest threads: the guest thread's wall time by fiber, busiest first.
    {
        FiberStack *current = current_stack;
        const uint64_t t = now();
        if (current->resumed != 0) { current->run_ticks += t - current->resumed; current->resumed = t; }
        int fiber_order[kFibers];
        for (int i = 0; i < kFibers; ++i) fiber_order[i] = i;
        std::sort(fiber_order, fiber_order + kFibers,
            [](int a, int b) { return fiber_stacks[a].run_ticks > fiber_stacks[b].run_ticks; });
        char fibers[256];
        int fused = 0;
        for (int i = 0; i < 5 && fused < (int)sizeof fibers; ++i) {
            FiberStack &f = fiber_stacks[fiber_order[i]];
            if (f.run_ticks == 0) break;
            fused += std::snprintf(fibers + fused, sizeof fibers - fused, " #%d=%.2fms/%llu", fiber_order[i],
                f.run_ticks * tick_ms * per_frame, (unsigned long long)(f.switches / frames));
        }
        for (FiberStack &f : fiber_stacks) f.run_ticks = f.switches = 0;
        uint64_t wait_ns = 0, waits = 0, pump_ns = 0, pump_max_ns = 0, passes = 0;
        if (recomp_dsound_lock_stats_take) recomp_dsound_lock_stats_take(&wait_ns, &waits, &pump_ns, &pump_max_ns, &passes);
        std::fprintf(stderr,
            "recomp profile: fibers:%s | math=%.2fms(%llu) | dsound lock wait=%.2fms(%llu) pump=%.2fms/s max=%.2fms passes=%llu\n",
            fused ? fibers : " -", math_ticks * tick_ms * per_frame, (unsigned long long)(math_calls / frames),
            wait_ns / 1e6 * per_frame, (unsigned long long)(waits / frames), pump_ns / 1e6 * (1000.0 / wall_ms),
            pump_max_ns / 1e6, (unsigned long long)passes);
        math_calls = math_ticks = 0;
    }

    std::fprintf(stderr,
        "recomp profile: frame=%.2fms game=%.2f adapters=%.2f kernel=%.2f lookup=%.2f(%llu) idle=%.2f lost=%llu top:%s\n",
        wall_ms * per_frame, game_ms * per_frame, adapter_ms * per_frame, kernel_ms * per_frame,
        lookup_ms * per_frame, (unsigned long long)(lookup_calls / frames), idle_ms * per_frame,
        (unsigned long long)lost_scopes, top);

    if (console_row < 0) {
        for (int i = 0; i < slot_count; ++i) {
            slots[i].ticks = 0;
            slots[i].calls = 0;
        }
        idle_ticks = lookup_ticks = lookup_calls = 0;
        return;
    }
    std::printf("\x1b[%d;1H", console_row);
    std::printf("Por frame:  juego %5.2f ms   adaptadores %5.2f ms\x1b[K\n",
        game_ms * per_frame, adapter_ms * per_frame);
    std::printf("            kernel %5.2f ms  busquedas %5.2f ms (%llu)\x1b[K\n",
        kernel_ms * per_frame, lookup_ms * per_frame, (unsigned long long)(lookup_calls / frames));
    std::printf("            espera %5.2f ms\x1b[K\n", idle_ms * per_frame);
    std::printf("Mas costosos:\x1b[K\n");
    for (int i = 0; i < 6; ++i) {
        if (i < slot_count && slots[order[i]].ticks != 0) {
            const Slot &s = slots[order[i]];
            std::printf("  %s 0x%08x  %5.2f ms  %6llu llamadas\x1b[K\n",
                s.kind == KIND_KERNEL ? "kernel " : "adapter", (unsigned)s.address,
                s.ticks * tick_ms * per_frame, (unsigned long long)(s.calls / frames));
        } else {
            std::printf("\x1b[K\n");
        }
    }

    for (int i = 0; i < slot_count; ++i) {
        slots[i].ticks = 0;
        slots[i].calls = 0;
    }
    idle_ticks = lookup_ticks = lookup_calls = 0;
}

}
