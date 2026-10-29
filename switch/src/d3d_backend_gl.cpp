// SPDX-License-Identifier: GPL-3.0-or-later
/* OpenGL 4.3 core presenter backend: the Nintendo Switch renderer of DOA2U NX
   (devkitPro mesa, nouveau), behind the same d3d_presenter_backend.h seam as
   upstream's Direct3D 11 and SDL_GPU backends.

   It follows recomp-runtime/d3d_presenter_sdl_gpu.cpp decision for decision:
   the same draw shader and constants (d3d_draw_shader.cpp, compiled to GLSL
   by d3d_shader_compiler.cpp), texture cache and fingerprints, target sizes,
   declines, CPU line widening and CPU coverage probes. Where OpenGL differs
   this file says so:
   - Clip control is UPPER_LEFT / ZERO_TO_ONE, so D3D clip space, viewports,
     scissors and texture rows map as they do on the Xbox: row 0 is the top of
     every target. The final present pass flips into the window.
   - Visibility tests use GL_SAMPLES_PASSED queries instead of SDL_GPU's
     additive counting pass.
   - Triangle fans draw natively; SDL_GPU expands them into lists.
   - MSAA and SMAA are not offered (the Switch build renders single-sampled).

   The same file builds for desktop Linux with an EGL pbuffer (DOA2U_NX_HOST),
   so the backend can be exercised by host tests on Mesa's llvmpipe. */
#include "d3d_presenter_backend.h"
#include "d3d_draw_model.h"
#include "d3d_draw_shader.h"
#include "d3d_shader_compiler.h"
#include "d3d_texture_model.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <glad/glad.h>
#ifdef __SWITCH__
#include <switch.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <vector>

#ifndef GL_UPPER_LEFT
#define GL_UPPER_LEFT 0x8CA2
#endif
#ifndef GL_LOWER_LEFT
#define GL_LOWER_LEFT 0x8CA1
#endif
#ifndef GL_ZERO_TO_ONE
#define GL_ZERO_TO_ONE 0x935F
#endif
#ifndef GL_COMPRESSED_RGBA_S3TC_DXT1_EXT
#define GL_COMPRESSED_RGBA_S3TC_DXT1_EXT 0x83F1
#define GL_COMPRESSED_RGBA_S3TC_DXT3_EXT 0x83F2
#define GL_COMPRESSED_RGBA_S3TC_DXT5_EXT 0x83F3
#endif
#ifndef GL_TEXTURE_LOD_BIAS
#define GL_TEXTURE_LOD_BIAS 0x8501
#endif

#ifdef __SWITCH__
extern "C" void doa2u_nx_release_console(void); // nx_main.cpp
#endif

/* What the render thread is doing, for the Switch stall watcher (profile.cpp):
   a stuck GL call shows up by name. Set without locks; read racily. */
namespace {
std::atomic<const char *> render_stage{"idle"};
std::atomic<uint64_t> render_stage_since_ms{0};
std::atomic<uint64_t> render_stage_count{0};

void stage(const char *name)
{
    render_stage.store(name, std::memory_order_relaxed);
    render_stage_since_ms.store(static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count()), std::memory_order_relaxed);
    render_stage_count.fetch_add(1u, std::memory_order_relaxed);
}
} // namespace

extern "C" const char *doa2u_nx_render_stage(uint64_t *since_ms, uint64_t *count)
{
    if (since_ms) *since_ms = render_stage_since_ms.load(std::memory_order_relaxed);
    if (count) *count = render_stage_count.load(std::memory_order_relaxed);
    return render_stage.load(std::memory_order_relaxed);
}

namespace {

constexpr uint32_t kTextureSlots = 4096u;
constexpr uint32_t kPaletteBytes = 256u * 4u;
constexpr uint64_t kTargetByteLimit = 64u * 1024u * 1024u;
constexpr uint32_t kMaxTextureSize = 16384u;
constexpr uint32_t kMaxVolumeSize = 2048u;
constexpr float kMipLodBiasMin = -16.0f, kMipLodBiasMax = 15.99f;
constexpr uint32_t kProgramFloats = 192u * 4u;
constexpr uint32_t kTransformFloats = kRecompD3dDrawConstantFloats - kProgramFloats;
constexpr uint32_t kVertexRingBytes = 32u * 1024u * 1024u;
constexpr uint32_t kIndexRingBytes = 8u * 1024u * 1024u;
constexpr uint32_t kUniformRingBytes = 16u * 1024u * 1024u;
/* Uniform buffer bindings (d3d_shader_compiler.h): vertex slot n is binding n,
   fragment slot n is binding n + 2. */
constexpr GLuint kVertexTransformBinding = 0u, kVertexProgramBinding = 1u;
constexpr GLuint kFragmentTransformBinding = 2u, kFragmentProgramBinding = 3u;
constexpr GLuint kGammaBinding = 4u;

/* With UPPER_LEFT clip control GL still breaks rasterization ties in its own
   lower-left frame, so an edge exactly on a row of pixel centres (common:
   upstream maps D3D's integer pixel centres onto host centres) lands one row
   lower than D3D's top-left rule puts it. Lifting every viewport by a small
   fraction of a pixel restores D3D's choice without moving anything else;
   columns already match. */
constexpr float kRowTieBias = 1.0f / 256.0f;

using ClipControlFn = void (*)(GLenum origin, GLenum depth);
ClipControlFn clipControl = nullptr;

bool envFlag(const char *name)
{
    const char *value = std::getenv(name);
    return value != nullptr && std::strcmp(value, "0") != 0 && value[0] != '\0';
}

uint64_t hashBytes(const void *bytes, size_t count, uint64_t hash = 0xcbf29ce484222325ull)
{
    const auto *data = static_cast<const uint8_t *>(bytes);
    for (size_t i = 0u; i < count; ++i) hash = (hash ^ data[i]) * 0x100000001b3ull;
    return hash;
}

/* As upstream: small spans hash every byte, larger ones sample 128 words. */
uint64_t textureFingerprint(const void *bytes, uint32_t count)
{
    const auto *data = static_cast<const uint8_t *>(bytes);
    uint64_t hash = 0xcbf29ce484222325ull ^ count;
    if (count <= 1024u) return hashBytes(data, count, hash);
    constexpr uint32_t kSamples = 128u;
    for (uint32_t i = 0u; i < kSamples; ++i) {
        uint64_t word;
        std::memcpy(&word, data + static_cast<uint64_t>(count - 8u) * i / (kSamples - 1u), 8u);
        hash = (hash ^ word) * 0x100000001b3ull;
    }
    return hash;
}

struct TextureEntry {
    bool used = false;
    uint32_t data = 0u;
    uint32_t format_byte = 0u;
    uint32_t width = 0u;
    uint32_t height = 0u;
    uint32_t volume_depth = 0u;
    uint32_t mip_levels = 0u;
    uint8_t palette[kPaletteBytes]{};
    uint64_t fingerprint = 0u;
    GLuint texture = 0u;
};

struct RenderTargetEntry {
    RecompD3dTextureDesc desc{};
    GLuint texture = 0u;
    uint64_t bytes = 0u;
    uint32_t host_width = 0u, host_height = 0u;
};

struct DepthTargetEntry {
    RecompD3dTextureDesc desc{};
    GLuint renderbuffer = 0u;
    bool host_main = false;
    uint64_t bytes = 0u;
    uint32_t host_width = 0u;
};

struct SamplerEntry {
    GLint u, v, w;
    bool point_mip_filter;
    float mip_lod_bias;
    GLuint sampler;
};

struct ProgramEntry {
    GLuint program = 0u;
    RecompD3dVertexLayout layout{};
};

struct ShaderKey {
    uint32_t fvf;
    uint32_t program_count;
    uint64_t program_hash;
    uint8_t coverage_only, scaled_depth_bias, sample_depth_bias, pad;
};

struct ShaderKeyHash {
    size_t operator()(const ShaderKey &key) const { return static_cast<size_t>(hashBytes(&key, sizeof key)); }
};

struct ShaderKeyEqual {
    bool operator()(const ShaderKey &a, const ShaderKey &b) const { return std::memcmp(&a, &b, sizeof a) == 0; }
};

/* Fixed-function raster state of one draw: SDL_GPU's pipeline key, applied
   field by field so unchanged GL state is not set again. */
struct RasterState {
    GLenum primitive = GL_TRIANGLES;
    RecompD3dCullMode cull = RECOMP_D3D_CULL_NONE;
    int32_t depth_bias = 0;
    float slope_bias = 0.0f;
    bool depth_test = false, depth_write = false;
    GLenum depth_func = GL_LEQUAL;
    bool stencil = false;
    uint8_t stencil_read = 0xffu, stencil_write = 0xffu;
    GLenum stencil_func = GL_ALWAYS, stencil_fail = GL_KEEP, stencil_zfail = GL_KEEP, stencil_pass = GL_KEEP;
    bool blend = false;
    GLenum src_rgb = GL_ONE, dst_rgb = GL_ZERO, src_alpha = GL_ONE, dst_alpha = GL_ZERO;
    GLenum blend_op = GL_FUNC_ADD;
    uint8_t write_mask = 0x0fu;
};

struct QueuedDraw {
    GLuint program = 0u;
    GLuint vao = 0u;
    RasterState raster{};
    uint32_t vertex_offset = 0u;
    uint32_t vertex_stride = 0u;
    uint32_t index_offset = 0u;
    uint32_t index_count = 0u;
    GLuint textures[4]{};
    GLuint samplers[4]{};
    uint32_t uniform_offset = 0u;
    uint32_t program_offset = UINT32_MAX;
    float blend_constants[4]{};
    uint8_t stencil_ref = 0u;
    float viewport[6]{};
    int32_t scissor[4]{};
};

/* Draws for one bound target, recorded until the target changes or something
   must observe what they drew. */
struct Segment {
    GLuint color = 0u;
    GLuint depth = 0u;
    uint32_t width = 0u, height = 0u;
    std::vector<uint8_t> vertex_data;
    std::vector<uint8_t> index_data;
    std::vector<uint8_t> uniform_data;
    std::vector<QueuedDraw> draws;
};

struct BoundTarget {
    GLuint color = 0u;
    GLuint depth = 0u;
    RenderTargetEntry *entry = nullptr;  // null for the main target
    uint32_t guest_width = 0u, guest_height = 0u;
    uint32_t host_width = 0u, host_height = 0u;
    float scale_x = 1.0f, scale_y = 1.0f;
};

struct PendingQuery {
    uint32_t index = 0u;
    GLuint query = 0u;  // 0: nothing was drawn while it was open
    float host_pixels_per_guest_pixel = 1.0f;
    std::chrono::steady_clock::time_point ended{};
};

/* How long a query may report itself unavailable before the result is read
   with a blocking call (RECOMP_GL_QUERY_TIMEOUT_MS, default 50). The guest
   polls GetVisibilityTestResult in a loop, so a result that never reports
   available would hang it: Eden's GPU emulation was seen never marking
   nouveau's non-blocking occlusion result ready, while a blocking read waits
   for the GPU's fence, which it does signal. */
std::chrono::milliseconds queryTimeout()
{
    static const long ms = [] {
        const char *value = std::getenv("RECOMP_GL_QUERY_TIMEOUT_MS");
        return value != nullptr && value[0] != '\0' ? std::strtol(value, nullptr, 10) : 50L;
    }();
    return std::chrono::milliseconds(ms < 0 ? 0 : ms);
}

struct FramebufferKey {
    GLuint color, depth;
    bool operator==(const FramebufferKey &other) const { return color == other.color && depth == other.depth; }
};

struct FramebufferKeyHash {
    size_t operator()(const FramebufferKey &key) const { return (static_cast<size_t>(key.color) << 20) ^ key.depth; }
};

} // namespace

struct RecompD3dPresenter {
    RecompD3dPresenterConfig config{};
    std::thread::id owner_thread;
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLContext context = EGL_NO_CONTEXT;
    EGLSurface surface = EGL_NO_SURFACE;
    uint32_t window_width = 1280u, window_height = 720u;
    bool close_requested = false;
    bool widescreen = false;
    bool flip_winding = false;
    bool s3tc = false;
    float scale = 1.0f;
    bool copy_mips = false;
    GLint uniform_align = 256;
#ifdef __SWITCH__
    PadState pad{};
#endif

    GLuint main_color = 0u;
    GLuint main_depth = 0u;
    GLuint back_buffer_copy = 0u;  // GetBackBuffer(0) snapshots
    GLuint previous_frame = 0u;    // GetBackBuffer(-1)
    GLuint white_texture = 0u;
    GLuint white_volume = 0u;

    GLuint draw_sampler = 0u;          // linear, wrap
    GLuint filter_sampler = 0u;        // linear, point mips, clamp
    GLuint program_mask_sampler = 0u;
    GLuint point_sampler = 0u;
    std::vector<SamplerEntry> samplers;

    GLuint vertex_buffer = 0u, index_buffer = 0u, uniform_buffer = 0u, gamma_buffer = 0u;
    uint32_t vertex_used = 0u, index_used = 0u, uniform_used = 0u;

    Segment segment;
    std::unordered_map<FramebufferKey, GLuint, FramebufferKeyHash> framebuffers;
    GLuint bound_framebuffer = UINT32_MAX;

    std::unordered_map<ShaderKey, ProgramEntry, ShaderKeyHash, ShaderKeyEqual> programs;
    std::unordered_map<uint32_t, GLuint> vaos;  // by shader FVF
    std::thread precompile;
    volatile bool precompile_stop = false;

    std::vector<TextureEntry> textures = std::vector<TextureEntry>(kTextureSlots);
    std::unordered_multimap<uint32_t, uint32_t> texture_index;
    uint32_t texture_count = 0u;
    uint32_t next_texture_slot = 0u;
    std::vector<GLuint> retired_textures;

    std::vector<RenderTargetEntry> render_targets;
    std::vector<DepthTargetEntry> depth_targets;
    uint64_t target_bytes = 0u;

    bool gamma_enabled = false;
    float gamma_ramp[256][4]{};
    GLuint present_program = 0u;
    GLint present_rect = -1, present_gamma = -1;
    GLuint empty_vao = 0u;

    // Applied raster state, so redundant changes are skipped within a segment.
    RasterState applied{};
    bool applied_valid = false;

    BoundTarget last_bound;
    bool visibility_open = false;
    PendingQuery visibility_active;
    std::vector<PendingQuery> visibility_pending;
    std::vector<GLuint> free_queries;

    uint32_t present_count = 0u;
    uint32_t draw_count = 0u;
    unsigned frame_dump_count = 0u;
};

namespace {

RecompD3dPresenter *active_presenter;
bool immediate_present;

uint32_t presentClientWidth(const RecompD3dPresenter *presenter, uint64_t height)
{
    return static_cast<uint32_t>(presenter->widescreen
        ? (height * 16u + 8u) / 9u
        : (height * 4u + 1u) / 3u);
}

uint32_t mainHeight(const RecompD3dPresenter *presenter)
{
    return static_cast<uint32_t>(presenter->config.height * presenter->scale + 0.5f);
}

uint32_t mainWidth(const RecompD3dPresenter *presenter)
{
    return presenter->scale == 1.0f
        ? presenter->config.width : presentClientWidth(presenter, mainHeight(presenter));
}

uint32_t scaledSize(const RecompD3dPresenter *presenter, uint32_t guest)
{
    return std::min(static_cast<uint32_t>(guest * presenter->scale + 0.5f), kMaxTextureSize);
}

/* ---- GL objects ------------------------------------------------------------ */

GLuint createTexture2D(GLenum internal_format, uint32_t width, uint32_t height, uint32_t levels)
{
    GLuint texture = 0u;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexStorage2D(GL_TEXTURE_2D, static_cast<GLsizei>(levels), internal_format,
        static_cast<GLsizei>(width), static_cast<GLsizei>(height));
    // Sampled levels stop at the last stored one, or a mip filter samples black.
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, static_cast<GLint>(levels - 1u));
    return texture;
}

GLuint createTexture3D(uint32_t width, uint32_t height, uint32_t depth)
{
    GLuint texture = 0u;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_3D, texture);
    glTexStorage3D(GL_TEXTURE_3D, 1, GL_RGBA8, static_cast<GLsizei>(width), static_cast<GLsizei>(height),
        static_cast<GLsizei>(depth));
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAX_LEVEL, 0);
    return texture;
}

GLuint createDepthRenderbuffer(uint32_t width, uint32_t height)
{
    GLuint renderbuffer = 0u;
    glGenRenderbuffers(1, &renderbuffer);
    glBindRenderbuffer(GL_RENDERBUFFER, renderbuffer);
    /* As SDL_GPU's preferred D32_FLOAT_S8; upstream notes D24S8's constant bias
       unit matches it for depths between 0.5 and 1. */
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH32F_STENCIL8, static_cast<GLsizei>(width),
        static_cast<GLsizei>(height));
    return renderbuffer;
}

void forgetFramebuffers(RecompD3dPresenter *presenter, GLuint color, GLuint depth)
{
    for (auto it = presenter->framebuffers.begin(); it != presenter->framebuffers.end();) {
        if ((color != 0u && it->first.color == color) || (depth != 0u && it->first.depth == depth)) {
            if (presenter->bound_framebuffer == it->second) presenter->bound_framebuffer = UINT32_MAX;
            glDeleteFramebuffers(1, &it->second);
            it = presenter->framebuffers.erase(it);
        } else {
            ++it;
        }
    }
}

void bindFramebuffer(RecompD3dPresenter *presenter, GLuint color, GLuint depth)
{
    const FramebufferKey key{color, depth};
    GLuint framebuffer;
    const auto found = presenter->framebuffers.find(key);
    if (found != presenter->framebuffers.end()) {
        framebuffer = found->second;
        if (framebuffer == presenter->bound_framebuffer) return;
        glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    } else {
        glGenFramebuffers(1, &framebuffer);
        glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color, 0);
        if (depth != 0u) glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, depth);
        const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        if (status != GL_FRAMEBUFFER_COMPLETE) {
            std::fprintf(stderr, "recomp d3d presenter: framebuffer color=%u depth=%u incomplete 0x%04x\n",
                color, depth, status);
        }
        presenter->framebuffers.emplace(key, framebuffer);
    }
    presenter->bound_framebuffer = framebuffer;
}

void retireTexture(RecompD3dPresenter *presenter, GLuint &texture)
{
    if (texture != 0u) presenter->retired_textures.push_back(texture);
    texture = 0u;
}

void releaseRetiredTextures(RecompD3dPresenter *presenter)
{
    for (GLuint texture : presenter->retired_textures) {
        forgetFramebuffers(presenter, texture, 0u);
        glDeleteTextures(1, &texture);
    }
    presenter->retired_textures.clear();
}

void unindexTexture(RecompD3dPresenter *presenter, uint32_t slot)
{
    const auto range = presenter->texture_index.equal_range(presenter->textures[slot].data);
    for (auto it = range.first; it != range.second; ++it) {
        if (it->second == slot) {
            presenter->texture_index.erase(it);
            return;
        }
    }
}

void flushSegment(RecompD3dPresenter *presenter);

/* ---- Targets ----------------------------------------------------------------- */

RenderTargetEntry *findRenderTarget(RecompD3dPresenter *presenter, const RecompD3dTextureDesc &desc)
{
    if (desc.volume_depth != 0u) return nullptr;
    for (RenderTargetEntry &entry : presenter->render_targets) {
        if (entry.desc.data == desc.data && entry.desc.format_byte == desc.format_byte &&
            entry.desc.width == desc.width && entry.desc.height == desc.height) {
            return &entry;
        }
    }
    return nullptr;
}

RecompD3dPresenterError lookupDepthTarget(RecompD3dPresenter *presenter,
    const RecompD3dPresenterTarget &target, uint32_t color_width, uint32_t color_height,
    uint32_t host_width, uint32_t host_height, GLuint &renderbuffer)
{
    renderbuffer = 0u;
    if (target.no_depth) return RECOMP_D3D_PRESENTER_OK;
    const RecompD3dTextureDesc &desc = target.depth;
    const uint32_t width = target.custom_depth ? desc.width : presenter->config.width;
    const uint32_t height = target.custom_depth ? desc.height : presenter->config.height;
    if (width != color_width || height != color_height) {
        std::fprintf(stderr,
            "recomp d3d presenter: target size mismatch color=0x%08X fmt=0x%02X size=%ux%u "
            "depth=0x%08X fmt=0x%02X size=%ux%u custom_depth=%d\n",
            target.color.data, target.color.format_byte, color_width, color_height,
            desc.data, desc.format_byte, width, height, target.custom_depth ? 1 : 0);
        return RECOMP_D3D_PRESENTER_UNSUPPORTED_COMMAND;
    }
    if (!target.custom_depth) {
        renderbuffer = presenter->main_depth;
        return RECOMP_D3D_PRESENTER_OK;
    }
    if (!desc.depth || desc.data == 0u || width == 0u || height == 0u ||
        width > kMaxTextureSize || height > kMaxTextureSize ||
        (desc.format_byte != 0x2au && desc.format_byte != 0x2eu)) {
        std::fprintf(stderr,
            "recomp d3d presenter: unsupported depth target data=0x%08X fmt=0x%02X size=%ux%u depth=%d\n",
            desc.data, desc.format_byte, width, height, desc.depth ? 1 : 0);
        return RECOMP_D3D_PRESENTER_UNSUPPORTED_COMMAND;
    }
    const bool host_main = color_width == presenter->config.width &&
        color_height == presenter->config.height && presenter->scale != 1.0f;
    for (const DepthTargetEntry &entry : presenter->depth_targets) {
        if (entry.desc.data == desc.data && entry.desc.format_byte == desc.format_byte &&
            entry.desc.width == width && entry.desc.height == height &&
            entry.host_main == host_main && entry.host_width == host_width) {
            renderbuffer = entry.renderbuffer;
            return RECOMP_D3D_PRESENTER_OK;
        }
    }
    const uint64_t bytes = host_main ? 0u : static_cast<uint64_t>(width) * height * 4u;
    if (bytes > kTargetByteLimit - presenter->target_bytes) {
        std::fprintf(stderr, "recomp d3d presenter: target memory budget exhausted\n");
        return RECOMP_D3D_PRESENTER_OUT_OF_MEMORY;
    }
    renderbuffer = createDepthRenderbuffer(host_width, host_height);
    if (renderbuffer == 0u) return RECOMP_D3D_PRESENTER_HOST_FAILURE;
    presenter->depth_targets.push_back({desc, renderbuffer, host_main, bytes, host_width});
    presenter->target_bytes += bytes;
    std::fprintf(stderr, "recomp d3d presenter: depth target data=0x%08X fmt=0x%02X size=%ux%u\n",
        desc.data, desc.format_byte, host_width, host_height);
    return RECOMP_D3D_PRESENTER_OK;
}

RecompD3dPresenterError bindTarget(RecompD3dPresenter *presenter,
    const RecompD3dPresenterTarget &target, BoundTarget &bound)
{
    bound = {};
    bound.color = presenter->main_color;
    uint32_t width = presenter->config.width, height = presenter->config.height;
    uint32_t host_width = mainWidth(presenter), host_height = mainHeight(presenter);
    const bool host_main = !target.offscreen ||
        (target.color.width == width && target.color.height == height);

    if (target.offscreen) {
        const RecompD3dTextureDesc &desc = target.color;
        if (desc.depth || desc.data == 0u || desc.width == 0u || desc.height == 0u ||
            desc.width > kMaxTextureSize || desc.height > kMaxTextureSize ||
            (desc.format_byte != RECOMP_D3D_TEXTURE_FORMAT_A8R8G8B8 && desc.format_byte != 0x12u)) {
            std::fprintf(stderr,
                "recomp d3d presenter: unsupported target data=0x%08X fmt=0x%02X size=%ux%u no_depth=%d\n",
                desc.data, desc.format_byte, desc.width, desc.height, target.no_depth ? 1 : 0);
            return RECOMP_D3D_PRESENTER_UNSUPPORTED_COMMAND;
        }
        RenderTargetEntry *entry = findRenderTarget(presenter, desc);
        if (entry == nullptr) {
            const bool scaled = !host_main && target.custom_depth;
            host_width = scaled ? scaledSize(presenter, desc.width) : host_main ? host_width : desc.width;
            host_height = scaled ? scaledSize(presenter, desc.height) : host_main ? host_height : desc.height;
            const uint64_t bytes = static_cast<uint64_t>(desc.width) * desc.height * 4u;
            if (bytes > kTargetByteLimit - presenter->target_bytes) {
                std::fprintf(stderr, "recomp d3d presenter: target memory budget exhausted\n");
                return RECOMP_D3D_PRESENTER_OUT_OF_MEMORY;
            }
            RenderTargetEntry created{};
            created.desc = desc;
            created.bytes = bytes;
            created.host_width = host_width;
            created.host_height = host_height;
            created.texture = createTexture2D(GL_RGBA8, host_width, host_height, 1u);
            if (created.texture == 0u) return RECOMP_D3D_PRESENTER_HOST_FAILURE;
            presenter->render_targets.push_back(created);
            presenter->target_bytes += bytes;
            entry = &presenter->render_targets.back();
            std::fprintf(stderr, "recomp d3d presenter: render target data=0x%08X fmt=0x%02X size=%ux%u\n",
                desc.data, desc.format_byte, desc.width, desc.height);
        }
        bound.color = entry->texture;
        bound.entry = entry;
        width = desc.width;
        height = desc.height;
        host_width = entry->host_width;
        host_height = entry->host_height;
    }
    const RecompD3dPresenterError depth = lookupDepthTarget(presenter, target, width, height,
        host_width, host_height, bound.depth);
    if (depth != RECOMP_D3D_PRESENTER_OK) return depth;
    bound.guest_width = width;
    bound.guest_height = height;
    bound.host_width = host_width;
    bound.host_height = host_height;
    bound.scale_x = static_cast<float>(host_width) / width;
    bound.scale_y = static_cast<float>(host_height) / height;
    return RECOMP_D3D_PRESENTER_OK;
}

/* ---- Samplers and textures ----------------------------------------------------- */

GLint hostAddressMode(uint32_t mode)
{
    /* Xbox D3DTADDRESS WRAP, MIRROR, CLAMP, BORDER, CLAMPTOEDGE. The draw
       adapter stops on BORDER before it gets here, as upstream. */
    switch (mode) {
    case 2u: return GL_MIRRORED_REPEAT;
    case 3u:
    case 4u:
    case 5u: return GL_CLAMP_TO_EDGE;
    default: return GL_REPEAT;
    }
}

GLuint createSampler(GLint u, GLint v, GLint w, bool point_mip_filter, float bias, bool nearest = false)
{
    GLuint sampler = 0u;
    glGenSamplers(1, &sampler);
    glSamplerParameteri(sampler, GL_TEXTURE_MIN_FILTER, nearest ? GL_NEAREST_MIPMAP_NEAREST
        : point_mip_filter ? GL_LINEAR_MIPMAP_NEAREST : GL_LINEAR_MIPMAP_LINEAR);
    glSamplerParameteri(sampler, GL_TEXTURE_MAG_FILTER, nearest ? GL_NEAREST : GL_LINEAR);
    glSamplerParameteri(sampler, GL_TEXTURE_WRAP_S, u);
    glSamplerParameteri(sampler, GL_TEXTURE_WRAP_T, v);
    glSamplerParameteri(sampler, GL_TEXTURE_WRAP_R, w);
    glSamplerParameterf(sampler, GL_TEXTURE_LOD_BIAS, bias);
    glSamplerParameterf(sampler, GL_TEXTURE_MAX_LOD, 1000.0f);
    return sampler;
}

GLuint lookupDrawSampler(RecompD3dPresenter *presenter, uint32_t address_u, uint32_t address_v,
    bool point_mip_filter, float bias, uint32_t address_w = 1u)
{
    const GLint u = hostAddressMode(address_u), v = hostAddressMode(address_v), w = hostAddressMode(address_w);
    if (u == GL_REPEAT && v == GL_REPEAT && w == GL_REPEAT && !point_mip_filter && bias == 0.0f) {
        return presenter->draw_sampler;
    }
    for (const SamplerEntry &entry : presenter->samplers) {
        if (entry.u == u && entry.v == v && entry.w == w &&
            entry.point_mip_filter == point_mip_filter && entry.mip_lod_bias == bias) {
            return entry.sampler;
        }
    }
    const GLuint sampler = createSampler(u, v, w, point_mip_filter, bias);
    presenter->samplers.push_back({u, v, w, point_mip_filter, bias, sampler});
    return sampler;
}

bool isSwizzledTextureFormat(uint32_t format_byte)
{
    return format_byte == RECOMP_D3D_TEXTURE_FORMAT_A8R8G8B8 ||
        format_byte == RECOMP_D3D_TEXTURE_FORMAT_A8 || format_byte == 0x28u;
}

/* DXT software decode, for a driver without S3TC (DOA3 NX's decoder). */
void decodeColorBlock(const uint8_t *block, uint8_t out[16][4], bool dxt1)
{
    uint16_t c0, c1;
    std::memcpy(&c0, block, 2);
    std::memcpy(&c1, block + 2, 2);
    uint8_t colors[4][4];
    const auto expand = [](uint16_t c, uint8_t *rgba) {
        rgba[0] = static_cast<uint8_t>(((c >> 11) & 31) * 255 / 31);
        rgba[1] = static_cast<uint8_t>(((c >> 5) & 63) * 255 / 63);
        rgba[2] = static_cast<uint8_t>((c & 31) * 255 / 31);
        rgba[3] = 255;
    };
    expand(c0, colors[0]);
    expand(c1, colors[1]);
    for (int i = 0; i < 3; ++i) {
        if (!dxt1 || c0 > c1) {
            colors[2][i] = static_cast<uint8_t>((2 * colors[0][i] + colors[1][i]) / 3);
            colors[3][i] = static_cast<uint8_t>((colors[0][i] + 2 * colors[1][i]) / 3);
        } else {
            colors[2][i] = static_cast<uint8_t>((colors[0][i] + colors[1][i]) / 2);
            colors[3][i] = 0;
        }
    }
    colors[2][3] = 255;
    colors[3][3] = (!dxt1 || c0 > c1) ? 255 : 0;
    uint32_t bits;
    std::memcpy(&bits, block + 4, 4);
    for (int i = 0; i < 16; ++i) std::memcpy(out[i], colors[(bits >> (2 * i)) & 3], 4);
}

void decodeDxt(const uint8_t *source, uint32_t width, uint32_t height, uint32_t format, uint8_t *destination)
{
    const uint32_t block_bytes = format == RECOMP_D3D_TEXTURE_FORMAT_DXT1 ? 8u : 16u;
    const uint32_t blocks_wide = (width + 3u) / 4u, blocks_high = (height + 3u) / 4u;
    for (uint32_t by = 0u; by < blocks_high; ++by) {
        for (uint32_t bx = 0u; bx < blocks_wide; ++bx) {
            const uint8_t *block = source + (by * blocks_wide + bx) * block_bytes;
            uint8_t texels[16][4];
            if (format == RECOMP_D3D_TEXTURE_FORMAT_DXT1) {
                decodeColorBlock(block, texels, true);
            } else {
                decodeColorBlock(block + 8, texels, false);
                if (format == RECOMP_D3D_TEXTURE_FORMAT_DXT3) {
                    for (int i = 0; i < 16; ++i) {
                        const uint8_t a = (block[i / 2] >> ((i & 1) * 4)) & 15;
                        texels[i][3] = static_cast<uint8_t>(a * 17);
                    }
                } else {
                    const uint8_t a0 = block[0], a1 = block[1];
                    uint8_t alphas[8] = {a0, a1};
                    if (a0 > a1) {
                        for (int i = 1; i < 7; ++i) alphas[i + 1] = static_cast<uint8_t>(((7 - i) * a0 + i * a1) / 7);
                    } else {
                        for (int i = 1; i < 5; ++i) alphas[i + 1] = static_cast<uint8_t>(((5 - i) * a0 + i * a1) / 5);
                        alphas[6] = 0;
                        alphas[7] = 255;
                    }
                    uint64_t bits = 0u;
                    for (int i = 0; i < 6; ++i) bits |= static_cast<uint64_t>(block[2 + i]) << (8 * i);
                    for (int i = 0; i < 16; ++i) texels[i][3] = alphas[(bits >> (3 * i)) & 7];
                }
            }
            for (uint32_t y = 0u; y < 4u; ++y) {
                for (uint32_t x = 0u; x < 4u; ++x) {
                    const uint32_t px = bx * 4u + x, py = by * 4u + y;
                    if (px < width && py < height) {
                        std::memcpy(destination + (static_cast<size_t>(py) * width + px) * 4u, texels[y * 4 + x], 4);
                    }
                }
            }
        }
    }
}

/* Copies the guest back buffer into a snapshot texture of the same size. */
void copyGuestBuffer(RecompD3dPresenter *presenter, GLuint target, bool mips = true)
{
    glCopyImageSubData(presenter->main_color, GL_TEXTURE_2D, 0, 0, 0, 0, target, GL_TEXTURE_2D, 0, 0, 0, 0,
        static_cast<GLsizei>(mainWidth(presenter)), static_cast<GLsizei>(mainHeight(presenter)), 1);
    if (mips && presenter->copy_mips) {
        glBindTexture(GL_TEXTURE_2D, target);
        glGenerateMipmap(GL_TEXTURE_2D);
    }
}

RecompD3dPresenterError submitCopyMainColor(RecompD3dPresenter *presenter, const RecompD3dTextureDesc &destination)
{
    if (destination.width != presenter->config.width || destination.height != presenter->config.height)
        return RECOMP_D3D_PRESENTER_UNSUPPORTED_COMMAND;
    RecompD3dPresenterTarget target{};
    target.offscreen = true;
    target.no_depth = true;
    target.color = destination;
    BoundTarget bound;
    const RecompD3dPresenterError error = bindTarget(presenter, target, bound);
    if (error != RECOMP_D3D_PRESENTER_OK) return error;
    flushSegment(presenter);
    copyGuestBuffer(presenter, bound.entry->texture, false);
    return RECOMP_D3D_PRESENTER_OK;
}

GLuint lookupBackBufferTexture(RecompD3dPresenter *presenter, const RecompD3dTextureDesc &desc, bool previous_frame)
{
    /* As upstream: a horizontally supersampled guest back buffer maps onto
       the one host buffer by its normalized UVs. */
    if (desc.format_byte != 0x12u || !desc.linear || desc.depth ||
        desc.width == 0u || desc.height == 0u ||
        desc.width % presenter->config.width != 0u ||
        desc.height % presenter->config.height != 0u) return 0u;
    GLuint &copy = previous_frame ? presenter->previous_frame : presenter->back_buffer_copy;
    const bool created = copy == 0u;
    if (created) {
        uint32_t levels = 1u;
        if (presenter->copy_mips) {
            while ((std::max(mainWidth(presenter), mainHeight(presenter)) >> levels) != 0u) ++levels;
        }
        copy = createTexture2D(GL_RGBA8, mainWidth(presenter), mainHeight(presenter), levels);
        if (copy == 0u) return 0u;
    }
    if (previous_frame && !created) return copy;
    flushSegment(presenter);
    copyGuestBuffer(presenter, copy);
    return copy;
}

struct DrawTextureTally {
    uint32_t textured[256];
    uint32_t rejected[256];
    uint32_t untextured;
};

DrawTextureTally draw_texture_tally;

void countDrawTexture(const RecompD3dPresenterDrawCommand &draw, bool sampled)
{
    if (!draw.has_texture) ++draw_texture_tally.untextured;
    else if (sampled) ++draw_texture_tally.textured[draw.texture.format_byte & 0xffu];
    else ++draw_texture_tally.rejected[draw.texture.format_byte & 0xffu];
}

/* Uploads one decoded texture; returns 0 on failure. */
GLuint uploadTexture(RecompD3dPresenter *presenter, const RecompD3dTextureDesc &desc,
    const RecompD3dPresenterDrawCommand &draw, bool volume, bool linear_bgra, bool palettized, bool compressed,
    uint32_t levels)
{
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    const auto *bytes = static_cast<const uint8_t *>(draw.texture_bytes);
    if (compressed) {
        const uint32_t block_bytes = desc.format_byte == RECOMP_D3D_TEXTURE_FORMAT_DXT1 ? 8u : 16u;
        const GLenum format = desc.format_byte == RECOMP_D3D_TEXTURE_FORMAT_DXT1 ? GL_COMPRESSED_RGBA_S3TC_DXT1_EXT
            : desc.format_byte == RECOMP_D3D_TEXTURE_FORMAT_DXT3 ? GL_COMPRESSED_RGBA_S3TC_DXT3_EXT
            : GL_COMPRESSED_RGBA_S3TC_DXT5_EXT;
        const GLuint texture = createTexture2D(presenter->s3tc ? format : GL_RGBA8, desc.width, desc.height, levels);
        uint32_t width = desc.width, height = desc.height, offset = 0u;
        std::vector<uint8_t> decoded;
        for (uint32_t level = 0u; level < levels; ++level) {
            const uint32_t size = ((width + 3u) / 4u) * block_bytes * ((height + 3u) / 4u);
            if (presenter->s3tc) {
                glCompressedTexSubImage2D(GL_TEXTURE_2D, static_cast<GLint>(level), 0, 0,
                    static_cast<GLsizei>(width), static_cast<GLsizei>(height), format, static_cast<GLsizei>(size),
                    bytes + offset);
            } else {
                decoded.resize(static_cast<size_t>(width) * height * 4u);
                decodeDxt(bytes + offset, width, height, desc.format_byte, decoded.data());
                glTexSubImage2D(GL_TEXTURE_2D, static_cast<GLint>(level), 0, 0, static_cast<GLsizei>(width),
                    static_cast<GLsizei>(height), GL_RGBA, GL_UNSIGNED_BYTE, decoded.data());
            }
            offset += size;
            width = width > 1u ? width / 2u : 1u;
            height = height > 1u ? height / 2u : 1u;
        }
        return texture;
    }
    if (volume) {
        std::vector<uint8_t> unpacked(static_cast<size_t>(desc.width) * desc.height * desc.volume_depth * 4u);
        if (!recomp_d3d_texture_unswizzle_volume(bytes, unpacked.data(), desc.width, desc.height,
                desc.volume_depth, 4u)) return 0u;
        const GLuint texture = createTexture3D(desc.width, desc.height, desc.volume_depth);
        glTexSubImage3D(GL_TEXTURE_3D, 0, 0, 0, 0, static_cast<GLsizei>(desc.width),
            static_cast<GLsizei>(desc.height), static_cast<GLsizei>(desc.volume_depth), GL_BGRA, GL_UNSIGNED_BYTE,
            unpacked.data());
        return texture;
    }
    if (linear_bgra) {
        const GLuint texture = createTexture2D(GL_RGBA8, desc.width, desc.height, 1u);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, static_cast<GLint>(desc.pitch / 4u));
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, static_cast<GLsizei>(desc.width), static_cast<GLsizei>(desc.height),
            GL_BGRA, GL_UNSIGNED_BYTE, bytes);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        return texture;
    }
    std::vector<uint8_t> unpacked;
    if (palettized) {
        const size_t texels = static_cast<size_t>(desc.width) * desc.height;
        std::vector<uint8_t> indices(texels);
        if (!recomp_d3d_texture_unswizzle(bytes, indices.data(), desc.width, desc.height, 1u)) return 0u;
        unpacked.resize(texels * 4u);
        const auto *palette = static_cast<const uint8_t *>(draw.palette_bytes);
        // Little-endian ARGB palette words are already BGRA bytes.
        for (size_t i = 0u; i < texels; ++i) std::memcpy(unpacked.data() + i * 4u, palette + indices[i] * 4u, 4u);
    } else {
        const uint32_t texel_bytes = desc.bits_per_pixel / 8u;
        unpacked.resize(static_cast<size_t>(desc.width) * desc.height * texel_bytes);
        if (!recomp_d3d_texture_unswizzle(bytes, unpacked.data(), desc.width, desc.height, texel_bytes)) return 0u;
        if (desc.format_byte == 0x28u) {
            /* V8U8: U in the low byte, V in the high byte, read signed.
               shortcut: SNORM scale is unverified against NV2A, as upstream. */
            const GLuint texture = createTexture2D(GL_RG8_SNORM, desc.width, desc.height, 1u);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, static_cast<GLsizei>(desc.width),
                static_cast<GLsizei>(desc.height), GL_RG, GL_BYTE, unpacked.data());
            return texture;
        }
        if (desc.format_byte == RECOMP_D3D_TEXTURE_FORMAT_A8) {
            // NV2A supplies white RGB with A8.
            std::vector<uint8_t> white(unpacked.size() * 4u, 0xffu);
            for (size_t i = 0u; i < unpacked.size(); ++i) white[i * 4u + 3u] = unpacked[i];
            unpacked.swap(white);
        }
    }
    const GLuint texture = createTexture2D(GL_RGBA8, desc.width, desc.height, 1u);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, static_cast<GLsizei>(desc.width), static_cast<GLsizei>(desc.height),
        GL_BGRA, GL_UNSIGNED_BYTE, unpacked.data());
    return texture;
}

GLuint lookupTexture(RecompD3dPresenter *presenter, const RecompD3dPresenterDrawCommand &draw)
{
    const RecompD3dTextureDesc &desc = draw.texture;
    const bool volume = desc.volume_depth != 0u;
    if (volume && (desc.linear || desc.depth || desc.format_byte != 6u ||
        desc.bits_per_pixel != 32u || desc.mip_levels > 1u ||
        desc.width == 0u || desc.height == 0u ||
        desc.width > kMaxVolumeSize || desc.height > kMaxVolumeSize || desc.volume_depth > kMaxVolumeSize ||
        static_cast<uint64_t>(desc.width) * desc.height * desc.volume_depth * 4u > draw.texture_byte_count))
        return 0u;
    if (desc.format_byte == 0x28u && !draw.bump_env) return 0u;
    const bool palettized = desc.format_byte == RECOMP_D3D_TEXTURE_FORMAT_P8;
    const bool linear_bgra = desc.format_byte == 0x12u;
    const bool compressed = desc.format_byte == RECOMP_D3D_TEXTURE_FORMAT_DXT1 ||
        desc.format_byte == RECOMP_D3D_TEXTURE_FORMAT_DXT3 || desc.format_byte == RECOMP_D3D_TEXTURE_FORMAT_DXT5;
    const uint32_t levels = compressed && desc.mip_levels ? desc.mip_levels : 1u;
    if (compressed) {
        const uint32_t span = recomp_d3d_texture_compressed_mip_span(&desc);
        if (span == 0u || span > draw.texture_byte_count ||
            desc.width > kMaxTextureSize || desc.height > kMaxTextureSize) return 0u;
    }
    if (draw.texture_is_backbuffer) {
        const GLuint texture = lookupBackBufferTexture(presenter, desc, draw.texture_is_front_buffer);
        static bool reported;
        if (texture == 0u && !reported) {
            reported = true;
            std::fprintf(stderr, "recomp d3d: frame-buffer texture %ux%u format=0x%x has no host view;"
                " the draw samples nothing\n", desc.width, desc.height, desc.format_byte);
        }
        return texture;
    }
    if (palettized && (draw.palette_bytes == nullptr || draw.palette_byte_count != kPaletteBytes)) return 0u;
    if (RenderTargetEntry *entry = findRenderTarget(presenter, desc)) return entry->texture;
    if (linear_bgra && (!desc.linear || desc.depth ||
        desc.bits_per_pixel != 32u || desc.width == 0u || desc.height == 0u ||
        desc.width > kMaxTextureSize || desc.height > kMaxTextureSize ||
        desc.pitch < desc.width * 4u || desc.pitch % 4u != 0u || draw.texture_bytes == nullptr ||
        static_cast<uint64_t>(desc.height - 1u) * desc.pitch + desc.width * 4u > draw.texture_byte_count)) {
        return 0u;
    }
    const auto cached = presenter->texture_index.equal_range(desc.data);
    const bool fingerprinted = !linear_bgra && draw.texture_bytes != nullptr;
    const uint64_t fingerprint = fingerprinted ? textureFingerprint(draw.texture_bytes, draw.texture_byte_count) : 0u;
    for (auto it = cached.first; it != cached.second; ++it) {
        TextureEntry &entry = presenter->textures[it->second];
        if (entry.format_byte == desc.format_byte && entry.width == desc.width &&
            entry.height == desc.height && entry.volume_depth == desc.volume_depth &&
            entry.mip_levels == levels &&
            (!palettized || std::memcmp(entry.palette, draw.palette_bytes, kPaletteBytes) == 0)) {
            if (fingerprinted && entry.fingerprint != fingerprint) {
                static unsigned reported;
                if (reported < 32u) {
                    ++reported;
                    std::fprintf(stderr, "recomp d3d presenter: texels changed data=0x%08X fmt=0x%02X size=%ux%u\n",
                        desc.data, desc.format_byte, desc.width, desc.height);
                }
                unindexTexture(presenter, it->second);
                retireTexture(presenter, entry.texture);
                entry.used = false;
                break;
            }
            if (linear_bgra) {
                // Queued draws sampled the old pixels; record them before this upload.
                flushSegment(presenter);
                glBindTexture(GL_TEXTURE_2D, entry.texture);
                glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
                glPixelStorei(GL_UNPACK_ROW_LENGTH, static_cast<GLint>(desc.pitch / 4u));
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, static_cast<GLsizei>(desc.width),
                    static_cast<GLsizei>(desc.height), GL_BGRA, GL_UNSIGNED_BYTE, draw.texture_bytes);
                glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
            }
            return entry.texture;
        }
    }
    const bool supported = volume || linear_bgra || palettized || compressed ||
        isSwizzledTextureFormat(desc.format_byte);
    if (!supported || draw.texture_bytes == nullptr || draw.texture_byte_count == 0u ||
        (desc.linear && !linear_bgra)) {
        return 0u;
    }
    if (!compressed && !volume && !linear_bgra) {
        const size_t texels = static_cast<size_t>(desc.width) * desc.height;
        const uint32_t texel_bytes = palettized ? 1u : desc.bits_per_pixel / 8u;
        if (desc.width == 0u || desc.height == 0u || desc.width > kMaxTextureSize ||
            desc.height > kMaxTextureSize || texel_bytes == 0u ||
            texels * texel_bytes > draw.texture_byte_count) return 0u;
    }
    stage("texture upload");
    const GLuint texture = uploadTexture(presenter, desc, draw, volume, linear_bgra, palettized, compressed, levels);
    stage("draw");
    if (texture == 0u) return 0u;

    // FIFO eviction, as upstream.
    const uint32_t slot = presenter->next_texture_slot;
    TextureEntry &entry = presenter->textures[slot];
    if (entry.used) unindexTexture(presenter, slot);
    retireTexture(presenter, entry.texture);
    presenter->next_texture_slot = (presenter->next_texture_slot + 1u) % kTextureSlots;
    if (presenter->texture_count < kTextureSlots) ++presenter->texture_count;
    entry.used = true;
    entry.data = desc.data;
    entry.format_byte = desc.format_byte;
    entry.width = desc.width;
    entry.height = desc.height;
    entry.volume_depth = desc.volume_depth;
    entry.mip_levels = levels;
    if (palettized) std::memcpy(entry.palette, draw.palette_bytes, kPaletteBytes);
    entry.fingerprint = fingerprint;
    entry.texture = texture;
    presenter->texture_index.emplace(desc.data, slot);
    return texture;
}

/* ---- Shaders ----------------------------------------------------------------- */

GLuint compileStage(GLenum stage, const char *source, size_t length, const char *what)
{
    GLuint shader = glCreateShader(stage);
    const GLint size = static_cast<GLint>(length);
    glShaderSource(shader, 1, &source, &size);
    glCompileShader(shader);
    GLint ok = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetShaderInfoLog(shader, sizeof log, nullptr, log);
        std::fprintf(stderr, "recomp d3d presenter: %s %s shader rejected: %s\n", what,
            stage == GL_VERTEX_SHADER ? "vertex" : "fragment", log);
        glDeleteShader(shader);
        return 0u;
    }
    return shader;
}

GLuint linkProgram(GLuint vertex, GLuint fragment, const char *what)
{
    if (vertex == 0u || fragment == 0u) return 0u;
    GLuint program = glCreateProgram();
    glAttachShader(program, vertex);
    glAttachShader(program, fragment);
    glLinkProgram(program);
    glDetachShader(program, vertex);
    glDetachShader(program, fragment);
    GLint ok = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetProgramInfoLog(program, sizeof log, nullptr, log);
        std::fprintf(stderr, "recomp d3d presenter: %s program link failed: %s\n", what, log);
        glDeleteProgram(program);
        return 0u;
    }
    return program;
}

/* The draw shader's HLSL for a key, with upstream's SDL_GPU bindings. */
bool drawShaderSource(const RecompD3dDrawShaderKey &draw_key, RecompD3dVertexLayout &layout, std::string &hlsl)
{
    if (!recomp_d3d_draw_shader_source(draw_key, layout, hlsl)) return false;
    hlsl = recomp_shader_draw_bindings(hlsl);
    return true;
}

/* The draw program for one key. Failures are cached too, so a shader that
   cannot build is not retried every draw. */
const ProgramEntry &lookupProgram(RecompD3dPresenter *presenter, const RecompD3dDrawShaderKey &draw_key)
{
    ShaderKey key{};
    key.fvf = draw_key.fvf;
    key.program_count = draw_key.program_count;
    key.program_hash = draw_key.program_count ? hashBytes(draw_key.program, draw_key.program_count * 16u) : 0u;
    key.coverage_only = draw_key.coverage_only;
    key.scaled_depth_bias = draw_key.scaled_depth_bias;
    key.sample_depth_bias = draw_key.sample_depth_bias;
    const auto found = presenter->programs.find(key);
    if (found != presenter->programs.end()) return found->second;
    const auto start = std::chrono::steady_clock::now();
    stage("draw shader compile");
    ProgramEntry entry;
    std::string hlsl;
    if (drawShaderSource(draw_key, entry.layout, hlsl)) {
        // Single-sampled: sample-depth-bias shaders see one sample.
        std::vector<std::string> defines;
        if (draw_key.sample_depth_bias) defines.push_back("RECOMP_SAMPLE_COUNT=1");
        RecompCompiledShader vertex, fragment;
        std::string error;
        if (recomp_shader_compile(hlsl, "vs_main", true, RecompShaderTarget::Glsl, defines, vertex, error) &&
            recomp_shader_compile(hlsl, "ps_main", false, RecompShaderTarget::Glsl, defines, fragment, error)) {
            const GLuint vs = compileStage(GL_VERTEX_SHADER, reinterpret_cast<const char *>(vertex.code.data()),
                vertex.code.size(), "draw");
            const GLuint fs = compileStage(GL_FRAGMENT_SHADER,
                reinterpret_cast<const char *>(fragment.code.data()), fragment.code.size(), "draw");
            entry.program = linkProgram(vs, fs, "draw");
            if (vs != 0u) glDeleteShader(vs);
            if (fs != 0u) glDeleteShader(fs);
        } else {
            std::fprintf(stderr, "recomp d3d presenter: draw shader fvf=0x%03X failed: %s\n",
                draw_key.fvf, error.c_str());
        }
    } else if (draw_key.program_count) {
        std::fprintf(stderr, "recomp d3d presenter: vertex program untranslated fvf=%03x count=%u\n",
            draw_key.fvf, draw_key.program_count);
    }
    std::fprintf(stderr, "recomp d3d presenter: draw shader fvf=0x%03X program=%u%s in %lld ms\n",
        draw_key.fvf, draw_key.program_count, entry.program != 0u ? "" : " failed",
        static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count()));
    return presenter->programs.emplace(key, entry).first->second;
}

/* Compiles the boot shaders' GLSL off the render thread into the shader
   cache (RECOMP_SHADER_CACHE), so first use only links them. */
void precompileDrawShaders(RecompD3dPresenter *presenter)
{
    const auto start = std::chrono::steady_clock::now();
    unsigned compiled = 0u;
    for (const uint32_t fvf : kRecompD3dBootDrawFvfs) {
        if (presenter->precompile_stop) return;
        const RecompD3dDrawShaderKey key{fvf, 0u, nullptr, false, false, false};
        RecompD3dVertexLayout layout;
        std::string hlsl, error;
        RecompCompiledShader vertex, fragment;
        if (drawShaderSource(key, layout, hlsl) &&
            recomp_shader_compile(hlsl, "vs_main", true, RecompShaderTarget::Glsl, {}, vertex, error) &&
            recomp_shader_compile(hlsl, "ps_main", false, RecompShaderTarget::Glsl, {}, fragment, error)) {
            ++compiled;
        }
    }
    std::fprintf(stderr, "recomp d3d presenter: precompiled %u draw shaders in %lld ms\n", compiled,
        static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count()));
}

/* Vertex inputs in the shader's VSIn order, which recomp_shader_draw_bindings
   numbers from zero (upstream's vertexAttributes). */
GLuint lookupVertexArray(RecompD3dPresenter *presenter, uint32_t fvf, const RecompD3dVertexLayout &layout)
{
    const auto found = presenter->vaos.find(fvf);
    if (found != presenter->vaos.end()) return found->second;
    GLuint vao = 0u;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    GLuint location = 0u;
    const auto add = [&](GLint size, GLenum type, GLboolean normalized, uint32_t offset) {
        glEnableVertexAttribArray(location);
        glVertexAttribFormat(location, size, type, normalized, offset);
        glVertexAttribBinding(location, 0u);
        ++location;
    };
    add(layout.pretransformed ? 4 : 3, GL_FLOAT, GL_FALSE, layout.position_offset);
    if (layout.blend_weight_count != 0u) add(static_cast<GLint>(layout.blend_weight_count), GL_FLOAT, GL_FALSE, 12u);
    if (layout.normal_offset != RECOMP_D3D_FVF_ABSENT) add(3, GL_FLOAT, GL_FALSE, layout.normal_offset);
    // Memory order, as SDL_GPU's UBYTE4_NORM: the shader swizzles BGRA.
    if (layout.diffuse_offset != RECOMP_D3D_FVF_ABSENT) add(4, GL_UNSIGNED_BYTE, GL_TRUE, layout.diffuse_offset);
    const uint32_t texture_coords = layout.texcoord_count == 4u ? 4u
        : layout.texcoord_count == 2u ? 2u : (layout.texcoord_count != 0u ? 1u : 0u);
    for (uint32_t i = 0u; i < texture_coords; ++i) add(2, GL_FLOAT, GL_FALSE, layout.texcoord_offset + i * 8u);
    glBindVertexArray(0u);
    presenter->vaos.emplace(fvf, vao);
    return vao;
}

/* ---- Raster state -------------------------------------------------------------- */

GLenum hostBlendFactor(RecompD3dBlendFactor factor, bool alpha_channel)
{
    switch (factor) {
    case RECOMP_D3D_BLEND_ZERO: return GL_ZERO;
    case RECOMP_D3D_BLEND_CONSTANT_COLOR:
    case RECOMP_D3D_BLEND_CONSTANT_ALPHA: return GL_CONSTANT_COLOR;
    case RECOMP_D3D_BLEND_INV_CONSTANT_COLOR:
    case RECOMP_D3D_BLEND_INV_CONSTANT_ALPHA: return GL_ONE_MINUS_CONSTANT_COLOR;
    case RECOMP_D3D_BLEND_SRC_COLOR: return alpha_channel ? GL_SRC_ALPHA : GL_SRC_COLOR;
    case RECOMP_D3D_BLEND_INV_SRC_COLOR: return alpha_channel ? GL_ONE_MINUS_SRC_ALPHA : GL_ONE_MINUS_SRC_COLOR;
    case RECOMP_D3D_BLEND_SRC_ALPHA: return GL_SRC_ALPHA;
    case RECOMP_D3D_BLEND_INV_SRC_ALPHA: return GL_ONE_MINUS_SRC_ALPHA;
    case RECOMP_D3D_BLEND_DST_ALPHA: return GL_DST_ALPHA;
    case RECOMP_D3D_BLEND_INV_DST_ALPHA: return GL_ONE_MINUS_DST_ALPHA;
    case RECOMP_D3D_BLEND_DST_COLOR: return alpha_channel ? GL_DST_ALPHA : GL_DST_COLOR;
    case RECOMP_D3D_BLEND_INV_DST_COLOR: return alpha_channel ? GL_ONE_MINUS_DST_ALPHA : GL_ONE_MINUS_DST_COLOR;
    case RECOMP_D3D_BLEND_SRC_ALPHA_SATURATE: return GL_SRC_ALPHA_SATURATE;
    case RECOMP_D3D_BLEND_ONE:
    default: return GL_ONE;
    }
}

GLenum hostBlendOp(RecompD3dBlendOp op)
{
    switch (op) {
    case RECOMP_D3D_BLEND_OP_MIN: return GL_MIN;
    case RECOMP_D3D_BLEND_OP_MAX: return GL_MAX;
    case RECOMP_D3D_BLEND_OP_SUBTRACT: return GL_FUNC_SUBTRACT;
    case RECOMP_D3D_BLEND_OP_REVERSE_SUBTRACT: return GL_FUNC_REVERSE_SUBTRACT;
    case RECOMP_D3D_BLEND_OP_ADD:
    default: return GL_FUNC_ADD;
    }
}

GLenum hostCompareOp(RecompD3dCompareFunc func)
{
    switch (func) {
    case RECOMP_D3D_COMPARE_NEVER: return GL_NEVER;
    case RECOMP_D3D_COMPARE_LESS: return GL_LESS;
    case RECOMP_D3D_COMPARE_EQUAL: return GL_EQUAL;
    case RECOMP_D3D_COMPARE_GREATER: return GL_GREATER;
    case RECOMP_D3D_COMPARE_NOT_EQUAL: return GL_NOTEQUAL;
    case RECOMP_D3D_COMPARE_GREATER_EQUAL: return GL_GEQUAL;
    case RECOMP_D3D_COMPARE_ALWAYS: return GL_ALWAYS;
    case RECOMP_D3D_COMPARE_LESS_EQUAL:
    default: return GL_LEQUAL;
    }
}

GLenum hostStencilOp(RecompD3dStencilOp op)
{
    switch (op) {
    case RECOMP_D3D_STENCIL_ZERO: return GL_ZERO;
    case RECOMP_D3D_STENCIL_REPLACE: return GL_REPLACE;
    case RECOMP_D3D_STENCIL_INCRSAT: return GL_INCR;
    case RECOMP_D3D_STENCIL_DECRSAT: return GL_DECR;
    case RECOMP_D3D_STENCIL_INVERT: return GL_INVERT;
    case RECOMP_D3D_STENCIL_INCRWRAP: return GL_INCR_WRAP;
    case RECOMP_D3D_STENCIL_DECRWRAP: return GL_DECR_WRAP;
    case RECOMP_D3D_STENCIL_KEEP:
    default: return GL_KEEP;
    }
}

RasterState rasterState(const RecompD3dPresenterDrawCommand &draw, GLenum primitive, RecompD3dCullMode cull,
    float slope_bias)
{
    RasterState state;
    state.primitive = primitive;
    state.cull = cull;
    state.depth_bias = draw.depth_bias;
    state.slope_bias = slope_bias;
    state.depth_test = draw.depth.depth_test_enable;
    state.depth_write = draw.depth.depth_write_enable;
    state.depth_func = hostCompareOp(draw.depth.depth_func);
    state.stencil = draw.depth.stencil_enable;
    if (state.stencil) {
        state.stencil_read = static_cast<uint8_t>(draw.depth.stencil_read_mask);
        state.stencil_write = static_cast<uint8_t>(draw.depth.stencil_write_mask);
        state.stencil_func = hostCompareOp(draw.depth.stencil_func);
        state.stencil_fail = hostStencilOp(draw.depth.stencil_fail);
        state.stencil_zfail = hostStencilOp(draw.depth.stencil_zfail);
        state.stencil_pass = hostStencilOp(draw.depth.stencil_pass);
    }
    state.blend = draw.blend.blend_enable;
    state.write_mask = static_cast<uint8_t>(draw.blend.color_write_mask & 0x0fu);
    if (state.blend) {
        state.src_rgb = hostBlendFactor(draw.blend.src_factor, false);
        state.dst_rgb = hostBlendFactor(draw.blend.dst_factor, false);
        state.src_alpha = hostBlendFactor(draw.blend.src_factor, true);
        state.dst_alpha = hostBlendFactor(draw.blend.dst_factor, true);
        state.blend_op = hostBlendOp(draw.blend.op);
    }
    return state;
}

void applyRaster(RecompD3dPresenter *presenter, const RasterState &s)
{
    RasterState &a = presenter->applied;
    const bool all = !presenter->applied_valid;
    presenter->applied_valid = true;
    if (all || s.cull != a.cull) {
        if (s.cull == RECOMP_D3D_CULL_NONE) {
            glDisable(GL_CULL_FACE);
        } else {
            glEnable(GL_CULL_FACE);
            glCullFace(GL_BACK);
            /* With UPPER_LEFT clip control GL's winding matches D3D's screen
               winding (verified on Mesa), so this is SDL_GPU's mapping:
               CULL_CLOCKWISE culls the back of counterclockwise fronts.
               RECOMP_GL_FLIP_WINDING=1 inverts it should a driver differ. */
            const bool front_ccw = (s.cull == RECOMP_D3D_CULL_CLOCKWISE) != presenter->flip_winding;
            glFrontFace(front_ccw ? GL_CCW : GL_CW);
        }
    }
    if (all || s.depth_bias != a.depth_bias || s.slope_bias != a.slope_bias) {
        if (s.depth_bias != 0 || s.slope_bias != 0.0f) {
            glEnable(GL_POLYGON_OFFSET_FILL);
            glEnable(GL_POLYGON_OFFSET_LINE);
            glPolygonOffset(s.slope_bias, static_cast<float>(s.depth_bias));
        } else {
            glDisable(GL_POLYGON_OFFSET_FILL);
            glDisable(GL_POLYGON_OFFSET_LINE);
        }
    }
    if (all || s.depth_test != a.depth_test || s.depth_write != a.depth_write || s.depth_func != a.depth_func) {
        // D3D writes depth with the test off; GL needs the test on, so a disabled test is ALWAYS.
        if (s.depth_test || s.depth_write) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
        glDepthFunc(s.depth_test ? s.depth_func : GL_ALWAYS);
        glDepthMask(s.depth_write ? GL_TRUE : GL_FALSE);
    }
    if (all || s.stencil != a.stencil || s.stencil_write != a.stencil_write ||
        s.stencil_fail != a.stencil_fail || s.stencil_zfail != a.stencil_zfail || s.stencil_pass != a.stencil_pass) {
        if (s.stencil) {
            glEnable(GL_STENCIL_TEST);
            glStencilMask(s.stencil_write);
            glStencilOp(s.stencil_fail, s.stencil_zfail, s.stencil_pass);
        } else {
            glDisable(GL_STENCIL_TEST);
        }
    }
    if (all || s.blend != a.blend || s.src_rgb != a.src_rgb || s.dst_rgb != a.dst_rgb ||
        s.src_alpha != a.src_alpha || s.dst_alpha != a.dst_alpha || s.blend_op != a.blend_op) {
        if (s.blend) {
            glEnable(GL_BLEND);
            glBlendFuncSeparate(s.src_rgb, s.dst_rgb, s.src_alpha, s.dst_alpha);
            glBlendEquation(s.blend_op);
        } else {
            glDisable(GL_BLEND);
        }
    }
    if (all || s.write_mask != a.write_mask) {
        glColorMask((s.write_mask & 1u) != 0u, (s.write_mask & 2u) != 0u, (s.write_mask & 4u) != 0u,
            (s.write_mask & 8u) != 0u);
    }
    a = s;
}

/* ---- Recording ------------------------------------------------------------------ */

uint32_t appendAligned(std::vector<uint8_t> &data, const void *bytes, size_t size, size_t alignment)
{
    const size_t offset = (data.size() + alignment - 1u) / alignment * alignment;
    data.resize(offset + size);
    std::memcpy(data.data() + offset, bytes, size);
    return static_cast<uint32_t>(offset);
}

/* Uploads one segment's bytes into a ring buffer, wrapping (and orphaning,
   so the driver need not wait for draws still reading it) when full. */
uint32_t uploadRing(GLuint buffer, uint32_t &used, uint32_t capacity, const std::vector<uint8_t> &bytes,
    uint32_t alignment)
{
    stage("upload ring");
    glBindBuffer(GL_COPY_WRITE_BUFFER, buffer);
    const uint32_t size = static_cast<uint32_t>(bytes.size());
    uint32_t base = (used + alignment - 1u) / alignment * alignment;
    if (base + size > capacity) {
        glBufferData(GL_COPY_WRITE_BUFFER, capacity, nullptr, GL_STREAM_DRAW);
        base = 0u;
    }
    if (size != 0u) glBufferSubData(GL_COPY_WRITE_BUFFER, base, size, bytes.data());
    used = base + size;
    return base;
}

void flushSegment(RecompD3dPresenter *presenter)
{
    Segment &segment = presenter->segment;
    if (segment.draws.empty()) {
        releaseRetiredTextures(presenter);
        return;
    }
    stage("flush segment");
    if (segment.vertex_data.size() > kVertexRingBytes || segment.index_data.size() > kIndexRingBytes ||
        segment.uniform_data.size() > kUniformRingBytes) {
        std::fprintf(stderr, "recomp d3d presenter: dropped %zu draws: segment exceeds the upload rings\n",
            segment.draws.size());
    } else {
        const uint32_t vertex_base = uploadRing(presenter->vertex_buffer, presenter->vertex_used, kVertexRingBytes,
            segment.vertex_data, 256u);
        const uint32_t index_base = uploadRing(presenter->index_buffer, presenter->index_used, kIndexRingBytes,
            segment.index_data, 256u);
        const uint32_t uniform_base = uploadRing(presenter->uniform_buffer, presenter->uniform_used,
            kUniformRingBytes, segment.uniform_data, static_cast<uint32_t>(presenter->uniform_align));
        bindFramebuffer(presenter, segment.color, segment.depth);
        glEnable(GL_SCISSOR_TEST);

        GLuint program = UINT32_MAX, vao = UINT32_MAX;
        uint32_t uniforms = UINT32_MAX, constants_offset = UINT32_MAX;
        uint32_t vertex_offset = UINT32_MAX, vertex_stride = 0u;
        GLuint textures[4] = {UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX};
        GLuint samplers[4] = {UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX};
        float blend[4] = {-1.0f, -1.0f, -1.0f, -1.0f};
        int stencil_key = -1;
        float viewport[6] = {NAN, NAN, NAN, NAN, NAN, NAN};
        int32_t scissor[4] = {-1, -1, -1, -1};
        for (const QueuedDraw &draw : segment.draws) {
            if (draw.program != program) {
                glUseProgram(draw.program);
                program = draw.program;
            }
            if (draw.vao != vao) {
                glBindVertexArray(draw.vao);
                // The element buffer and vertex buffer bindings are VAO state.
                glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, presenter->index_buffer);
                vao = draw.vao;
                vertex_offset = UINT32_MAX;
            }
            applyRaster(presenter, draw.raster);
            if (vertex_base + draw.vertex_offset != vertex_offset || draw.vertex_stride != vertex_stride) {
                vertex_offset = vertex_base + draw.vertex_offset;
                vertex_stride = draw.vertex_stride;
                glBindVertexBuffer(0u, presenter->vertex_buffer, vertex_offset, static_cast<GLsizei>(vertex_stride));
            }
            if (draw.uniform_offset != uniforms) {
                uniforms = draw.uniform_offset;
                const GLintptr at = uniform_base + uniforms;
                glBindBufferRange(GL_UNIFORM_BUFFER, kVertexTransformBinding, presenter->uniform_buffer, at,
                    kTransformFloats * sizeof(float));
                glBindBufferRange(GL_UNIFORM_BUFFER, kFragmentTransformBinding, presenter->uniform_buffer, at,
                    kTransformFloats * sizeof(float));
            }
            if (draw.program_offset != UINT32_MAX && draw.program_offset != constants_offset) {
                constants_offset = draw.program_offset;
                const GLintptr at = uniform_base + constants_offset;
                glBindBufferRange(GL_UNIFORM_BUFFER, kVertexProgramBinding, presenter->uniform_buffer, at,
                    kProgramFloats * sizeof(float));
                glBindBufferRange(GL_UNIFORM_BUFFER, kFragmentProgramBinding, presenter->uniform_buffer, at,
                    kProgramFloats * sizeof(float));
            }
            for (unsigned slot = 0u; slot < 4u; ++slot) {
                if (draw.textures[slot] != textures[slot]) {
                    textures[slot] = draw.textures[slot];
                    glActiveTexture(GL_TEXTURE0 + slot);
                    glBindTexture(slot == 3u ? GL_TEXTURE_3D : GL_TEXTURE_2D, textures[slot]);
                }
                if (draw.samplers[slot] != samplers[slot]) {
                    samplers[slot] = draw.samplers[slot];
                    glBindSampler(slot, samplers[slot]);
                }
            }
            if (std::memcmp(draw.blend_constants, blend, sizeof blend) != 0) {
                std::memcpy(blend, draw.blend_constants, sizeof blend);
                glBlendColor(blend[0], blend[1], blend[2], blend[3]);
            }
            if (draw.raster.stencil) {
                const int key = draw.stencil_ref | (static_cast<int>(draw.raster.stencil_read) << 8) |
                    (static_cast<int>(draw.raster.stencil_func & 0xffu) << 16);
                if (key != stencil_key) {
                    stencil_key = key;
                    glStencilFunc(draw.raster.stencil_func, draw.stencil_ref, draw.raster.stencil_read);
                }
            }
            if (std::memcmp(draw.viewport, viewport, sizeof viewport) != 0) {
                std::memcpy(viewport, draw.viewport, sizeof viewport);
                glViewportIndexedf(0u, viewport[0], viewport[1] - kRowTieBias, viewport[2], viewport[3]);
                glDepthRangef(viewport[4], viewport[5]);
            }
            if (std::memcmp(draw.scissor, scissor, sizeof scissor) != 0) {
                std::memcpy(scissor, draw.scissor, sizeof scissor);
                glScissor(scissor[0], scissor[1], scissor[2], scissor[3]);
            }
            glDrawElements(draw.raster.primitive, static_cast<GLsizei>(draw.index_count), GL_UNSIGNED_SHORT,
                reinterpret_cast<const void *>(static_cast<uintptr_t>(index_base + draw.index_offset)));
        }
        glBindVertexArray(0u);
        glDisable(GL_SCISSOR_TEST);
        stage("segment drawn");
    }
    segment.draws.clear();
    segment.vertex_data.clear();
    segment.index_data.clear();
    segment.uniform_data.clear();
    releaseRetiredTextures(presenter);
}

void useTarget(RecompD3dPresenter *presenter, const BoundTarget &bound)
{
    Segment &segment = presenter->segment;
    if (segment.color != bound.color || segment.depth != bound.depth) {
        flushSegment(presenter);
        segment.color = bound.color;
        segment.depth = bound.depth;
        segment.width = bound.host_width;
        segment.height = bound.host_height;
    }
}

RecompD3dPresenterError submitClear(RecompD3dPresenter *presenter, const RecompD3dPresenterClearCommand &clear)
{
    if ((clear.clear_depth && (!std::isfinite(clear.z) || clear.z < 0.0f || clear.z > 1.0f)) ||
        (clear.clear_stencil && clear.stencil > 0xffu)) {
        return RECOMP_D3D_PRESENTER_UNSUPPORTED_COMMAND;
    }
    BoundTarget bound;
    const RecompD3dPresenterError error = bindTarget(presenter, clear.target, bound);
    if (error != RECOMP_D3D_PRESENTER_OK) return error;
    flushSegment(presenter);
    /* The guest ignores depth/stencil clear bits when no depth is attached. */
    const GLuint depth = (clear.clear_depth || clear.clear_stencil) ? bound.depth : 0u;
    if (clear.clear_color || depth != 0u) {
        // Clear the whole target: a depth-only clear binds the color it pairs with.
        bindFramebuffer(presenter, bound.color, bound.depth);
        glDisable(GL_SCISSOR_TEST);
        if (clear.clear_color) {
            constexpr float byte_to_float = 1.0f / 255.0f;
            const float color[4] = {
                static_cast<float>((clear.color >> 16u) & 0xffu) * byte_to_float,
                static_cast<float>((clear.color >> 8u) & 0xffu) * byte_to_float,
                static_cast<float>(clear.color & 0xffu) * byte_to_float,
                static_cast<float>((clear.color >> 24u) & 0xffu) * byte_to_float,
            };
            glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            glClearBufferfv(GL_COLOR, 0, color);
        }
        if (depth != 0u) {
            if (clear.clear_depth) glDepthMask(GL_TRUE);
            if (clear.clear_stencil) glStencilMask(0xffu);
            if (clear.clear_depth && clear.clear_stencil) {
                glClearBufferfi(GL_DEPTH_STENCIL, 0, clear.z, static_cast<GLint>(clear.stencil));
            } else if (clear.clear_depth) {
                glClearBufferfv(GL_DEPTH, 0, &clear.z);
            } else {
                const GLint stencil = static_cast<GLint>(clear.stencil);
                glClearBufferiv(GL_STENCIL, 0, &stencil);
            }
        }
        presenter->applied_valid = false;  // masks changed
    }
    presenter->last_bound = bound;
    return RECOMP_D3D_PRESENTER_OK;
}

struct DrawBindings {
    GLuint textures[4];
    GLuint samplers[4];
    float blend_constants[4];
    float viewport[6];
    int32_t scissor[4];
};

void queueDraw(RecompD3dPresenter *presenter, GLuint program, GLuint vao, const RasterState &raster,
    const void *vertices, uint32_t vertex_bytes, uint32_t stride, const void *indices, uint32_t index_count,
    const float *constants, bool program_constants, const DrawBindings &bindings, uint8_t stencil_ref)
{
    Segment &segment = presenter->segment;
    QueuedDraw queued;
    queued.program = program;
    queued.vao = vao;
    queued.raster = raster;
    queued.vertex_offset = appendAligned(segment.vertex_data, vertices, vertex_bytes, 16u);
    queued.vertex_stride = stride;
    queued.index_offset = appendAligned(segment.index_data, indices, index_count * sizeof(uint16_t), 16u);
    queued.index_count = index_count;
    /* As upstream: the Transform buffer is the draw constants without vc[],
       which goes to its own buffer; consecutive draws often repeat both. */
    float transform[kTransformFloats];
    std::memcpy(transform, constants, 140u * sizeof(float));
    std::memcpy(transform + 140u, constants + 140u + kProgramFloats, (kTransformFloats - 140u) * sizeof(float));
    std::vector<uint8_t> &uniforms = segment.uniform_data;
    const size_t alignment = static_cast<size_t>(presenter->uniform_align);
    if (!segment.draws.empty() &&
        segment.draws.back().uniform_offset + sizeof transform <= uniforms.size() &&
        std::memcmp(uniforms.data() + segment.draws.back().uniform_offset, transform, sizeof transform) == 0) {
        queued.uniform_offset = segment.draws.back().uniform_offset;
    } else {
        queued.uniform_offset = appendAligned(uniforms, transform, sizeof transform, alignment);
    }
    if (program_constants) {
        const float *vc = constants + 140u;
        if (!segment.draws.empty() && segment.draws.back().program_offset != UINT32_MAX &&
            std::memcmp(uniforms.data() + segment.draws.back().program_offset, vc,
                kProgramFloats * sizeof(float)) == 0) {
            queued.program_offset = segment.draws.back().program_offset;
        } else {
            queued.program_offset = appendAligned(uniforms, vc, kProgramFloats * sizeof(float), alignment);
        }
    }
    std::copy(std::begin(bindings.textures), std::end(bindings.textures), queued.textures);
    std::copy(std::begin(bindings.samplers), std::end(bindings.samplers), queued.samplers);
    std::copy(std::begin(bindings.blend_constants), std::end(bindings.blend_constants), queued.blend_constants);
    std::copy(std::begin(bindings.viewport), std::end(bindings.viewport), queued.viewport);
    std::copy(std::begin(bindings.scissor), std::end(bindings.scissor), queued.scissor);
    queued.stencil_ref = stencil_ref;
    segment.draws.push_back(queued);
}

/* Upstream's CPU widening of XYZ|DIFFUSE lines (its geometry-shader stand-in). */
struct LineVertex {
    float clip[4];
    float color[4];
};

struct WideVertex {
    float x, y, z, rhw;
    uint32_t diffuse;
};

uint32_t packColor(const float (&color)[4])
{
    const auto byte = [](float value) {
        return static_cast<uint32_t>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
    };
    return byte(color[2]) | byte(color[1]) << 8u | byte(color[0]) << 16u | byte(color[3]) << 24u;
}

void expandLine(LineVertex a, LineVertex b, const float *style, std::vector<WideVertex> &out)
{
    if (style[0] <= 0.0f) return;
    for (unsigned plane = 0u; plane < 3u; ++plane) {
        const auto distance = [plane](const LineVertex &v) {
            return plane == 0u ? v.clip[2] : plane == 1u ? v.clip[3] - v.clip[2] : v.clip[3] - 1e-6f;
        };
        const float da = distance(a), db = distance(b);
        if (da < 0.0f && db < 0.0f) return;
        if (da < 0.0f || db < 0.0f) {
            const float t = da / (da - db);
            LineVertex c{};
            for (unsigned i = 0u; i < 4u; ++i) {
                c.clip[i] = a.clip[i] + (b.clip[i] - a.clip[i]) * t;
                c.color[i] = a.color[i] + (b.color[i] - a.color[i]) * t;
            }
            (da < 0.0f ? a : b) = c;
        }
    }
    const float dx = (b.clip[0] / b.clip[3] - a.clip[0] / a.clip[3]) / style[2];
    const float dy = (b.clip[1] / b.clip[3] - a.clip[1] / a.clip[3]) / style[3];
    const float length = std::sqrt(dx * dx + dy * dy);
    if (length < 1e-6f) return;
    float side[2];
    if (style[1] > 0.0f) {
        side[0] = -dy / length;
        side[1] = dx / length;
    } else if (std::fabs(dx) >= std::fabs(dy)) {
        side[0] = 0.0f;
        side[1] = 1.0f;
    } else {
        side[0] = 1.0f;
        side[1] = 0.0f;
    }
    const unsigned layers = style[1] > 0.0f ? 4u : 2u;
    std::vector<WideVertex> strip;
    for (unsigned i = 0u; i < layers; ++i) {
        const float offset = style[1] > 0.0f
            ? (i == 0u ? -(style[0] + 1.0f) * 0.5f : i == 1u ? -std::fabs(style[0] - 1.0f) * 0.5f
                : i == 2u ? std::fabs(style[0] - 1.0f) * 0.5f : (style[0] + 1.0f) * 0.5f)
            : (i == 0u ? -style[0] * 0.5f : style[0] * 0.5f);
        const float coverage = style[1] > 0.0f ? (i == 0u || i == 3u ? 0.0f : std::min(style[0], 1.0f)) : 1.0f;
        for (const LineVertex *end : {&a, &b}) {
            float clip[4] = {end->clip[0], end->clip[1], end->clip[2], end->clip[3]};
            clip[0] += side[0] * offset * style[2] * clip[3];
            clip[1] += side[1] * offset * style[3] * clip[3];
            float color[4] = {end->color[0], end->color[1], end->color[2], end->color[3] * coverage};
            strip.push_back({clip[0] / clip[3], clip[1] / clip[3], clip[2] / clip[3], 1.0f / clip[3],
                packColor(color)});
        }
    }
    for (size_t i = 2u; i < strip.size(); ++i) {
        out.push_back(strip[i - 2u]);
        out.push_back(strip[i - 1u]);
        out.push_back(strip[i]);
    }
}

/* ---- Visibility -------------------------------------------------------------------- */

GLuint takeQuery(RecompD3dPresenter *presenter)
{
    if (!presenter->free_queries.empty()) {
        const GLuint query = presenter->free_queries.back();
        presenter->free_queries.pop_back();
        return query;
    }
    GLuint query = 0u;
    glGenQueries(1, &query);
    return query;
}

/* Starts counting on the first draw inside a test, so a test that draws
   nothing needs no query. Queued draws so far are outside it. */
void openQuery(RecompD3dPresenter *presenter)
{
    if (presenter->visibility_active.query != 0u) return;
    flushSegment(presenter);
    presenter->visibility_active.query = takeQuery(presenter);
    glBeginQuery(GL_SAMPLES_PASSED, presenter->visibility_active.query);
}

/* A repeated BEGIN restarts counting, as the SDK's CLEAR_REPORT_VALUE does. */
RecompD3dPresenterError beginVisibility(RecompD3dPresenter *presenter)
{
    if (presenter->visibility_open) {
        if (presenter->visibility_active.query != 0u) {
            flushSegment(presenter);
            glEndQuery(GL_SAMPLES_PASSED);
            presenter->free_queries.push_back(presenter->visibility_active.query);
            presenter->visibility_active.query = 0u;
        }
        return RECOMP_D3D_PRESENTER_OK;
    }
    presenter->visibility_active = {};
    presenter->visibility_open = true;
    return RECOMP_D3D_PRESENTER_OK;
}

RecompD3dPresenterError endVisibility(RecompD3dPresenter *presenter, uint32_t index, const BoundTarget &last)
{
    if (!presenter->visibility_open) return RECOMP_D3D_PRESENTER_UNSUPPORTED_COMMAND;
    presenter->visibility_open = false;
    PendingQuery query = presenter->visibility_active;
    presenter->visibility_active = {};
    query.index = index;
    query.host_pixels_per_guest_pixel = last.scale_x * last.scale_y;
    query.ended = std::chrono::steady_clock::now();
    if (query.query != 0u) {
        flushSegment(presenter);
        glEndQuery(GL_SAMPLES_PASSED);
        // The result is polled later; flushing lets the GPU finish it meanwhile.
        stage("glFlush (visibility end)");
        glFlush();
    }
    presenter->visibility_pending.push_back(query);
    return RECOMP_D3D_PRESENTER_OK;
}

/* ---- Coverage (upstream's CPU rasterizer, single-sampled) -------------------------- */

bool triangleCovers(const float (&clip)[3][4], RecompD3dCullMode cull, const float (&rect)[4], const float (&test)[4])
{
    std::vector<std::array<float, 4>> polygon = {
        {clip[0][0], clip[0][1], clip[0][2], clip[0][3]},
        {clip[1][0], clip[1][1], clip[1][2], clip[1][3]},
        {clip[2][0], clip[2][1], clip[2][2], clip[2][3]}};
    for (unsigned plane = 0u; plane < 3u && !polygon.empty(); ++plane) {
        const auto distance = [plane](const std::array<float, 4> &v) {
            return plane == 0u ? v[2] : plane == 1u ? v[3] - v[2] : v[3] - 1e-6f;
        };
        std::vector<std::array<float, 4>> kept;
        for (size_t i = 0u; i < polygon.size(); ++i) {
            const auto &a = polygon[i], &b = polygon[(i + 1u) % polygon.size()];
            const float da = distance(a), db = distance(b);
            if (da >= 0.0f) kept.push_back(a);
            if ((da >= 0.0f) != (db >= 0.0f)) {
                const float t = da / (da - db);
                std::array<float, 4> c;
                for (unsigned k = 0u; k < 4u; ++k) c[k] = a[k] + (b[k] - a[k]) * t;
                kept.push_back(c);
            }
        }
        polygon.swap(kept);
    }
    if (polygon.size() < 3u) return false;
    const auto snap = [](float value) { return std::nearbyint(value * 256.0f) / 256.0f; };
    std::vector<std::array<float, 2>> screen;
    for (const auto &v : polygon) {
        screen.push_back({snap(rect[0] + (v[0] / v[3] + 1.0f) * 0.5f * (rect[2] - rect[0])),
            snap(rect[1] + (1.0f - v[1] / v[3]) * 0.5f * (rect[3] - rect[1]))});
    }
    float area = 0.0f;
    for (size_t i = 0u; i < screen.size(); ++i) {
        const auto &a = screen[i], &b = screen[(i + 1u) % screen.size()];
        area += a[0] * b[1] - b[0] * a[1];
    }
    if (area == 0.0f) return false;
    if (cull == RECOMP_D3D_CULL_CLOCKWISE && area > 0.0f) return false;
    if (cull == RECOMP_D3D_CULL_COUNTER_CLOCKWISE && area < 0.0f) return false;
    float min_x = test[2], min_y = test[3], max_x = test[0], max_y = test[1];
    for (const auto &p : screen) {
        min_x = std::min(min_x, p[0]);
        min_y = std::min(min_y, p[1]);
        max_x = std::max(max_x, p[0]);
        max_y = std::max(max_y, p[1]);
    }
    const int x0 = static_cast<int>(std::floor(std::max(min_x, test[0])));
    const int y0 = static_cast<int>(std::floor(std::max(min_y, test[1])));
    const int x1 = static_cast<int>(std::ceil(std::min(max_x, test[2])));
    const int y1 = static_cast<int>(std::ceil(std::min(max_y, test[3])));
    const float sign = area > 0.0f ? 1.0f : -1.0f;
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            const float px = x + 0.5f, py = y + 0.5f;
            bool inside = px >= test[0] && px < test[2] && py >= test[1] && py < test[3];
            for (size_t i = 0u; i < screen.size() && inside; ++i) {
                const auto &a = screen[i], &b = screen[(i + 1u) % screen.size()];
                inside = sign * ((b[0] - a[0]) * (py - a[1]) - (b[1] - a[1]) * (px - a[0])) >= 0.0f;
            }
            if (inside) return true;
        }
    }
    return false;
}

/* ---- Draws ----------------------------------------------------------------------- */

RecompD3dPresenterError submitDraw(RecompD3dPresenter *presenter, const RecompD3dPresenterDrawCommand &draw)
{
    const bool screen_offset = draw.screen_space_offset[0] != 0.0f || draw.screen_space_offset[1] != 0.0f;
    const bool geometry_line = recomp_d3d_draw_geometry_line(draw);
    if (draw.vertex_bytes == nullptr || draw.index_bytes == nullptr ||
        (draw.texture_factor_color_scale != 0u && draw.texture_factor_color_scale != 2u &&
         draw.texture_factor_color_scale != 4u) ||
        draw.vertex_stride == 0u || draw.vertex_count == 0u ||
        draw.index_count == 0u || !std::isfinite(draw.line_width) || draw.line_width < 0.0f ||
        (geometry_line && (draw.fvf != 0x42u || draw.program_count || draw.primitive_type != 2u ||
            draw.has_texture || draw.directional.enabled)) ||
        static_cast<unsigned>(draw.cull_mode) > RECOMP_D3D_CULL_COUNTER_CLOCKWISE ||
        !std::isfinite(draw.slope_scaled_depth_bias) ||
        !std::isfinite(draw.screen_space_offset[0]) || !std::isfinite(draw.screen_space_offset[1]) ||
        (screen_offset && draw.program_count) ||
        !std::isfinite(draw.mip_lod_bias) ||
        draw.mip_lod_bias < kMipLodBiasMin || draw.mip_lod_bias > kMipLodBiasMax ||
        !std::isfinite(draw.reflection_mip_lod_bias) ||
        draw.reflection_mip_lod_bias < kMipLodBiasMin || draw.reflection_mip_lod_bias > kMipLodBiasMax) {
        return RECOMP_D3D_PRESENTER_UNSUPPORTED_COMMAND;
    }
    const bool sub_viewport = draw.has_transform && !draw.program_count && draw.viewport[2] > 0.0f;

    RecompD3dVertexLayout layout;
    if (!recomp_d3d_fvf_layout(draw.fvf, &layout) || layout.stride > draw.vertex_stride ||
        (!layout.pretransformed && !draw.has_transform && !draw.program_count) ||
        (draw.blend_weight_count != 0u && layout.blend_weight_count != draw.blend_weight_count)) {
        return RECOMP_D3D_PRESENTER_UNSUPPORTED_COMMAND;
    }
    GLenum primitive;
    switch (draw.primitive_type) {
    case 2u:
        if (draw.index_count < 2u || draw.index_count % 2u != 0u) return RECOMP_D3D_PRESENTER_UNSUPPORTED_COMMAND;
        primitive = geometry_line ? GL_TRIANGLES : GL_LINES;
        break;
    case 6u: primitive = GL_TRIANGLE_STRIP; break;
    case 7u: primitive = GL_TRIANGLE_FAN; break;
    default: primitive = GL_TRIANGLES; break;
    }

    const bool scaled_bias = presenter->scale != 1.0f &&
        (!draw.target.offscreen || (draw.target.color.width == presenter->config.width &&
            draw.target.color.height == presenter->config.height)) &&
        draw.slope_scaled_depth_bias != 0.0f;
    if (draw.program_count > 136u) return RECOMP_D3D_PRESENTER_UNSUPPORTED_COMMAND;
    const uint32_t shader_fvf = geometry_line ? 0x44u : draw.fvf;
    // Single-sampled, so the sample-position variant of the scaled bias never applies.
    const RecompD3dDrawShaderKey shader_key{shader_fvf, draw.program_count,
        draw.program_count ? draw.program : nullptr, false, scaled_bias, false};
    const ProgramEntry &program = lookupProgram(presenter, shader_key);
    if (program.program == 0u) return RECOMP_D3D_PRESENTER_UNSUPPORTED_COMMAND;

    BoundTarget bound;
    const RecompD3dPresenterError target_error = bindTarget(presenter, draw.target, bound);
    if (target_error != RECOMP_D3D_PRESENTER_OK) return target_error;
    int32_t scissor[4] = {0, 0, static_cast<int32_t>(bound.host_width), static_cast<int32_t>(bound.host_height)};
    if (draw.has_scissor) {
        if (draw.scissor[0] < 0 || draw.scissor[1] < 0 ||
            draw.scissor[2] < draw.scissor[0] || draw.scissor[3] < draw.scissor[1])
            return RECOMP_D3D_PRESENTER_UNSUPPORTED_COMMAND;
        double edges[4];
        for (unsigned i = 0u; i < 4u; ++i) {
            edges[i] = draw.scissor[i] * static_cast<double>(i % 2u == 0u ? bound.scale_x : bound.scale_y);
            if (!std::isfinite(edges[i]) || edges[i] > 32767.0) return RECOMP_D3D_PRESENTER_UNSUPPORTED_COMMAND;
            edges[i] = std::floor(edges[i] + 0.5);
        }
        const int left = static_cast<int>(std::min<double>(edges[0], bound.host_width));
        const int top = static_cast<int>(std::min<double>(edges[1], bound.host_height));
        const int right = static_cast<int>(std::min<double>(edges[2], bound.host_width));
        const int bottom = static_cast<int>(std::min<double>(edges[3], bound.host_height));
        scissor[0] = left;
        scissor[1] = top;
        scissor[2] = right - left;
        scissor[3] = bottom - top;
    }
    if (draw.has_texture) {
        const RenderTargetEntry *sampled = findRenderTarget(presenter, draw.texture);
        if (sampled != nullptr && sampled->texture == bound.color) {
            std::fprintf(stderr, "recomp d3d presenter: cannot sample active render target data=0x%08X\n",
                draw.texture.data);
            return RECOMP_D3D_PRESENTER_UNSUPPORTED_COMMAND;
        }
    }
    useTarget(presenter, bound);

    const GLuint texture = draw.has_texture ? lookupTexture(presenter, draw) : 0u;
    GLuint mask = 0u, bump = 0u;
    if (draw.has_alpha_mask) {
        if (layout.texcoord_count != 2u) return RECOMP_D3D_PRESENTER_UNSUPPORTED_COMMAND;
        RecompD3dPresenterDrawCommand lookup{};
        lookup.texture = draw.alpha_mask;
        lookup.texture_bytes = draw.alpha_mask_bytes;
        lookup.texture_byte_count = draw.alpha_mask_byte_count;
        lookup.palette_bytes = draw.alpha_mask_palette;
        lookup.palette_byte_count = draw.alpha_mask_palette_byte_count;
        mask = lookupTexture(presenter, lookup);
        if (mask == 0u || texture == 0u) return RECOMP_D3D_PRESENTER_UNSUPPORTED_COMMAND;
    }
    const auto reflectionLookup = [&](const RecompD3dTextureDesc &desc, const void *bytes, uint32_t count,
                                      bool bump_env = false) {
        RecompD3dPresenterDrawCommand lookup{};
        lookup.bump_env = bump_env;
        lookup.texture = desc;
        lookup.texture_bytes = bytes;
        lookup.texture_byte_count = count;
        return lookupTexture(presenter, lookup);
    };
    if (draw.has_reflection || draw.program_alpha_mask) {
        if (draw.has_alpha_mask || draw.four_tap_filter || layout.pretransformed ||
            (draw.has_reflection && draw.program_alpha_mask) ||
            (draw.program_alpha_mask && layout.normal_offset == RECOMP_D3D_FVF_ABSENT) ||
            layout.texcoord_count == 0u) return RECOMP_D3D_PRESENTER_UNSUPPORTED_COMMAND;
        mask = reflectionLookup(draw.reflection_texture, draw.reflection_bytes, draw.reflection_byte_count);
        if (mask == 0u || texture == 0u) return RECOMP_D3D_PRESENTER_UNSUPPORTED_COMMAND;
    }
    if (draw.distortion) {
        if (draw.has_alpha_mask || draw.has_reflection || draw.program_alpha_mask ||
            draw.four_tap_filter || layout.texcoord_count != 4u) return RECOMP_D3D_PRESENTER_UNSUPPORTED_COMMAND;
        mask = reflectionLookup(draw.reflection_texture, draw.reflection_bytes, draw.reflection_byte_count);
        if (mask == 0u || texture == 0u) return RECOMP_D3D_PRESENTER_UNSUPPORTED_COMMAND;
    }
    if (draw.stage1_alpha_mask) {
        if (draw.has_alpha_mask || draw.has_reflection || draw.program_count || draw.distortion ||
            draw.four_tap_filter || (draw.stage1_kill_only ? layout.texcoord_count == 0u
                : layout.texcoord_count != 2u)) return RECOMP_D3D_PRESENTER_UNSUPPORTED_COMMAND;
        mask = reflectionLookup(draw.reflection_texture, draw.reflection_bytes, draw.reflection_byte_count);
        if (mask == 0u || texture == 0u) return RECOMP_D3D_PRESENTER_UNSUPPORTED_COMMAND;
    }
    if (draw.reflection_stage2) {
        if (!draw.has_reflection || draw.program_count || draw.bump_env || draw.distortion ||
            draw.reflection_premultiplied || draw.reflection_skip_diffuse)
            return RECOMP_D3D_PRESENTER_UNSUPPORTED_COMMAND;
        bump = reflectionLookup(draw.reflection_stage2_texture, draw.reflection_stage2_bytes,
            draw.reflection_stage2_byte_count);
        if (bump == 0u || mask == 0u || texture == 0u) return RECOMP_D3D_PRESENTER_UNSUPPORTED_COMMAND;
    }
    if (draw.bump_env) {
        if (!draw.program_count || draw.has_alpha_mask || draw.has_reflection || draw.program_alpha_mask ||
            draw.distortion || draw.stage1_alpha_mask || draw.four_tap_filter || draw.bright_pass)
            return RECOMP_D3D_PRESENTER_UNSUPPORTED_COMMAND;
        mask = reflectionLookup(draw.reflection_texture, draw.reflection_bytes, draw.reflection_byte_count);
        bump = reflectionLookup(draw.alpha_mask, draw.alpha_mask_bytes, draw.alpha_mask_byte_count, true);
        if (bump == 0u || mask == 0u || texture == 0u) return RECOMP_D3D_PRESENTER_UNSUPPORTED_COMMAND;
    }
    if (draw.four_tap_filter && (texture == 0u ||
            !std::isfinite(draw.four_tap_scale) || !std::isfinite(draw.four_tap_sum_scale))) {
        return RECOMP_D3D_PRESENTER_UNSUPPORTED_COMMAND;
    }
    if (draw.bright_pass && (texture == 0u || draw.four_tap_filter ||
            draw.has_alpha_mask || draw.has_reflection || draw.program_count ||
            layout.texcoord_count != 1u || draw.texture.width == 0u || draw.texture.height == 0u)) {
        return RECOMP_D3D_PRESENTER_UNSUPPORTED_COMMAND;
    }
    if (draw.luminance_tint && (texture == 0u || draw.four_tap_filter || draw.bright_pass ||
            draw.has_alpha_mask || draw.has_reflection || draw.program_count || layout.texcoord_count != 1u)) {
        return RECOMP_D3D_PRESENTER_UNSUPPORTED_COMMAND;
    }
    countDrawTexture(draw, texture != 0u);

    RecompD3dDrawTargetState target_state{};
    target_state.viewport[2] = static_cast<float>(bound.host_width);
    target_state.viewport[3] = static_cast<float>(bound.host_height);
    target_state.viewport[5] = 1.0f;
    target_state.scale_x = bound.scale_x;
    target_state.scale_y = bound.scale_y;
    float constants[kRecompD3dDrawConstantFloats];
    if (!recomp_d3d_draw_constants(draw, layout, target_state, texture != 0u, scaled_bias, constants)) {
        return RECOMP_D3D_PRESENTER_UNSUPPORTED_COMMAND;
    }

    const auto *source_indices = static_cast<const uint16_t *>(draw.index_bytes);
    const void *vertices = draw.vertex_bytes;
    uint32_t vertex_bytes = draw.vertex_stride * draw.vertex_count;
    uint32_t stride = draw.vertex_stride;
    std::vector<uint16_t> expanded_indices;
    std::vector<WideVertex> wide;
    const void *indices = draw.index_bytes;
    uint32_t index_count = draw.index_count;
    if (geometry_line) {
        const float *style = constants + 140u + kProgramFloats + 340u + 8u;
        const auto fetch = [&](uint16_t index, LineVertex &vertex) {
            if (index >= draw.vertex_count) return false;
            const auto *bytes = static_cast<const uint8_t *>(draw.vertex_bytes) +
                static_cast<size_t>(index) * draw.vertex_stride;
            float position[3];
            uint32_t diffuse;
            std::memcpy(position, bytes + layout.position_offset, sizeof position);
            std::memcpy(&diffuse, bytes + layout.diffuse_offset, sizeof diffuse);
            for (unsigned column = 0u; column < 4u; ++column) {
                vertex.clip[column] = position[0] * constants[column] + position[1] * constants[4u + column] +
                    position[2] * constants[8u + column] + constants[12u + column];
            }
            vertex.color[0] = ((diffuse >> 16u) & 0xffu) / 255.0f;
            vertex.color[1] = ((diffuse >> 8u) & 0xffu) / 255.0f;
            vertex.color[2] = (diffuse & 0xffu) / 255.0f;
            vertex.color[3] = (diffuse >> 24u) / 255.0f;
            return true;
        };
        for (uint32_t i = 0u; i + 1u < draw.index_count; i += 2u) {
            LineVertex a{}, b{};
            if (!fetch(source_indices[i], a) || !fetch(source_indices[i + 1u], b)) {
                return RECOMP_D3D_PRESENTER_UNSUPPORTED_COMMAND;
            }
            expandLine(a, b, style, wide);
        }
        if (wide.empty() || wide.size() > 65535u) return wide.empty() ? RECOMP_D3D_PRESENTER_OK
            : RECOMP_D3D_PRESENTER_UNSUPPORTED_COMMAND;
        std::fill(constants, constants + 16, 0.0f);
        constants[0] = constants[5] = constants[10] = constants[15] = 1.0f;
        expanded_indices.resize(wide.size());
        for (size_t i = 0u; i < wide.size(); ++i) expanded_indices[i] = static_cast<uint16_t>(i);
        vertices = wide.data();
        vertex_bytes = static_cast<uint32_t>(wide.size() * sizeof(WideVertex));
        stride = sizeof(WideVertex);
        indices = expanded_indices.data();
        index_count = static_cast<uint32_t>(wide.size());
    }

    const RasterState raster = rasterState(draw, primitive, geometry_line ? RECOMP_D3D_CULL_NONE : draw.cull_mode,
        scaled_bias ? 0.0f : draw.slope_scaled_depth_bias);
    const GLuint vao = lookupVertexArray(presenter, shader_fvf, program.layout);

    const bool volume = draw.texture.volume_depth != 0u;
    DrawBindings bindings{};
    bindings.textures[0] = !volume && texture != 0u ? texture : presenter->white_texture;
    bindings.textures[1] = mask != 0u ? mask : presenter->white_texture;
    bindings.textures[2] = bump != 0u ? bump : presenter->white_texture;
    bindings.textures[3] = volume && texture != 0u ? texture : presenter->white_volume;
    bindings.samplers[0] = draw.four_tap_filter || draw.has_alpha_mask ||
            (draw.program_count && (!draw.address_u || !draw.address_v))
        ? presenter->filter_sampler
        : lookupDrawSampler(presenter, draw.address_u, draw.address_v, draw.point_mip_filter,
            draw.mip_lod_bias, volume ? draw.address_w : 1u);
    bindings.samplers[1] = draw.program_alpha_mask ? presenter->program_mask_sampler
        : draw.has_reflection || draw.distortion || draw.stage1_alpha_mask || draw.bump_env
        ? lookupDrawSampler(presenter, draw.reflection_address_u, draw.reflection_address_v,
            draw.reflection_point_mip_filter, draw.reflection_mip_lod_bias)
        : presenter->filter_sampler;
    bindings.samplers[2] = draw.reflection_stage2
        ? lookupDrawSampler(presenter, draw.reflection_stage2_address_u, draw.reflection_stage2_address_v,
            draw.reflection_stage2_point_mip_filter, draw.reflection_stage2_mip_lod_bias)
        : draw.bump_env ? lookupDrawSampler(presenter, draw.bump_address_u, draw.bump_address_v, false, 0.0f)
        : presenter->filter_sampler;
    // The volume samples with the stage-0 sampler, as guest_sampler upstream.
    bindings.samplers[3] = bindings.samplers[0];
    {
        const uint32_t color = draw.blend.constant_color;
        const float alpha = ((color >> 24u) & 255u) / 255.0f;
        const auto alpha_factor = [](RecompD3dBlendFactor factor) {
            return factor == RECOMP_D3D_BLEND_CONSTANT_ALPHA || factor == RECOMP_D3D_BLEND_INV_CONSTANT_ALPHA;
        };
        const bool constant_alpha = alpha_factor(draw.blend.src_factor) || alpha_factor(draw.blend.dst_factor);
        bindings.blend_constants[0] = constant_alpha ? alpha : ((color >> 16u) & 255u) / 255.0f;
        bindings.blend_constants[1] = constant_alpha ? alpha : ((color >> 8u) & 255u) / 255.0f;
        bindings.blend_constants[2] = constant_alpha ? alpha : (color & 255u) / 255.0f;
        bindings.blend_constants[3] = alpha;
    }
    if (sub_viewport) {
        const float viewport[6] = {draw.viewport[0] * bound.scale_x, draw.viewport[1] * bound.scale_y,
            draw.viewport[2] * bound.scale_x, draw.viewport[3] * bound.scale_y, draw.viewport[4], draw.viewport[5]};
        std::copy(viewport, viewport + 6, bindings.viewport);
    } else {
        const float viewport[6] = {0.0f, 0.0f, static_cast<float>(bound.host_width),
            static_cast<float>(bound.host_height), 0.0f, 1.0f};
        std::copy(viewport, viewport + 6, bindings.viewport);
    }
    std::copy(scissor, scissor + 4, bindings.scissor);

    if (presenter->visibility_open) {
        openQuery(presenter);
        // openQuery flushed the segment; this draw records into the same target again.
    }
    queueDraw(presenter, program.program, vao, raster, vertices, vertex_bytes, stride, indices, index_count,
        constants, draw.program_count != 0u, bindings, static_cast<uint8_t>(draw.depth.stencil_ref));
    presenter->last_bound = bound;
    ++presenter->draw_count;
    return RECOMP_D3D_PRESENTER_OK;
}

/* ---- Presentation ----------------------------------------------------------------- */

const char kPresentVertex[] = R"(#version 430 core
layout(location = 0) out vec2 v_uv;
uniform vec4 u_rect; // destination x, y, w, h in NDC units (lower-left origin)
void main() {
    vec2 p = vec2(float(gl_VertexID & 1), float((gl_VertexID >> 1) & 1));
    v_uv = vec2(p.x, 1.0 - p.y);  // texture row 0 is the top of the frame
    gl_Position = vec4(u_rect.xy + p * u_rect.zw * 2.0, 0.0, 1.0);
}
)";

const char kPresentFragment[] = R"(#version 430 core
layout(binding = 0) uniform sampler2D frame;
layout(std140, binding = 4) uniform GammaRamp { vec4 ramp[256]; } g;
uniform int u_gamma;
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;
void main() {
    vec4 c = texture(frame, v_uv);
    if (u_gamma != 0) {
        uvec3 i = uvec3(clamp(c.rgb, 0.0, 1.0) * 255.0 + 0.5);
        c.rgb = vec3(g.ramp[i.r].r, g.ramp[i.g].g, g.ramp[i.b].b);
    }
    out_color = vec4(c.rgb, 1.0);
}
)";

RecompD3dPresenterError submitGamma(RecompD3dPresenter *presenter, const uint8_t (&ramp)[3][256])
{
    bool enabled = false;
    float values[256][4]{};
    for (unsigned i = 0u; i < 256u; ++i) {
        for (unsigned channel = 0u; channel < 3u; ++channel) {
            values[i][channel] = ramp[channel][i] / 255.0f;
            enabled |= ramp[channel][i] != i;
        }
    }
    std::memcpy(presenter->gamma_ramp, values, sizeof values);
    presenter->gamma_enabled = enabled;
    glBindBuffer(GL_COPY_WRITE_BUFFER, presenter->gamma_buffer);
    glBufferSubData(GL_COPY_WRITE_BUFFER, 0, sizeof values, values);
    return RECOMP_D3D_PRESENTER_OK;
}

/* Writes BGRA rows, top row first, as a bottom-up 24-bit BMP. */
void writeBmp(const char *path, const uint8_t *pixels, uint32_t width, uint32_t height)
{
    const uint32_t row_bytes = width * 3u;
    const uint32_t padded = (row_bytes + 3u) & ~3u;
    const uint32_t image_bytes = padded * height;
    FILE *file = std::fopen(path, "wb");
    if (file == nullptr) return;
    uint8_t header[54]{};
    const uint32_t total = 54u + image_bytes, offset = 54u, info_size = 40u;
    const uint16_t planes = 1u, bpp = 24u;
    header[0] = 'B';
    header[1] = 'M';
    std::memcpy(header + 2, &total, 4);
    std::memcpy(header + 10, &offset, 4);
    std::memcpy(header + 14, &info_size, 4);
    std::memcpy(header + 18, &width, 4);
    std::memcpy(header + 22, &height, 4);
    std::memcpy(header + 26, &planes, 2);
    std::memcpy(header + 28, &bpp, 2);
    std::memcpy(header + 34, &image_bytes, 4);
    std::fwrite(header, 1, sizeof header, file);
    std::vector<uint8_t> row(padded, 0u);
    for (uint32_t y = 0u; y < height; ++y) {
        const uint8_t *source = pixels + static_cast<size_t>(height - 1u - y) * width * 4u;
        for (uint32_t x = 0u; x < width; ++x) std::memcpy(&row[x * 3u], source + x * 4u, 3u);
        std::fwrite(row.data(), 1, padded, file);
    }
    std::fclose(file);
}

/* RECOMP_D3D_FRAME_DUMP names a BMP of the guest frame; with
   RECOMP_D3D_FRAME_DUMP_COUNT > 1, presents from RECOMP_D3D_FRAME_DUMP_AT
   (default 1) get numbered files. The pixels are the main target, row 0 on top. */
void dumpFrame(RecompD3dPresenter *presenter)
{
    const char *path = std::getenv("RECOMP_D3D_FRAME_DUMP");
    if (path == nullptr || path[0] == '\0') return;
    const char *count_text = std::getenv("RECOMP_D3D_FRAME_DUMP_COUNT");
    const unsigned count = count_text ? static_cast<unsigned>(std::strtoul(count_text, nullptr, 10)) : 1u;
    if (presenter->frame_dump_count >= (count == 0u ? 1u : count)) return;
    const char *at_text = std::getenv("RECOMP_D3D_FRAME_DUMP_AT");
    const unsigned at = at_text && at_text[0] ? static_cast<unsigned>(std::strtoul(at_text, nullptr, 10)) : 1u;
    if (presenter->present_count + 1u < at) return;
    char burst[1024];
    const unsigned index = presenter->frame_dump_count++;
    if (count > 1u) {
        std::snprintf(burst, sizeof burst, "%s.%03u.bmp", path, index);
        path = burst;
    }
    const uint32_t width = mainWidth(presenter), height = mainHeight(presenter);
    std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * 4u);
    bindFramebuffer(presenter, presenter->main_color, 0u);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, static_cast<GLsizei>(width), static_cast<GLsizei>(height), GL_BGRA, GL_UNSIGNED_BYTE,
        pixels.data());
    writeBmp(path, pixels.data(), width, height);
    std::fprintf(stderr, "recomp d3d presenter: frame dump path=%s size=%ux%u present=%u\n",
        path, width, height, presenter->present_count + 1u);
}

void pollHost(RecompD3dPresenter *presenter)
{
#ifdef __SWITCH__
    if (!appletMainLoop()) {
        presenter->close_requested = true;
        return;
    }
    padUpdate(&presenter->pad);
    const u64 held = padGetButtons(&presenter->pad);
    const u64 combo = HidNpadButton_ZL | HidNpadButton_ZR | HidNpadButton_Minus | HidNpadButton_Plus;
    if ((held & combo) == combo) presenter->close_requested = true;
#else
    (void)presenter;
#endif
}

RecompD3dPresenterError submitPresent(RecompD3dPresenter *presenter, const RecompD3dPresenterPresentCommand &present)
{
    if (present.effective_flags != 5u || present.swap_counter == 0u) {
        return RECOMP_D3D_PRESENTER_UNSUPPORTED_COMMAND;
    }
    pollHost(presenter);
    if (presenter->close_requested) return RECOMP_D3D_PRESENTER_CLOSED;
    flushSegment(presenter);
    dumpFrame(presenter);

    // The window has a lower-left origin, so this pass flips rows; gamma stays display state.
    glBindFramebuffer(GL_FRAMEBUFFER, 0u);
    presenter->bound_framebuffer = UINT32_MAX;
    clipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE);
    glViewport(0, 0, static_cast<GLsizei>(presenter->window_width), static_cast<GLsizei>(presenter->window_height));
    glDepthRangef(0.0f, 1.0f);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_POLYGON_OFFSET_FILL);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    presenter->applied_valid = false;
    const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    glClearBufferfv(GL_COLOR, 0, black);
    const float source_aspect = static_cast<float>(presentClientWidth(presenter, mainHeight(presenter))) /
        mainHeight(presenter);
    const float window_aspect = static_cast<float>(presenter->window_width) / presenter->window_height;
    float width = 1.0f, height = 1.0f;
    if (window_aspect > source_aspect) width = source_aspect / window_aspect;
    else height = window_aspect / source_aspect;
    glUseProgram(presenter->present_program);
    glUniform4f(presenter->present_rect, -width, -height, width, height);
    glUniform1i(presenter->present_gamma, presenter->gamma_enabled ? 1 : 0);
    glBindBufferBase(GL_UNIFORM_BUFFER, kGammaBinding, presenter->gamma_buffer);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, presenter->main_color);
    glBindSampler(0u, presenter->filter_sampler);
    glBindVertexArray(presenter->empty_vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0u);
    clipControl(GL_UPPER_LEFT, GL_ZERO_TO_ONE);

    // GL errors are not checked per call; report the first few per second.
    static uint32_t error_reports = 0u;
    if ((presenter->present_count % 60u) == 0u) error_reports = 0u;
    for (GLenum error = glGetError(); error != GL_NO_ERROR; error = glGetError()) {
        if (error_reports++ < 4u) {
            std::fprintf(stderr, "recomp d3d presenter: gl error 0x%04x by present %u\n", error,
                presenter->present_count);
        }
    }
    stage("eglSwapBuffers");
    eglSwapBuffers(presenter->display, presenter->surface);
    stage("present done");

    /* Keep the presented guest frame for GetBackBuffer(-1) once a draw has used it. */
    if (presenter->previous_frame != 0u) copyGuestBuffer(presenter, presenter->previous_frame);
    ++presenter->present_count;
    return RECOMP_D3D_PRESENTER_OK;
}

/* ---- Lifetime ------------------------------------------------------------------- */

bool initEgl(RecompD3dPresenter *presenter)
{
#ifdef __SWITCH__
    NWindow *window = nwindowGetDefault();
    nwindowSetDimensions(window, presenter->window_width, presenter->window_height);
    presenter->display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
#else
    const auto platform_display = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(
        eglGetProcAddress("eglGetPlatformDisplayEXT"));
    presenter->display = platform_display
        ? platform_display(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr)
        : eglGetDisplay(EGL_DEFAULT_DISPLAY);
#endif
    if (presenter->display == EGL_NO_DISPLAY || !eglInitialize(presenter->display, nullptr, nullptr)) {
        std::fprintf(stderr, "recomp d3d presenter: eglInitialize failed 0x%x\n", eglGetError());
        return false;
    }
    if (!eglBindAPI(EGL_OPENGL_API)) return false;
    const EGLint config_attributes[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
#ifndef __SWITCH__
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
#endif
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 0, EGL_STENCIL_SIZE, 0,
        EGL_NONE};
    EGLConfig config;
    EGLint count = 0;
    if (!eglChooseConfig(presenter->display, config_attributes, &config, 1, &count) || count == 0) {
        std::fprintf(stderr, "recomp d3d presenter: eglChooseConfig failed\n");
        return false;
    }
#ifdef __SWITCH__
    presenter->surface = eglCreateWindowSurface(presenter->display, config, window, nullptr);
#else
    const EGLint surface_attributes[] = {EGL_WIDTH, static_cast<EGLint>(presenter->window_width),
        EGL_HEIGHT, static_cast<EGLint>(presenter->window_height), EGL_NONE};
    presenter->surface = eglCreatePbufferSurface(presenter->display, config, surface_attributes);
#endif
    if (presenter->surface == EGL_NO_SURFACE) {
        std::fprintf(stderr, "recomp d3d presenter: eglCreateWindowSurface failed 0x%x\n", eglGetError());
        return false;
    }
    const EGLint context_attributes[] = {
        EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT_KHR,
        EGL_CONTEXT_MAJOR_VERSION_KHR, 4,
        EGL_CONTEXT_MINOR_VERSION_KHR, 3,
        EGL_NONE};
    presenter->context = eglCreateContext(presenter->display, config, EGL_NO_CONTEXT, context_attributes);
    if (presenter->context == EGL_NO_CONTEXT) {
        std::fprintf(stderr, "recomp d3d presenter: eglCreateContext failed 0x%x\n", eglGetError());
        return false;
    }
    if (!eglMakeCurrent(presenter->display, presenter->surface, presenter->surface, presenter->context)) {
        std::fprintf(stderr, "recomp d3d presenter: eglMakeCurrent failed 0x%x\n", eglGetError());
        return false;
    }
    if (!gladLoadGL()) {
        std::fprintf(stderr, "recomp d3d presenter: gladLoadGL failed\n");
        return false;
    }
    clipControl = reinterpret_cast<ClipControlFn>(eglGetProcAddress("glClipControl"));
    if (clipControl == nullptr) {
        std::fprintf(stderr, "recomp d3d presenter: glClipControl unavailable\n");
        return false;
    }
    GLint extensions = 0;
    glGetIntegerv(GL_NUM_EXTENSIONS, &extensions);
    for (GLint i = 0; i < extensions; ++i) {
        const char *name = reinterpret_cast<const char *>(glGetStringi(GL_EXTENSIONS, static_cast<GLuint>(i)));
        if (name != nullptr && std::strcmp(name, "GL_EXT_texture_compression_s3tc") == 0) presenter->s3tc = true;
    }
    if (envFlag("RECOMP_GL_NO_S3TC")) presenter->s3tc = false;
    std::fprintf(stderr, "recomp d3d presenter: %s / %s / %s s3tc=%d\n", glGetString(GL_VENDOR),
        glGetString(GL_RENDERER), glGetString(GL_VERSION), presenter->s3tc ? 1 : 0);
    return true;
}

void releasePresenter(RecompD3dPresenter *presenter)
{
    if (presenter->display == EGL_NO_DISPLAY) return;
    if (presenter->context != EGL_NO_CONTEXT) {
        flushSegment(presenter);
        glFinish();
        for (TextureEntry &entry : presenter->textures) {
            if (entry.texture != 0u) glDeleteTextures(1, &entry.texture);
        }
        releaseRetiredTextures(presenter);
        for (RenderTargetEntry &entry : presenter->render_targets) glDeleteTextures(1, &entry.texture);
        for (DepthTargetEntry &entry : presenter->depth_targets) glDeleteRenderbuffers(1, &entry.renderbuffer);
        for (auto &entry : presenter->framebuffers) glDeleteFramebuffers(1, &entry.second);
        for (auto &entry : presenter->programs) {
            if (entry.second.program != 0u) glDeleteProgram(entry.second.program);
        }
        for (auto &entry : presenter->vaos) glDeleteVertexArrays(1, &entry.second);
        for (SamplerEntry &entry : presenter->samplers) glDeleteSamplers(1, &entry.sampler);
        for (GLuint sampler : {presenter->draw_sampler, presenter->filter_sampler,
                 presenter->program_mask_sampler, presenter->point_sampler}) {
            if (sampler != 0u) glDeleteSamplers(1, &sampler);
        }
        for (GLuint texture : {presenter->back_buffer_copy, presenter->previous_frame, presenter->white_texture,
                 presenter->white_volume, presenter->main_color}) {
            if (texture != 0u) glDeleteTextures(1, &texture);
        }
        if (presenter->main_depth != 0u) glDeleteRenderbuffers(1, &presenter->main_depth);
        for (GLuint buffer : {presenter->vertex_buffer, presenter->index_buffer, presenter->uniform_buffer,
                 presenter->gamma_buffer}) {
            if (buffer != 0u) glDeleteBuffers(1, &buffer);
        }
        for (const PendingQuery &query : presenter->visibility_pending) {
            if (query.query != 0u) glDeleteQueries(1, &query.query);
        }
        if (!presenter->free_queries.empty()) {
            glDeleteQueries(static_cast<GLsizei>(presenter->free_queries.size()), presenter->free_queries.data());
        }
        if (presenter->present_program != 0u) glDeleteProgram(presenter->present_program);
        if (presenter->empty_vao != 0u) glDeleteVertexArrays(1, &presenter->empty_vao);
    }
    eglMakeCurrent(presenter->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (presenter->context != EGL_NO_CONTEXT) eglDestroyContext(presenter->display, presenter->context);
    if (presenter->surface != EGL_NO_SURFACE) eglDestroySurface(presenter->display, presenter->surface);
    eglTerminate(presenter->display);
    presenter->display = EGL_NO_DISPLAY;
    presenter->context = EGL_NO_CONTEXT;
    presenter->surface = EGL_NO_SURFACE;
}

bool createResources(RecompD3dPresenter *presenter)
{
    const uint32_t width = mainWidth(presenter), height = mainHeight(presenter);
    glGetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, &presenter->uniform_align);
    if (presenter->uniform_align < 16) presenter->uniform_align = 16;
    presenter->main_color = createTexture2D(GL_RGBA8, width, height, 1u);
    presenter->main_depth = createDepthRenderbuffer(width, height);
    presenter->white_texture = createTexture2D(GL_RGBA8, 1u, 1u, 1u);
    const uint32_t white = 0xffffffffu;
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, &white);
    presenter->white_volume = createTexture3D(1u, 1u, 1u);
    glTexSubImage3D(GL_TEXTURE_3D, 0, 0, 0, 0, 1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, &white);
    presenter->copy_mips = presenter->scale != 1.0f;

    bindFramebuffer(presenter, presenter->main_color, presenter->main_depth);
    const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    glClearBufferfv(GL_COLOR, 0, black);
    glClearBufferfi(GL_DEPTH_STENCIL, 0, 1.0f, 0);

    presenter->draw_sampler = createSampler(GL_REPEAT, GL_REPEAT, GL_REPEAT, false, 0.0f);
    presenter->filter_sampler = createSampler(GL_CLAMP_TO_EDGE, GL_CLAMP_TO_EDGE, GL_CLAMP_TO_EDGE, true, 0.0f);
    presenter->program_mask_sampler = createSampler(GL_REPEAT, GL_REPEAT, GL_REPEAT, true, 0.0f);
    presenter->point_sampler = createSampler(GL_CLAMP_TO_EDGE, GL_CLAMP_TO_EDGE, GL_CLAMP_TO_EDGE, true, 0.0f, true);

    // Allocated through GL_COPY_WRITE_BUFFER: core profile has no default VAO for an element binding.
    const auto allocate = [](GLuint &buffer, GLsizeiptr bytes, GLenum usage) {
        glGenBuffers(1, &buffer);
        glBindBuffer(GL_COPY_WRITE_BUFFER, buffer);
        glBufferData(GL_COPY_WRITE_BUFFER, bytes, nullptr, usage);
    };
    allocate(presenter->vertex_buffer, kVertexRingBytes, GL_STREAM_DRAW);
    allocate(presenter->index_buffer, kIndexRingBytes, GL_STREAM_DRAW);
    allocate(presenter->uniform_buffer, kUniformRingBytes, GL_STREAM_DRAW);
    allocate(presenter->gamma_buffer, sizeof presenter->gamma_ramp, GL_DYNAMIC_DRAW);

    const GLuint vs = compileStage(GL_VERTEX_SHADER, kPresentVertex, sizeof kPresentVertex - 1u, "present");
    const GLuint fs = compileStage(GL_FRAGMENT_SHADER, kPresentFragment, sizeof kPresentFragment - 1u, "present");
    presenter->present_program = linkProgram(vs, fs, "present");
    if (vs != 0u) glDeleteShader(vs);
    if (fs != 0u) glDeleteShader(fs);
    if (presenter->present_program == 0u) return false;
    presenter->present_rect = glGetUniformLocation(presenter->present_program, "u_rect");
    presenter->present_gamma = glGetUniformLocation(presenter->present_program, "u_gamma");
    glGenVertexArrays(1, &presenter->empty_vao);
    const GLenum status = glGetError();
    if (status != GL_NO_ERROR) {
        std::fprintf(stderr, "recomp d3d presenter: gl error 0x%04x creating resources\n", status);
        return false;
    }
    return true;
}

RecompD3dPresenterError checkOwner(RecompD3dPresenter *presenter)
{
    if (presenter == nullptr || presenter != active_presenter) return RECOMP_D3D_PRESENTER_NOT_INITIALIZED;
    return std::this_thread::get_id() == presenter->owner_thread
        ? RECOMP_D3D_PRESENTER_OK : RECOMP_D3D_PRESENTER_WRONG_THREAD;
}

} // namespace

RecompD3dPresenterError d3d_backend_create(const RecompD3dPresenterConfig *config, RecompD3dPresenter **presenter)
{
    if (config == nullptr || presenter == nullptr || config->width == 0u || config->height == 0u ||
        config->color_format != RECOMP_D3D_PRESENTER_COLOR_FORMAT_BGRA8_UNORM ||
        config->depth_format != RECOMP_D3D_PRESENTER_DEPTH_FORMAT_D24S8) {
        return RECOMP_D3D_PRESENTER_INVALID_ARGUMENT;
    }
    if (*presenter != nullptr || active_presenter != nullptr) return RECOMP_D3D_PRESENTER_ALREADY_INITIALIZED;
    if (!recomp_shader_compiler_available()) return RECOMP_D3D_PRESENTER_HOST_FAILURE;
    auto *created = new (std::nothrow) RecompD3dPresenter;
    if (created == nullptr) return RECOMP_D3D_PRESENTER_OUT_OF_MEMORY;
    created->config = *config;
    created->owner_thread = std::this_thread::get_id();
    created->widescreen = envFlag("RECOMP_D3D_WIDESCREEN");
    created->flip_winding = envFlag("RECOMP_GL_FLIP_WINDING");
    if (const char *scale = std::getenv("RECOMP_D3D_SCALE")) {
        const float value = static_cast<float>(std::atof(scale));
        if (std::isfinite(value)) created->scale = std::clamp(value, 1.0f, 3.0f);
    }
#ifdef __SWITCH__
    if (appletGetOperationMode() == AppletOperationMode_Console) {
        created->window_width = 1920u;
        created->window_height = 1080u;
    }
    padInitializeDefault(&created->pad);
    // The text console draws into the window EGL needs.
    doa2u_nx_release_console();
#else
    created->window_width = mainWidth(created);
    created->window_height = mainHeight(created);
#endif
    if (!initEgl(created) || (clipControl(GL_UPPER_LEFT, GL_ZERO_TO_ONE), !createResources(created))) {
        releasePresenter(created);
        delete created;
        return RECOMP_D3D_PRESENTER_HOST_FAILURE;
    }
    eglSwapInterval(created->display, immediate_present ? 0 : 1);
    std::fprintf(stderr, "recomp d3d presenter: opengl target=%ux%u scale=%.2f window=%ux%u widescreen=%d\n",
        mainWidth(created), mainHeight(created), created->scale, created->window_width, created->window_height,
        created->widescreen ? 1 : 0);
    try {
        created->precompile = std::thread(precompileDrawShaders, created);
    } catch (const std::system_error &) {
        // Shaders then compile on first use.
    }
    active_presenter = created;
    *presenter = created;
    return RECOMP_D3D_PRESENTER_OK;
}

RecompD3dPresenterError d3d_backend_submit(RecompD3dPresenter *presenter, const RecompD3dPresenterCommand *command)
{
    const RecompD3dPresenterError owner = checkOwner(presenter);
    if (owner != RECOMP_D3D_PRESENTER_OK) return owner;
    if (presenter->close_requested) return RECOMP_D3D_PRESENTER_CLOSED;
    if (command == nullptr) return RECOMP_D3D_PRESENTER_INVALID_ARGUMENT;
    static const char *const names[] = {"clear", "present", "draw", "gamma", "visibility begin",
        "visibility end", "coverage", "copy main color"};
    stage(static_cast<unsigned>(command->type) < 8u ? names[command->type] : "command");
    switch (command->type) {
    case RECOMP_D3D_PRESENTER_COMMAND_GAMMA:
        return submitGamma(presenter, command->data.gamma);
    case RECOMP_D3D_PRESENTER_COMMAND_CLEAR:
        return submitClear(presenter, command->data.clear);
    case RECOMP_D3D_PRESENTER_COMMAND_PRESENT:
        return submitPresent(presenter, command->data.present);
    case RECOMP_D3D_PRESENTER_COMMAND_DRAW:
        return submitDraw(presenter, command->data.draw);
    case RECOMP_D3D_PRESENTER_COMMAND_VISIBILITY_BEGIN:
        return beginVisibility(presenter);
    case RECOMP_D3D_PRESENTER_COMMAND_VISIBILITY_END: {
        BoundTarget last = presenter->last_bound;
        if (last.host_width == 0u) {
            last.scale_x = static_cast<float>(mainWidth(presenter)) / presenter->config.width;
            last.scale_y = static_cast<float>(mainHeight(presenter)) / presenter->config.height;
        }
        return endVisibility(presenter, command->data.visibility_index, last);
    }
    case RECOMP_D3D_PRESENTER_COMMAND_COPY_MAIN_COLOR:
        return submitCopyMainColor(presenter, command->data.copy_destination);
    default:
        return RECOMP_D3D_PRESENTER_UNSUPPORTED_COMMAND;
    }
}

RecompD3dPresenterError d3d_backend_draw_coverage(
    RecompD3dPresenter *presenter, const RecompD3dPresenterDrawCommand *draw, bool *covered)
{
    if (covered) *covered = true;
    const RecompD3dPresenterError owner = checkOwner(presenter);
    if (owner != RECOMP_D3D_PRESENTER_OK) return owner;
    if (draw == nullptr || covered == nullptr) return RECOMP_D3D_PRESENTER_INVALID_ARGUMENT;
    RecompD3dVertexLayout layout;
    if (draw->program_count != 0u || !recomp_d3d_fvf_layout(draw->fvf, &layout) ||
        layout.pretransformed || draw->primitive_type < 5u || draw->primitive_type > 7u ||
        !draw->has_transform || draw->blend_weight_count > 3u ||
        (draw->blend_weight_count != 0u && draw->blend_weight_count != layout.blend_weight_count))
        return RECOMP_D3D_PRESENTER_UNSUPPORTED_COMMAND;
    if (draw->vertex_bytes == nullptr || draw->index_bytes == nullptr ||
        draw->vertex_count == 0u || draw->vertex_count > 65536u || draw->index_count < 3u ||
        static_cast<uint64_t>(draw->index_count) * sizeof(uint16_t) > 64u * 1024u * 1024u ||
        draw->vertex_stride < layout.stride ||
        static_cast<uint64_t>(draw->vertex_stride) * draw->vertex_count > 64u * 1024u * 1024u ||
        draw->triangle_count != recomp_d3d_draw_triangle_count(draw->primitive_type, draw->index_count))
        return RECOMP_D3D_PRESENTER_INVALID_ARGUMENT;
    const auto *indices = static_cast<const uint16_t *>(draw->index_bytes);
    for (uint32_t i = 0u; i < draw->index_count; ++i) {
        if (indices[i] >= draw->vertex_count) return RECOMP_D3D_PRESENTER_INVALID_ARGUMENT;
        const auto *vertex = static_cast<const uint8_t *>(draw->vertex_bytes) +
            static_cast<size_t>(indices[i]) * draw->vertex_stride;
        for (uint32_t j = 0u; j < 3u + draw->blend_weight_count; ++j) {
            float value;
            std::memcpy(&value, vertex + j * sizeof value, sizeof value);
            if (!std::isfinite(value)) return RECOMP_D3D_PRESENTER_INVALID_ARGUMENT;
        }
    }
    for (uint32_t i = 0u; i <= draw->blend_weight_count; ++i) {
        const float *matrix = i == 0u ? draw->transform : draw->blend_transforms[i - 1u];
        for (uint32_t j = 0u; j < 16u; ++j)
            if (!std::isfinite(matrix[j])) return RECOMP_D3D_PRESENTER_INVALID_ARGUMENT;
    }
    for (float value : draw->viewport)
        if (!std::isfinite(value)) return RECOMP_D3D_PRESENTER_INVALID_ARGUMENT;
    if (draw->viewport[2] < 0.0f || (draw->viewport[2] > 0.0f &&
        (draw->viewport[3] <= 0.0f || draw->viewport[4] < 0.0f || draw->viewport[5] > 1.0f ||
         draw->viewport[4] > draw->viewport[5])))
        return RECOMP_D3D_PRESENTER_INVALID_ARGUMENT;
    if (presenter->visibility_open) return RECOMP_D3D_PRESENTER_UNSUPPORTED_COMMAND;
    if (!std::isfinite(draw->screen_space_offset[0]) || !std::isfinite(draw->screen_space_offset[1]) ||
        static_cast<unsigned>(draw->cull_mode) > RECOMP_D3D_CULL_COUNTER_CLOCKWISE)
        return RECOMP_D3D_PRESENTER_UNSUPPORTED_COMMAND;

    BoundTarget bound;
    const RecompD3dPresenterError target_error = bindTarget(presenter, draw->target, bound);
    if (target_error != RECOMP_D3D_PRESENTER_OK) return target_error;
    float rect[4] = {0.0f, 0.0f, static_cast<float>(bound.host_width), static_cast<float>(bound.host_height)};
    float clip_rect[4] = {rect[0], rect[1], rect[2], rect[3]};
    if (draw->has_scissor) {
        if (draw->scissor[0] < 0 || draw->scissor[1] < 0 ||
            draw->scissor[2] < draw->scissor[0] || draw->scissor[3] < draw->scissor[1])
            return RECOMP_D3D_PRESENTER_UNSUPPORTED_COMMAND;
        for (unsigned i = 0u; i < 4u; ++i) {
            const double edge = draw->scissor[i] * static_cast<double>(i % 2u == 0u ? bound.scale_x : bound.scale_y);
            if (!std::isfinite(edge) || edge > 32767.0) return RECOMP_D3D_PRESENTER_UNSUPPORTED_COMMAND;
            const float rounded = static_cast<float>(std::floor(edge + 0.5));
            clip_rect[i] = i < 2u ? std::max(clip_rect[i], rounded) : std::min(clip_rect[i], rounded);
        }
    }
    if (draw->viewport[2] > 0.0f) {
        const float guest[4] = {draw->viewport[0] * bound.scale_x, draw->viewport[1] * bound.scale_y,
            draw->viewport[2] * bound.scale_x, draw->viewport[3] * bound.scale_y};
        if (guest[0] < -32768.0f || guest[1] < -32768.0f || guest[0] + guest[2] > 32767.0f ||
            guest[1] + guest[3] > 32767.0f) return RECOMP_D3D_PRESENTER_UNSUPPORTED_COMMAND;
        rect[0] = guest[0];
        rect[1] = guest[1];
        rect[2] = guest[0] + guest[2];
        rect[3] = guest[1] + guest[3];
        clip_rect[0] = std::max(clip_rect[0], rect[0]);
        clip_rect[1] = std::max(clip_rect[1], rect[1]);
        clip_rect[2] = std::min(clip_rect[2], rect[2]);
        clip_rect[3] = std::min(clip_rect[3], rect[3]);
    }
    RecompD3dPresenterDrawCommand geometry{};
    geometry.fvf = draw->fvf;
    geometry.has_transform = true;
    geometry.blend_weight_count = draw->blend_weight_count;
    std::memcpy(geometry.transform, draw->transform, sizeof geometry.transform);
    std::memcpy(geometry.blend_transforms, draw->blend_transforms, sizeof geometry.blend_transforms);
    std::memcpy(geometry.viewport, draw->viewport, sizeof geometry.viewport);
    std::memcpy(geometry.screen_space_offset, draw->screen_space_offset, sizeof geometry.screen_space_offset);
    RecompD3dDrawTargetState target_state{};
    target_state.viewport[2] = static_cast<float>(bound.host_width);
    target_state.viewport[3] = static_cast<float>(bound.host_height);
    target_state.viewport[5] = 1.0f;
    target_state.scale_x = bound.scale_x;
    target_state.scale_y = bound.scale_y;
    float constants[kRecompD3dDrawConstantFloats];
    if (!recomp_d3d_draw_constants(geometry, layout, target_state, false, false, constants))
        return RECOMP_D3D_PRESENTER_UNSUPPORTED_COMMAND;
    const auto transform = [&](uint16_t index, float (&clip)[4]) {
        const auto *vertex = static_cast<const uint8_t *>(draw->vertex_bytes) +
            static_cast<size_t>(index) * draw->vertex_stride;
        float position[3], weights[3] = {};
        std::memcpy(position, vertex, sizeof position);
        std::memcpy(weights, vertex + 12u, draw->blend_weight_count * sizeof(float));
        const auto apply = [&](const float *matrix, float weight) {
            for (unsigned column = 0u; column < 4u; ++column) {
                clip[column] += weight * (position[0] * matrix[column] + position[1] * matrix[4u + column] +
                    position[2] * matrix[8u + column] + matrix[12u + column]);
            }
        };
        std::fill(clip, clip + 4, 0.0f);
        if (draw->blend_weight_count == 0u) {
            apply(constants, 1.0f);
            return;
        }
        float remainder = 1.0f;
        for (uint32_t i = 0u; i < draw->blend_weight_count; ++i) {
            apply(constants + 16u * i, weights[i]);
            remainder -= weights[i];
        }
        apply(constants + 16u * draw->blend_weight_count, remainder);
    };
    if (clip_rect[2] <= clip_rect[0] || clip_rect[3] <= clip_rect[1]) {
        *covered = false;
        return RECOMP_D3D_PRESENTER_OK;
    }
    bool hit = false;
    for (uint32_t t = 0u; t < draw->triangle_count && !hit; ++t) {
        uint16_t corner[3];
        if (draw->primitive_type == RECOMP_D3D_PT_TRIANGLELIST) {
            corner[0] = indices[t * 3u]; corner[1] = indices[t * 3u + 1u]; corner[2] = indices[t * 3u + 2u];
        } else if (draw->primitive_type == RECOMP_D3D_PT_TRIANGLESTRIP) {
            corner[0] = indices[t]; corner[1] = indices[t + 1u + (t & 1u)]; corner[2] = indices[t + 2u - (t & 1u)];
        } else {
            corner[0] = indices[0]; corner[1] = indices[t + 1u]; corner[2] = indices[t + 2u];
        }
        float clip[3][4];
        for (unsigned i = 0u; i < 3u; ++i) transform(corner[i], clip[i]);
        hit = triangleCovers(clip, draw->cull_mode, rect, clip_rect);
    }
    *covered = hit;
    return RECOMP_D3D_PRESENTER_OK;
}

RecompD3dPresenterError d3d_backend_release_memory(
    RecompD3dPresenter *presenter, uint32_t base, uint32_t size, bool targets)
{
    const RecompD3dPresenterError owner = checkOwner(presenter);
    if (owner != RECOMP_D3D_PRESENTER_OK) return owner;
    stage(targets ? "release memory" : "invalidate textures");
    if (base >= 0x80000000u && base < 0x84000000u) base -= 0x80000000u;
    const uint64_t end = static_cast<uint64_t>(base) + size;
    if (size == 0u || end > 0x100000000ull) return RECOMP_D3D_PRESENTER_INVALID_ARGUMENT;
    flushSegment(presenter);
    const auto released = [base, end](uint32_t data) { return data >= base && data < end; };
    const auto colors = presenter->render_targets.size();
    const auto depths = presenter->depth_targets.size();
    for (size_t i = 0u; targets && i < presenter->render_targets.size();) {
        RenderTargetEntry &entry = presenter->render_targets[i];
        if (!released(entry.desc.data)) { ++i; continue; }
        retireTexture(presenter, entry.texture);
        presenter->target_bytes -= entry.bytes;
        entry = presenter->render_targets.back();
        presenter->render_targets.pop_back();
    }
    for (size_t i = 0u; targets && i < presenter->depth_targets.size();) {
        DepthTargetEntry &entry = presenter->depth_targets[i];
        if (!released(entry.desc.data)) { ++i; continue; }
        forgetFramebuffers(presenter, 0u, entry.renderbuffer);
        glDeleteRenderbuffers(1, &entry.renderbuffer);
        presenter->target_bytes -= entry.bytes;
        entry = presenter->depth_targets.back();
        presenter->depth_targets.pop_back();
    }
    for (uint32_t i = 0u; i < presenter->texture_count; ++i) {
        TextureEntry &entry = presenter->textures[i];
        if (entry.used && released(entry.data)) {
            unindexTexture(presenter, i);
            retireTexture(presenter, entry.texture);
            entry.used = false;
        }
    }
    presenter->segment.color = presenter->segment.depth = 0u;
    releaseRetiredTextures(presenter);
    if (colors != presenter->render_targets.size() || depths != presenter->depth_targets.size()) {
        std::fprintf(stderr,
            "recomp d3d presenter: released storage base=0x%08X size=%u color=%zu depth=%zu remaining=%zu/%zu\n",
            base, size, colors - presenter->render_targets.size(), depths - presenter->depth_targets.size(),
            presenter->render_targets.size(), presenter->depth_targets.size());
    }
    return RECOMP_D3D_PRESENTER_OK;
}

RecompD3dPresenterError d3d_backend_destroy(RecompD3dPresenter **presenter)
{
    if (presenter == nullptr) return RECOMP_D3D_PRESENTER_INVALID_ARGUMENT;
    const RecompD3dPresenterError owner = checkOwner(*presenter);
    if (owner != RECOMP_D3D_PRESENTER_OK) return owner;
    RecompD3dPresenter *destroyed = *presenter;
    active_presenter = nullptr;
    *presenter = nullptr;
    destroyed->precompile_stop = true;
    if (destroyed->precompile.joinable()) destroyed->precompile.join();
    releasePresenter(destroyed);
    delete destroyed;
    return RECOMP_D3D_PRESENTER_OK;
}

void d3d_backend_set_immediate_present(bool enabled)
{
    immediate_present = enabled;
    RecompD3dPresenter *presenter = active_presenter;
    if (presenter != nullptr && presenter->display != EGL_NO_DISPLAY &&
        std::this_thread::get_id() == presenter->owner_thread) {
        eglSwapInterval(presenter->display, enabled ? 0 : 1);
    }
}

D3dBackendVisibilityPoll d3d_backend_poll_visibility(RecompD3dPresenter *presenter, uint32_t *index, uint32_t *pixels)
{
    if (checkOwner(presenter) != RECOMP_D3D_PRESENTER_OK) return D3D_BACKEND_VISIBILITY_FAILED;
    if (presenter->visibility_pending.empty()) return D3D_BACKEND_VISIBILITY_IDLE;
    PendingQuery &front = presenter->visibility_pending.front();
    GLuint samples = 0u;
    if (front.query != 0u) {
        GLuint available = GL_FALSE;
        stage("visibility poll");
        glGetQueryObjectuiv(front.query, GL_QUERY_RESULT_AVAILABLE, &available);
        if (!available) {
            if (std::chrono::steady_clock::now() - front.ended < queryTimeout()) {
                stage("idle");
                return D3D_BACKEND_VISIBILITY_PENDING;
            }
            static unsigned forced;
            if (forced++ < 4u) {
                std::fprintf(stderr, "recomp d3d presenter: visibility index=%u not available after %lld ms;"
                    " reading it blocking\n", front.index, static_cast<long long>(queryTimeout().count()));
            }
            stage("visibility read (blocking)");
        }
        glGetQueryObjectuiv(front.query, GL_QUERY_RESULT, &samples);
        stage("idle");
        presenter->free_queries.push_back(front.query);
    }
    *index = front.index;
    // Samples are host pixels when single-sampled; report guest pixels.
    *pixels = static_cast<uint32_t>(std::min(
        std::floor(samples / static_cast<double>(front.host_pixels_per_guest_pixel) + 0.5), 4294967295.0));
    presenter->visibility_pending.erase(presenter->visibility_pending.begin());
    return D3D_BACKEND_VISIBILITY_READY;
}

void d3d_backend_report_draw_textures()
{
    for (uint32_t format = 0u; format < 256u; ++format) {
        const uint32_t sampled = draw_texture_tally.textured[format];
        const uint32_t rejected = draw_texture_tally.rejected[format];
        if (sampled == 0u && rejected == 0u) continue;
        std::fprintf(stderr, "recomp d3d presenter: draw texture fmt=0x%02x sampled=%u rejected=%u\n",
            format, sampled, rejected);
    }
    std::fprintf(stderr, "recomp d3d presenter: draw texture untextured=%u cached=%u\n",
        draw_texture_tally.untextured, active_presenter != nullptr ? active_presenter->texture_count : 0u);
}
