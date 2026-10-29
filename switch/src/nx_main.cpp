// SPDX-License-Identifier: GPL-3.0-or-later
// Nintendo Switch entry point. runner.cpp is compiled with its main renamed to
// recomp_runner_main; this file sets up the console, the log file and the
// paths on the SD card, then starts the runner as tools/run_game.py does:
// recomp_program_runner --xbe <disc>/<game>.xbe --disc-root <disc>.
//
// SD card layout:
//   sdmc:/switch/doa2u-nx/doa2u-nx.nro
//   sdmc:/switch/doa2u-nx/disc/            (the whole extracted Disc 2, USA)
//   sdmc:/switch/doa2u-nx/doa2u-nx.log     (runtime log, rewritten each boot)
// The game executable is picked among disc/*.xbe by its SHA-256, as
// build_game.py does. Saves and the hard-disk cache live in
// disc/.recomp-storage, as private/play-disc does on PC.
#include <switch.h>

#include <fcntl.h>
#include <strings.h>
#include <sys/iosupport.h>
#include <sys/stat.h>
#include <unistd.h>

#include <pthread.h>

#include <dirent.h>

#include <cerrno>
#include <cstdio>
#include <exception>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

#include "xbe_parser.h"

#ifndef DOA2U_NX_VERSION
#define DOA2U_NX_VERSION "0.1.0"
#endif

int recomp_runner_main(int argc, char **argv); // runner.cpp main, renamed at compile time
extern "C" void __real_exit(int code) __attribute__((noreturn));

extern "C" {
static void flushLog(void); // below, with the log devoptab
}

namespace {

const char kRoot[] = "sdmc:/switch/doa2u-nx";
const char kDisc[] = "sdmc:/switch/doa2u-nx/disc";
const char kLog[] = "sdmc:/switch/doa2u-nx/doa2u-nx.log";
// The runner gets paths without the "sdmc:" device prefix (sdmc is the
// default device): win32_compat.c resolves guest file names case-insensitively
// one "/" component at a time, which needs the path to start at "/".
const char kRunnerDisc[] = "/switch/doa2u-nx/disc";

// The disc archives the game installs to its hard-disk cache (Z:) on first
// boot, as tools/run_game.py's seed_hdd_cache copies them: the game's own
// installer streams them 32 KB at a time and holds the boot for minutes; with
// matching sizes its install check accepts the cache at once. Copies, not
// links: the installer reopens a stale file with CREATE_ALWAYS.
const char *const kHddArchives[] = {"voice.afs", "datahdd.afs", "datahdd2.afs"};

bool console_ready = false;
PrintConsole *console_state = nullptr; // the live console, for cursor rows

bool hasXbeSuffix(const char *name)
{
    const size_t length = std::strlen(name);
    return length > 4 && strcasecmp(name + length - 4, ".xbe") == 0;
}

// The executable build_game.py authenticated, found among disc/*.xbe by its
// SHA-256 (RECOMP_XBE_SHA256, the same pin runner.cpp checks). Empty when no
// file matches; mismatched names are listed for the screen.
std::string findGameXbe(std::vector<std::string> &rejected)
{
    DIR *dir = opendir(kDisc);
    if (dir == nullptr) return {};
    std::vector<std::string> names;
    while (const dirent *entry = readdir(dir)) {
        if (hasXbeSuffix(entry->d_name)) names.emplace_back(entry->d_name);
    }
    closedir(dir);
    std::sort(names.begin(), names.end());
    for (const std::string &name : names) {
        const std::string path = std::string(kDisc) + "/" + name;
        std::printf("Comprobando %s...\n", name.c_str());
        consoleUpdate(nullptr);
        const auto parsed = doaxbv::XbeParser().parse(std::filesystem::u8path(path));
        if (parsed.ok() && parsed.metadata.fileSha256 == RECOMP_XBE_SHA256) return name;
        rejected.push_back(name);
    }
    return {};
}

long long fileSize(const std::string &path)
{
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode) ? static_cast<long long>(st.st_size) : -1;
}

// Copies one archive through a .tmp name, drawing a progress bar on line row.
bool copyWithProgress(const std::string &source, const std::string &target, int row)
{
    const long long total = fileSize(source);
    FILE *in = std::fopen(source.c_str(), "rb");
    if (in == nullptr || total < 0) {
        if (in) std::fclose(in);
        return false;
    }
    const std::string partial = target + ".tmp";
    FILE *out = std::fopen(partial.c_str(), "wb");
    if (out == nullptr) {
        std::fclose(in);
        return false;
    }
    std::vector<char> buffer(1u << 20);
    long long done = 0;
    int shown = -1;
    bool ok = true;
    for (;;) {
        const size_t got = std::fread(buffer.data(), 1, buffer.size(), in);
        if (got == 0) break;
        if (std::fwrite(buffer.data(), 1, got, out) != got) {
            ok = false;
            break;
        }
        done += static_cast<long long>(got);
        const int percent = total > 0 ? static_cast<int>(done * 100 / total) : 100;
        if (percent != shown) {
            shown = percent;
            char bar[41];
            const int filled = percent * 40 / 100;
            for (int i = 0; i < 40; ++i) bar[i] = i < filled ? '#' : '.';
            bar[40] = '\0';
            std::printf("\x1b[%d;1H  [%s] %3d%%\x1b[K", row, bar, percent);
            consoleUpdate(nullptr);
        }
    }
    ok = ok && std::ferror(in) == 0;
    std::fclose(in);
    ok = std::fclose(out) == 0 && ok && done == total;
    if (ok) {
        std::remove(target.c_str());
        ok = std::rename(partial.c_str(), target.c_str()) == 0;
    }
    if (!ok) std::remove(partial.c_str());
    return ok;
}

// tools/run_game.py seed_hdd_cache: copy disc/doa2/<archive> into
// disc/.recomp-storage/partition5 unless a copy of the same size is there.
bool seedHddCache()
{
    const std::string storage = std::string(kDisc) + "/.recomp-storage";
    const std::string cache = storage + "/partition5";
    bool announced = false;
    for (const char *name : kHddArchives) {
        const std::string source = std::string(kDisc) + "/doa2/" + name;
        const std::string target = cache + "/" + name;
        const long long size = fileSize(source);
        if (size < 0 || fileSize(target) == size) continue;
        if (!announced) {
            std::printf("\nPrimer arranque: copiando los archivos que el juego\n");
            std::printf("instala en su disco duro (unos 700 MB). Solo esta vez.\n");
            announced = true;
        }
        mkdir(storage.c_str(), 0777);
        mkdir(cache.c_str(), 0777);
        std::printf("\n%s (%lld MB)\n", name, size >> 20);
        consoleUpdate(nullptr);
        std::fprintf(stderr, "[nx] seeding hdd cache %s (%lld bytes)\n", name, size);
        int row = 1;
        if (console_state != nullptr) row = console_state->cursorY;
        if (!copyWithProgress(source, target, row)) {
            std::printf("\n\x1b[31mNo se pudo copiar %s (errno %d).\x1b[0m\n", name, errno);
            std::printf("Comprueba el espacio libre en la SD.\n");
            std::fprintf(stderr, "[nx] hdd cache copy failed: %s errno=%d\n", name, errno);
            return false;
        }
        std::printf("\n");
    }
    return true;
}

void waitForPlus()
{
    PadState pad;
    padInitializeDefault(&pad);
    std::printf("\nPulsa (+) para salir.\n");
    consoleUpdate(nullptr);
    while (appletMainLoop()) {
        padUpdate(&pad);
        if (padGetButtonsDown(&pad) & HidNpadButton_Plus) break;
        consoleUpdate(nullptr);
        svcSleepThread(16'000'000);
    }
}

// The last lines of the log, so a stop is readable on the console itself.
void printLogTail(int lines)
{
    std::fflush(stderr);
    flushLog();
    FILE *file = std::fopen(kLog, "rb");
    if (file == nullptr) return;
    std::fseek(file, 0, SEEK_END);
    const long size = std::ftell(file);
    const long start = size > 4096 ? size - 4096 : 0;
    std::fseek(file, start, SEEK_SET);
    std::string text(static_cast<size_t>(size - start), '\0');
    text.resize(std::fread(text.data(), 1, text.size(), file));
    std::fclose(file);
    size_t pos = text.size();
    int found = 0;
    while (pos > 0 && found <= lines) {
        pos = text.rfind('\n', pos - 1);
        if (pos == std::string::npos) { pos = 0; break; }
        ++found;
    }
    std::printf("\n--- final del log ---\n%s\n", text.c_str() + (pos ? pos + 1 : 0));
}

} // namespace

extern "C" {

// stderr goes to the log file at the devoptab level, so every thread's
// stderr stream (each newlib thread has its own) lands in the same file,
// while stdout stays on the on-screen console.
static int log_fd = -1;

// The last complete log line, shown on screen while the game boots.
static char last_line[96];
static char line_build[96];
static size_t line_length;

static void trackLastLine(const char *ptr, size_t len)
{
    for (size_t i = 0; i < len; ++i) {
        const char c = ptr[i];
        if (c == '\n') {
            line_build[line_length] = '\0';
            if (line_length != 0) std::memcpy(last_line, line_build, line_length + 1);
            line_length = 0;
        } else if (line_length + 1 < sizeof line_build && c >= ' ') {
            line_build[line_length++] = c;
        }
    }
}

// The runtime logs every guest file read and write; one SD write per line
// would cost the guest real time. Lines collect in memory instead and a
// helper thread on another core writes them out four times a second (and
// whenever the buffer fills, or before exit and crash reports).
static Mutex log_mutex;
static char log_buffer[256 * 1024];
static size_t log_used;
static Thread log_thread;
static volatile bool log_thread_running;

static void flushLogLocked()
{
    if (log_fd >= 0 && log_used != 0) write(log_fd, log_buffer, log_used);
    log_used = 0;
}

static void flushLog()
{
    mutexLock(&log_mutex);
    flushLogLocked();
    mutexUnlock(&log_mutex);
}

static void logFlusher(void *arg)
{
    (void)arg;
    while (log_thread_running) {
        svcSleepThread(250'000'000);
        flushLog();
    }
}

static ssize_t logWrite(struct _reent *r, void *fd, const char *ptr, size_t len)
{
    (void)r;
    (void)fd;
    mutexLock(&log_mutex);
    trackLastLine(ptr, len);
    if (log_fd >= 0) {
        if (log_used + len > sizeof log_buffer) flushLogLocked();
        if (len > sizeof log_buffer) {
            write(log_fd, ptr, len);
        } else {
            std::memcpy(log_buffer + log_used, ptr, len);
            log_used += len;
        }
    }
    mutexUnlock(&log_mutex);
    return (ssize_t)len;
}

static devoptab_t log_devoptab;

static void redirectStderrToLog()
{
    mutexInit(&log_mutex);
    log_fd = open(kLog, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (log_fd < 0) return;
    log_thread_running = true;
    if (R_SUCCEEDED(threadCreate(&log_thread, logFlusher, nullptr, nullptr, 0x8000, 0x2D, 2))) {
        threadStart(&log_thread);
    }
    std::memset(&log_devoptab, 0, sizeof log_devoptab);
    log_devoptab.name = "doa2ulog";
    log_devoptab.write_r = logWrite;
    devoptab_list[STD_ERR] = &log_devoptab;
}

void doa2u_nx_log_raw(const char *text, size_t length)
{
    // Crash path: the faulting thread may hold log_mutex, so no locking.
    if (log_fd >= 0) {
        if (log_used != 0) write(log_fd, log_buffer, log_used);
        log_used = 0;
        write(log_fd, text, length);
    }
}

bool doa2u_nx_console_ready(void)
{
    return console_ready;
}

// libnx calls these around main.
void userAppInit(void)
{
    // hid is already up (libnx default); configure pads before anyone reads them.
    padConfigureInput(4, HidNpadStyleSet_NpadStandard);
    mkdir(kRoot, 0777);
}

void userAppExit(void)
{
    std::fflush(stderr);
    if (log_fd >= 0) {
        flushLog();
        fsync(log_fd);
        close(log_fd);
        log_fd = -1;
    }
    if (console_ready) {
        consoleExit(nullptr);
        console_ready = false;
    }
}

// host_diagnostics.cpp gives stderr a 64 KiB buffer and flushes it from a
// helper thread. newlib streams are per thread, so that thread never flushes
// the main thread's stderr; line buffering keeps the log current instead.
int __real_setvbuf(FILE *stream, char *buffer, int mode, size_t size);
int __wrap_setvbuf(FILE *stream, char *buffer, int mode, size_t size)
{
    if (stream == stderr && mode == _IOFBF) mode = _IOLBF;
    return __real_setvbuf(stream, buffer, mode, size);
}

// libnx starts every pthread (std::thread included) on the main thread's
// core. The guest runs on the main thread (core 0) and never yields, so host
// threads move off it: the render thread asks for core 1, which it then has to
// itself, and the rest (DirectSound's voice pump) join the audio mixer on core 2.
static int next_thread_core = -1;

void doa2u_nx_set_next_thread_core(int core)
{
    next_thread_core = core;
}

int __real_pthread_create(pthread_t *thread, const pthread_attr_t *attr,
    void *(*start)(void *), void *arg);
int __wrap_pthread_create(pthread_t *thread, const pthread_attr_t *attr,
    void *(*start)(void *), void *arg)
{
    const int rc = __real_pthread_create(thread, attr, start, arg);
    const int requested = next_thread_core;
    next_thread_core = -1;
    if (rc != 0 || thread == nullptr) return rc;
    u64 mask = 0;
    if (R_FAILED(svcGetInfo(&mask, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0))) return rc;
    unsigned core = requested >= 0 ? static_cast<unsigned>(requested) : 2u;
    if (!(mask & (1ull << core))) core = (mask & 2ull) ? 1u : 0u;
    // pthread_t is libnx's struct __pthread_t*, whose first member is a Thread
    // and whose first field is the kernel handle.
    const Handle handle = *reinterpret_cast<const Handle *>(*thread);
    svcSetThreadCoreMask(handle, static_cast<s32>(core), 1u << core);
    std::fprintf(stderr, "[nx] host thread on core %u\n", core);
    return rc;
}

// Until the presenter takes the screen, a heartbeat shows the boot is alive.
static Mutex console_mutex;
static bool presenter_owns_console = false;

void doa2u_nx_presenter_took_console(void)
{
    mutexLock(&console_mutex);
    presenter_owns_console = true;
    mutexUnlock(&console_mutex);
}

// The OpenGL presenter needs the window the text console draws to.
void doa2u_nx_release_console(void)
{
    mutexLock(&console_mutex);
    presenter_owns_console = true;
    if (console_ready) {
        consoleExit(nullptr);
        console_ready = false;
    }
    mutexUnlock(&console_mutex);
    std::fprintf(stderr, "[nx] text console released for the renderer\n");
}

static Thread heartbeat_thread;
uint64_t doa2u_nx_kernel_calls(void); // boot_trace.cpp
void doa2u_nx_profile_start(void);     // profile.cpp

static void heartbeat(void *arg)
{
    (void)arg;
    const u64 start = armGetSystemTick();
    for (;;) {
        svcSleepThread(500'000'000);
        mutexLock(&console_mutex);
        if (presenter_owns_console || !console_ready) {
            mutexUnlock(&console_mutex);
            return;
        }
        const double seconds = armTicksToNs(armGetSystemTick() - start) / 1e9;
        char line[sizeof last_line];
        std::memcpy(line, last_line, sizeof line);
        line[sizeof line - 1] = '\0';
        std::printf("\x1b[6;1HArrancando... %5.1f s   llamadas al kernel: %llu\x1b[K\n\x1b[K%.78s\n",
            seconds, (unsigned long long)doa2u_nx_kernel_calls(), line);
        consoleUpdate(nullptr);
        mutexUnlock(&console_mutex);
    }
}

// exit() is wrapped at link time (-Wl,--wrap=exit): a runtime stop with a
// nonzero code stays on screen instead of dropping back to the menu.
void __wrap_exit(int code)
{
    if (code != 0 && console_ready) {
        std::printf("\n\x1b[31mEl juego se ha detenido (codigo %d).\x1b[0m\n", code);
        std::printf("Log completo: %s\n", kLog);
        printLogTail(12);
        waitForPlus();
    }
    __real_exit(code);
}

} // extern "C"

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    console_state = consoleInit(nullptr);
    console_ready = true;
    redirectStderrToLog();
    std::fprintf(stderr, "DOA2U NX %s (Toniisound), based on NoRain211/doa2u-re\n",
        DOA2U_NX_VERSION);

    std::printf("DOA2U NX %s\n\n", DOA2U_NX_VERSION);
    std::vector<std::string> rejected;
    const std::string xbe = findGameXbe(rejected);
    if (xbe.empty()) {
        std::printf("\nNo se encuentra el ejecutable del juego en %s/\n\n", kDisc);
        std::printf("Copia ahi el contenido del Disco 2 USA de Dead or Alive\n");
        std::printf("Ultimate (Xbox), extraido con extract-xiso.\n");
        for (const std::string &name : rejected) {
            std::printf("  %s: no es la version USA esperada\n", name.c_str());
        }
        std::fprintf(stderr, "[nx] no authenticated xbe in %s (%zu rejected)\n", kDisc, rejected.size());
        waitForPlus();
        consoleExit(nullptr);
        console_ready = false;
        return 0;
    }
    std::fprintf(stderr, "[nx] game executable: %s\n", xbe.c_str());
    if (!seedHddCache()) {
        waitForPlus();
        consoleExit(nullptr);
        console_ready = false;
        return 1;
    }

    // Held at boot: R runs unpaced (as fast as the CPU allows, to measure
    // headroom above 60 fps); L keeps the original 4:3 picture instead of the
    // game's own 16:9 mode; Y skips the intro movies. There is no CPU boost:
    // boost mode would drop the GPU to 76.8 MHz. Clock the console with
    // sys-clk instead.
    PadState pad;
    padInitializeDefault(&pad);
    padUpdate(&pad);
    const u64 held = padGetButtons(&pad);
    const bool unpaced = (held & HidNpadButton_R) != 0;
    const bool narrow = (held & HidNpadButton_L) != 0;
    const bool skip_intro = (held & HidNpadButton_Y) != 0;
    if (unpaced) {
        setenv("RECOMP_UNPACED", "1", 1);
        std::fprintf(stderr, "[nx] RECOMP_UNPACED=1 (R held at boot)\n");
    }
    // The kernel's XC_VIDEO and XC_AUDIO settings follow these, as the
    // launcher sets them on PC. The console outputs stereo.
    setenv("RECOMP_D3D_WIDESCREEN", narrow ? "0" : "1", 1);
    unsetenv("RECOMP_AUDIO_SURROUND");
    std::printf("\nModo: %s, %s%s\n", unpaced ? "sin limite (unpaced)" : "60 Hz",
        narrow ? "4:3" : "16:9", skip_intro ? ", sin videos de intro" : "");
    std::printf("Arrancando...\n");
    consoleUpdate(nullptr);
    mutexInit(&console_mutex);
    if (R_SUCCEEDED(threadCreate(&heartbeat_thread, heartbeat, nullptr, nullptr,
            0x10000, 0x2D, 1))) {
        threadStart(&heartbeat_thread);
    }

    chdir(kRoot);
    const std::string runner_xbe = std::string(kRunnerDisc) + "/" + xbe;
    std::vector<std::string> arguments = {"doa2u-nx", "--xbe", runner_xbe, "--disc-root", kRunnerDisc};
    if (skip_intro) arguments.emplace_back("--skip-intro");
    std::vector<char *> runner_argv;
    for (std::string &argument : arguments) runner_argv.push_back(argument.data());
    runner_argv.push_back(nullptr);
    doa2u_nx_profile_start();
    int code = 1;
    try {
        code = recomp_runner_main(static_cast<int>(arguments.size()), runner_argv.data());
    } catch (const std::exception &error) {
        std::fprintf(stderr, "[nx] uncaught exception: %s\n", error.what());
        std::printf("\n\x1b[31mExcepcion: %s\x1b[0m\n", error.what());
        waitForPlus();
        return 1;
    }
    std::fprintf(stderr, "[nx] runner returned %d\n", code);
    return code;
}
