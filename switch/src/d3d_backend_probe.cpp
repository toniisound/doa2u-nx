// SPDX-License-Identifier: GPL-3.0-or-later
// Nintendo Switch, first bring-up stage: a presenter backend that accepts
// every command but draws nothing. It answers the question that decides how
// much GPU work the port can afford: how fast does the lifted game run on the
// console's CPU? Once a second it prints the guest frame rate, draws per
// frame and the time the guest spent between presents on the libnx console.
//
// It implements the backend seam (d3d_presenter_backend.h) that the D3D11 and
// SDL_GPU presenters implement, under upstream's d3d_presenter_inline.cpp, so
// the OpenGL backend of stage 2 drops in beside it. Frame pacing stays where
// upstream has it, in d3d8_adapter.c (d3d_vblank.cpp).
//
// Visibility tests have no rasterizer to count samples here, so each one
// resolves at once as visible; draw coverage answers "covered", which is
// what the real backends report on error.
#include "d3d_presenter_backend.h"

#include <switch.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <new>

extern "C" void doa2u_nx_presenter_took_console(void); // nx_main.cpp
extern "C" void doa2u_nx_profile_report(uint32_t frames, double wall_ms, int console_row); // profile.cpp
extern "C" void doa2u_nx_profile_note_present(void); // profile.cpp

struct RecompD3dPresenter {
    uint32_t width = 0;
    uint32_t height = 0;
    uint64_t present_count = 0;
    uint64_t clear_count = 0;
    uint64_t draw_count = 0;
    uint64_t textured_draw_count = 0;
    uint64_t program_draw_count = 0;
    uint64_t visibility_count = 0;
    uint64_t copy_count = 0;
    uint32_t second_presents = 0;
    uint64_t second_draws = 0;
    double second_max_ms = 0.0;
    std::chrono::steady_clock::time_point second_start{};
    std::chrono::steady_clock::time_point last_present{};
    std::chrono::steady_clock::time_point boot{};
    std::deque<uint32_t> ended_visibility;
    PadState pad{};
    bool close_requested = false;
};

namespace {

/* A visible result, in guest target pixels: the game reads these for lens
   flares and similar effects; without a rasterizer, "fully visible" is the
   answer that keeps them drawing. */
constexpr uint32_t kVisiblePixels = 64u * 64u;

bool immediate_present_setting = false;

void printHeader(const RecompD3dPresenter *p)
{
    std::printf("\x1b[2J\x1b[1;1H");
    std::printf("DOA2U NX - fase 1: prueba de rendimiento de CPU\n");
    std::printf("===============================================\n\n");
    std::printf("El juego corre sin graficos (con sonido): se mide\n");
    std::printf("a cuantos fotogramas por segundo avanza (objetivo: 60).\n\n");
    std::printf("Resolucion del juego: %ux%u\n", p->width, p->height);
    std::printf("Salir: HOME > cerrar, o ZL+ZR+(-)+(+) a la vez\n\n");
    consoleUpdate(nullptr);
}

void pollHost(RecompD3dPresenter *p)
{
    if (!appletMainLoop()) {
        p->close_requested = true;
        return;
    }
    padUpdate(&p->pad);
    const u64 held = padGetButtons(&p->pad);
    const u64 combo = HidNpadButton_ZL | HidNpadButton_ZR |
        HidNpadButton_Minus | HidNpadButton_Plus;
    if ((held & combo) == combo) p->close_requested = true;
}

void reportPerformance(RecompD3dPresenter *p)
{
    using Clock = std::chrono::steady_clock;
    const auto now = Clock::now();
    if (p->second_presents++ == 0u) {
        p->second_start = now;
    } else {
        p->second_max_ms = std::max(p->second_max_ms,
            std::chrono::duration<double, std::milli>(now - p->last_present).count());
    }
    p->last_present = now;
    const double elapsed_ms =
        std::chrono::duration<double, std::milli>(now - p->second_start).count();
    if (elapsed_ms < 1000.0) return;

    const uint32_t frames = p->second_presents - 1u;
    const double fps = frames * 1000.0 / elapsed_ms;
    const double uptime = std::chrono::duration<double>(now - p->boot).count();
    const uint64_t draws = frames ? p->second_draws / frames : 0u;
    std::fprintf(stderr,
        "recomp performance: present=%llu fps=%.2f frame_ms=%.3f max_ms=%.1f draws=%llu\n",
        (unsigned long long)p->present_count, fps, elapsed_ms / frames,
        p->second_max_ms, (unsigned long long)draws);
    std::printf("\x1b[11;1H");
    std::printf("Tiempo: %7.1f s   Fotogramas: %-10llu\x1b[K\n",
        uptime, (unsigned long long)p->present_count);
    std::printf("FPS: %6.2f   (%.1f ms/frame, peor %.1f ms)\x1b[K\n",
        fps, elapsed_ms / frames, p->second_max_ms);
    std::printf("Dibujos por frame: %llu\x1b[K\n", (unsigned long long)draws);
    std::printf("Total dibujos: %llu (texturizados %llu, vertex program %llu)\x1b[K\n",
        (unsigned long long)p->draw_count,
        (unsigned long long)p->textured_draw_count,
        (unsigned long long)p->program_draw_count);
    doa2u_nx_profile_report(frames, elapsed_ms, 16);
    consoleUpdate(nullptr);

    p->second_presents = 1u;
    p->second_start = now;
    p->second_max_ms = 0.0;
    p->second_draws = 0u;
}

} // namespace

RecompD3dPresenterError d3d_backend_create(
    const RecompD3dPresenterConfig *config, RecompD3dPresenter **presenter)
{
    if (config == nullptr || presenter == nullptr || config->width == 0u || config->height == 0u) {
        return RECOMP_D3D_PRESENTER_INVALID_ARGUMENT;
    }
    auto *p = new (std::nothrow) RecompD3dPresenter;
    if (p == nullptr) return RECOMP_D3D_PRESENTER_OUT_OF_MEMORY;
    p->width = config->width;
    p->height = config->height;
    p->boot = std::chrono::steady_clock::now();
    padInitializeDefault(&p->pad);
    doa2u_nx_presenter_took_console();
    printHeader(p);
    std::fprintf(stderr, "[presenter] Switch probe backend %ux%u (no rendering)\n",
        p->width, p->height);
    *presenter = p;
    return RECOMP_D3D_PRESENTER_OK;
}

RecompD3dPresenterError d3d_backend_submit(
    RecompD3dPresenter *presenter, const RecompD3dPresenterCommand *command)
{
    if (presenter == nullptr) return RECOMP_D3D_PRESENTER_NOT_INITIALIZED;
    // A close reaches the guest at the next submission and stays.
    if (presenter->close_requested) return RECOMP_D3D_PRESENTER_CLOSED;
    if (command == nullptr) return RECOMP_D3D_PRESENTER_INVALID_ARGUMENT;

    switch (command->type) {
    case RECOMP_D3D_PRESENTER_COMMAND_CLEAR:
        ++presenter->clear_count;
        return RECOMP_D3D_PRESENTER_OK;
    case RECOMP_D3D_PRESENTER_COMMAND_DRAW: {
        const RecompD3dPresenterDrawCommand &draw = command->data.draw;
        ++presenter->draw_count;
        ++presenter->second_draws;
        if (draw.has_texture) ++presenter->textured_draw_count;
        if (draw.program_count != 0u) ++presenter->program_draw_count;
        return RECOMP_D3D_PRESENTER_OK;
    }
    case RECOMP_D3D_PRESENTER_COMMAND_GAMMA:
    case RECOMP_D3D_PRESENTER_COMMAND_VISIBILITY_BEGIN:
    case RECOMP_D3D_PRESENTER_COMMAND_DRAW_COVERAGE:
        return RECOMP_D3D_PRESENTER_OK;
    case RECOMP_D3D_PRESENTER_COMMAND_VISIBILITY_END:
        ++presenter->visibility_count;
        try {
            presenter->ended_visibility.push_back(command->data.visibility_index);
        } catch (const std::bad_alloc &) {
            return RECOMP_D3D_PRESENTER_OUT_OF_MEMORY;
        }
        return RECOMP_D3D_PRESENTER_OK;
    case RECOMP_D3D_PRESENTER_COMMAND_COPY_MAIN_COLOR:
        ++presenter->copy_count;
        return RECOMP_D3D_PRESENTER_OK;
    case RECOMP_D3D_PRESENTER_COMMAND_PRESENT:
        ++presenter->present_count;
        doa2u_nx_profile_note_present();
        pollHost(presenter);
        reportPerformance(presenter);
        return presenter->close_requested ? RECOMP_D3D_PRESENTER_CLOSED
                                          : RECOMP_D3D_PRESENTER_OK;
    }
    return RECOMP_D3D_PRESENTER_UNSUPPORTED_COMMAND;
}

RecompD3dPresenterError d3d_backend_draw_coverage(
    RecompD3dPresenter *presenter, const RecompD3dPresenterDrawCommand *draw, bool *covered)
{
    if (covered != nullptr) *covered = true;
    if (presenter == nullptr) return RECOMP_D3D_PRESENTER_NOT_INITIALIZED;
    if (draw == nullptr || covered == nullptr) return RECOMP_D3D_PRESENTER_INVALID_ARGUMENT;
    return RECOMP_D3D_PRESENTER_OK;
}

RecompD3dPresenterError d3d_backend_release_memory(
    RecompD3dPresenter *presenter, uint32_t base, uint32_t size, bool targets)
{
    (void)base;
    (void)size;
    (void)targets;
    return presenter == nullptr ? RECOMP_D3D_PRESENTER_NOT_INITIALIZED : RECOMP_D3D_PRESENTER_OK;
}

RecompD3dPresenterError d3d_backend_destroy(RecompD3dPresenter **presenter)
{
    if (presenter == nullptr || *presenter == nullptr) return RECOMP_D3D_PRESENTER_INVALID_ARGUMENT;
    const RecompD3dPresenter *p = *presenter;
    std::fprintf(stderr,
        "[presenter] probe summary presents=%llu draws=%llu clears=%llu visibility=%llu copies=%llu\n",
        (unsigned long long)p->present_count, (unsigned long long)p->draw_count,
        (unsigned long long)p->clear_count, (unsigned long long)p->visibility_count,
        (unsigned long long)p->copy_count);
    delete *presenter;
    *presenter = nullptr;
    return RECOMP_D3D_PRESENTER_OK;
}

void d3d_backend_set_immediate_present(bool enabled)
{
    immediate_present_setting = enabled;
}

void d3d_backend_report_draw_textures() {}

D3dBackendVisibilityPoll d3d_backend_poll_visibility(
    RecompD3dPresenter *presenter, uint32_t *index, uint32_t *pixels)
{
    if (presenter == nullptr || index == nullptr || pixels == nullptr) {
        return D3D_BACKEND_VISIBILITY_FAILED;
    }
    if (presenter->ended_visibility.empty()) return D3D_BACKEND_VISIBILITY_IDLE;
    *index = presenter->ended_visibility.front();
    presenter->ended_visibility.pop_front();
    *pixels = kVisiblePixels;
    return D3D_BACKEND_VISIBILITY_READY;
}
