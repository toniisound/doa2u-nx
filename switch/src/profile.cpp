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
    current_stack = stackFor(target);
    __real_recomp_native_fiber_switch(target);
    current_stack = own;
    const uint64_t away = now() - out;
    if (own->depth != 0) own->scopes[own->depth - 1].child += away;
}

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
