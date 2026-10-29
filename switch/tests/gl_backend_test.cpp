// Host functional test of switch/src/d3d_backend_gl.cpp on Mesa llvmpipe,
// driven through the presenter API (d3d_presenter_thread_nx.cpp), as the
// D3D8 adapter drives it on the console. Frames are read back through the
// backend's RECOMP_D3D_FRAME_DUMP BMPs.
#include "d3d_presenter.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

int failures = 0;
const char *dump_path = "/tmp/gltest_frame";

struct Image {
    uint32_t width = 0, height = 0;
    std::vector<uint8_t> bgr;  // top row first
    bool at(uint32_t x, uint32_t y, uint8_t &r, uint8_t &g, uint8_t &b) const
    {
        if (x >= width || y >= height) return false;
        const uint8_t *p = &bgr[(static_cast<size_t>(y) * width + x) * 3u];
        b = p[0]; g = p[1]; r = p[2];
        return true;
    }
};

bool readBmp(const char *path, Image &image)
{
    FILE *file = std::fopen(path, "rb");
    if (!file) return false;
    uint8_t header[54];
    if (std::fread(header, 1, 54, file) != 54) { std::fclose(file); return false; }
    int32_t w, h;
    std::memcpy(&w, header + 18, 4);
    std::memcpy(&h, header + 22, 4);
    image.width = static_cast<uint32_t>(w);
    image.height = static_cast<uint32_t>(h);
    const uint32_t padded = (image.width * 3u + 3u) & ~3u;
    image.bgr.assign(static_cast<size_t>(image.width) * image.height * 3u, 0u);
    std::vector<uint8_t> row(padded);
    for (uint32_t y = 0; y < image.height; ++y) {
        if (std::fread(row.data(), 1, padded, file) != padded) break;
        std::memcpy(&image.bgr[(static_cast<size_t>(image.height - 1u - y)) * image.width * 3u], row.data(),
            image.width * 3u);
    }
    std::fclose(file);
    return true;
}

void expect(bool condition, const char *what)
{
    std::printf("  %s %s\n", condition ? "ok  " : "FAIL", what);
    if (!condition) ++failures;
}

bool near(uint8_t a, uint8_t b, int tolerance = 3) { return std::abs(int(a) - int(b)) <= tolerance; }

float scale = 1.0f;  // RECOMP_D3D_SCALE: probes name guest pixels

bool pixelIs(const Image &image, uint32_t gx, uint32_t gy, uint8_t r, uint8_t g, uint8_t b, int tolerance = 3)
{
    const uint32_t x = static_cast<uint32_t>(gx * scale + scale * 0.5f);
    const uint32_t y = static_cast<uint32_t>(gy * scale + scale * 0.5f);
    uint8_t pr, pg, pb;
    if (!image.at(x, y, pr, pg, pb)) return false;
    const bool ok = near(pr, r, tolerance) && near(pg, g, tolerance) && near(pb, b, tolerance);
    if (!ok) std::printf("       pixel (%u,%u) = %u,%u,%u want %u,%u,%u\n", x, y, pr, pg, pb, r, g, b);
    return ok;
}

RecompD3dPresenter *presenter = nullptr;
unsigned frame = 0;

RecompD3dPresenterError submit(const RecompD3dPresenterCommand &command)
{
    return recomp_d3d_presenter_submit(presenter, &command);
}

void clear(uint32_t argb, RecompD3dPresenterTarget target = {}, bool depth = true)
{
    RecompD3dPresenterCommand command{};
    command.type = RECOMP_D3D_PRESENTER_COMMAND_CLEAR;
    command.data.clear.clear_color = true;
    command.data.clear.clear_depth = depth;
    command.data.clear.clear_stencil = depth;
    command.data.clear.color = argb;
    command.data.clear.z = 1.0f;
    command.data.clear.target = target;
    const auto error = submit(command);
    if (error != RECOMP_D3D_PRESENTER_OK) std::printf("  clear error %d\n", error);
}

/* Presents and returns the frame the backend dumped. */
Image present()
{
    char path[256];
    std::snprintf(path, sizeof path, "%s.%03u.bmp", dump_path, frame++);
    RecompD3dPresenterCommand command{};
    command.type = RECOMP_D3D_PRESENTER_COMMAND_PRESENT;
    command.data.present.effective_flags = 5u;
    command.data.present.swap_counter = 1u;
    const auto error = submit(command);
    if (error != RECOMP_D3D_PRESENTER_OK) std::printf("  present error %d\n", error);
    recomp_d3d_presenter_flush(presenter, true);
    Image image;
    if (!readBmp(path, image)) std::printf("  no frame dump %s\n", path);
    return image;
}

struct RhwDiffuse { float x, y, z, rhw; uint32_t diffuse; };
struct RhwTex { float x, y, z, rhw, u, v; };
struct XyzDiffuse { float x, y, z; uint32_t diffuse; };

RecompD3dPresenterDrawCommand baseDraw()
{
    RecompD3dPresenterDrawCommand draw{};
    draw.blend.color_write_mask = 15;
    draw.depth.depth_func = RECOMP_D3D_COMPARE_LESS_EQUAL;
    draw.depth.stencil_read_mask = 0xff;
    draw.depth.stencil_write_mask = 0xff;
    return draw;
}

RecompD3dPresenterError drawCommand(const RecompD3dPresenterDrawCommand &draw)
{
    RecompD3dPresenterCommand command{};
    command.type = RECOMP_D3D_PRESENTER_COMMAND_DRAW;
    command.data.draw = draw;
    return submit(command);
}

/* Xbox swizzle for a power-of-two texture: Morton order of (x, y). */
uint32_t swizzle(uint32_t x, uint32_t y, uint32_t width, uint32_t height)
{
    uint32_t offset = 0u, bit = 0u;
    for (uint32_t mask = 1u; mask < width || mask < height; mask <<= 1u) {
        if (mask < width) { if (x & mask) offset |= 1u << bit; ++bit; }
        if (mask < height) { if (y & mask) offset |= 1u << bit; ++bit; }
    }
    return offset;
}

} // namespace

int main()
{
    if (const char *value = getenv("RECOMP_D3D_SCALE")) scale = static_cast<float>(atof(value));
    setenv("RECOMP_D3D_FRAME_DUMP", dump_path, 1);
    setenv("RECOMP_D3D_FRAME_DUMP_COUNT", "1000", 1);
    setenv("RECOMP_SHADER_CACHE", "/tmp/gltest_shaders", 1);
    std::system("rm -f /tmp/gltest_frame.*.bmp; mkdir -p /tmp/gltest_shaders");
    const RecompD3dPresenterConfig config{640u, 480u, RECOMP_D3D_PRESENTER_COLOR_FORMAT_BGRA8_UNORM,
        RECOMP_D3D_PRESENTER_DEPTH_FORMAT_D24S8};
    if (recomp_d3d_presenter_create(&config, &presenter) != RECOMP_D3D_PRESENTER_OK) {
        std::printf("presenter create failed\n");
        return 1;
    }

    std::printf("clear\n");
    clear(0xff102030u);
    Image image = present();
    expect(image.width == static_cast<uint32_t>(640 * scale + 0.5f) &&
        image.height == static_cast<uint32_t>(480 * scale + 0.5f), "frame is the scaled 640x480");
    expect(pixelIs(image, 10, 10, 0x10, 0x20, 0x30), "clear color");

    std::printf("pretransformed triangle (orientation)\n");
    {
        clear(0xff000000u);
        // Screen-space triangle in the top-left corner, D3D y down.
        const RhwDiffuse vertices[3] = {{0, 0, 0.5f, 1, 0xffff0000u}, {200, 0, 0.5f, 1, 0xffff0000u},
            {0, 200, 0.5f, 1, 0xffff0000u}};
        const uint16_t indices[3] = {0, 1, 2};
        RecompD3dPresenterDrawCommand draw = baseDraw();
        draw.fvf = 0x44u;
        draw.primitive_type = 5u;
        draw.vertex_stride = sizeof(RhwDiffuse);
        draw.vertex_count = 3u;
        draw.index_count = 3u;
        draw.triangle_count = 1u;
        draw.vertex_bytes = vertices;
        draw.index_bytes = indices;
        expect(drawCommand(draw) == RECOMP_D3D_PRESENTER_OK, "draw accepted");
        image = present();
        expect(pixelIs(image, 20, 20, 0xff, 0, 0), "red in the top-left");
        expect(pixelIs(image, 600, 440, 0, 0, 0), "black in the bottom-right");
        expect(pixelIs(image, 20, 440, 0, 0, 0), "black in the bottom-left (not flipped)");
    }

    std::printf("culling\n");
    {
        // (0,0) -> (200,0) -> (0,200) runs clockwise on screen (y down).
        const RhwDiffuse vertices[3] = {{0, 0, 0.5f, 1, 0xff00ff00u}, {200, 0, 0.5f, 1, 0xff00ff00u},
            {0, 200, 0.5f, 1, 0xff00ff00u}};
        const uint16_t indices[3] = {0, 1, 2};
        for (int mode = 0; mode < 3; ++mode) {
            clear(0xff000000u);
            RecompD3dPresenterDrawCommand draw = baseDraw();
            draw.fvf = 0x44u;
            draw.primitive_type = 5u;
            draw.vertex_stride = sizeof(RhwDiffuse);
            draw.vertex_count = 3u;
            draw.index_count = 3u;
            draw.triangle_count = 1u;
            draw.vertex_bytes = vertices;
            draw.index_bytes = indices;
            draw.cull_mode = static_cast<RecompD3dCullMode>(mode);
            drawCommand(draw);
            image = present();
            const bool drawn = pixelIs(image, 20, 20, 0, 0xff, 0);
            if (mode == RECOMP_D3D_CULL_CLOCKWISE) expect(!drawn, "CULL_CW removes a screen-clockwise triangle");
            else expect(drawn, mode == 0 ? "CULL_NONE draws it" : "CULL_CCW keeps a screen-clockwise triangle");
        }
    }

    std::printf("swizzled A8R8G8B8 texture\n");
    {
        clear(0xff000000u);
        // 4x4 texture: quadrants red (top-left), green (top-right), blue (bottom-left), white.
        std::vector<uint32_t> texels(16);
        for (uint32_t y = 0; y < 4; ++y) {
            for (uint32_t x = 0; x < 4; ++x) {
                const uint32_t color = y < 2 ? (x < 2 ? 0xffff0000u : 0xff00ff00u) : (x < 2 ? 0xff0000ffu : 0xffffffffu);
                texels[swizzle(x, y, 4, 4)] = color;
            }
        }
        const RhwTex vertices[4] = {{0, 0, 0.5f, 1, 0, 0}, {400, 0, 0.5f, 1, 1, 0}, {0, 400, 0.5f, 1, 0, 1},
            {400, 400, 0.5f, 1, 1, 1}};
        const uint16_t indices[4] = {0, 1, 2, 3};
        RecompD3dPresenterDrawCommand draw = baseDraw();
        draw.fvf = 0x104u;
        draw.primitive_type = 6u;
        draw.vertex_stride = sizeof(RhwTex);
        draw.vertex_count = 4u;
        draw.index_count = 4u;
        draw.triangle_count = 2u;
        draw.vertex_bytes = vertices;
        draw.index_bytes = indices;
        draw.has_texture = true;
        draw.texture.format_byte = RECOMP_D3D_TEXTURE_FORMAT_A8R8G8B8;
        draw.texture.bits_per_pixel = 32u;
        draw.texture.width = 4u;
        draw.texture.height = 4u;
        draw.texture.pitch = 16u;
        draw.texture.data = 0x100000u;
        draw.texture.mip_levels = 1u;
        draw.texture_bytes = texels.data();
        draw.texture_byte_count = 64u;
        draw.point_mip_filter = true;
        draw.address_u = draw.address_v = 3u;
        expect(drawCommand(draw) == RECOMP_D3D_PRESENTER_OK, "draw accepted");
        image = present();
        expect(pixelIs(image, 40, 40, 0xff, 0, 0, 8), "top-left texel red");
        expect(pixelIs(image, 360, 40, 0, 0xff, 0, 8), "top-right texel green");
        expect(pixelIs(image, 40, 360, 0, 0, 0xff, 8), "bottom-left texel blue");
        expect(pixelIs(image, 360, 360, 0xff, 0xff, 0xff, 8), "bottom-right texel white");
    }

    std::printf("DXT1 texture (driver S3TC)\n");
    {
        clear(0xff000000u);
        // One 4x4 block: color0 = pure red (0xf800), color1 = blue (0x001f), all indices 0.
        const uint8_t block[8] = {0x00, 0xf8, 0x1f, 0x00, 0, 0, 0, 0};
        const RhwTex vertices[4] = {{0, 0, 0.5f, 1, 0, 0}, {100, 0, 0.5f, 1, 1, 0}, {0, 100, 0.5f, 1, 0, 1},
            {100, 100, 0.5f, 1, 1, 1}};
        const uint16_t indices[4] = {0, 1, 2, 3};
        RecompD3dPresenterDrawCommand draw = baseDraw();
        draw.fvf = 0x104u;
        draw.primitive_type = 6u;
        draw.vertex_stride = sizeof(RhwTex);
        draw.vertex_count = 4u;
        draw.index_count = 4u;
        draw.triangle_count = 2u;
        draw.vertex_bytes = vertices;
        draw.index_bytes = indices;
        draw.has_texture = true;
        draw.texture.format_byte = RECOMP_D3D_TEXTURE_FORMAT_DXT1;
        draw.texture.bits_per_pixel = 4u;
        draw.texture.width = 4u;
        draw.texture.height = 4u;
        draw.texture.data = 0x200000u;
        draw.texture.mip_levels = 1u;
        draw.texture_bytes = block;
        draw.texture_byte_count = 8u;
        expect(drawCommand(draw) == RECOMP_D3D_PRESENTER_OK, "draw accepted");
        image = present();
        expect(pixelIs(image, 50, 50, 0xff, 0, 0, 8), "DXT1 block decodes red");
    }

    std::printf("transformed draw with depth test\n");
    {
        clear(0xff000000u);
        // Two full-screen quads in clip space (identity transform): near green, then far red.
        const XyzDiffuse near_quad[4] = {{-1, 1, 0.2f, 0xff00ff00u}, {1, 1, 0.2f, 0xff00ff00u},
            {-1, -1, 0.2f, 0xff00ff00u}, {1, -1, 0.2f, 0xff00ff00u}};
        const XyzDiffuse far_quad[4] = {{-1, 1, 0.8f, 0xffff0000u}, {1, 1, 0.8f, 0xffff0000u},
            {-1, -1, 0.8f, 0xffff0000u}, {1, -1, 0.8f, 0xffff0000u}};
        const uint16_t indices[4] = {0, 1, 2, 3};
        for (const XyzDiffuse *quad : {near_quad, far_quad}) {
            RecompD3dPresenterDrawCommand draw = baseDraw();
            draw.fvf = 0x42u;
            draw.primitive_type = 6u;
            draw.vertex_stride = sizeof(XyzDiffuse);
            draw.vertex_count = 4u;
            draw.index_count = 4u;
            draw.triangle_count = 2u;
            draw.vertex_bytes = quad;
            draw.index_bytes = indices;
            draw.has_transform = true;
            for (int i = 0; i < 4; ++i) draw.transform[i * 5] = 1.0f;
            draw.depth.depth_test_enable = true;
            draw.depth.depth_write_enable = true;
            const auto error = drawCommand(draw);
            if (error != RECOMP_D3D_PRESENTER_OK) std::printf("  draw error %d\n", error);
        }
        image = present();
        expect(pixelIs(image, 320, 240, 0, 0xff, 0), "the nearer quad wins the depth test");
    }

    std::printf("alpha blending\n");
    {
        clear(0xff0000ffu);
        const RhwDiffuse vertices[4] = {{0, 0, 0.5f, 1, 0x80ff0000u}, {640, 0, 0.5f, 1, 0x80ff0000u},
            {0, 480, 0.5f, 1, 0x80ff0000u}, {640, 480, 0.5f, 1, 0x80ff0000u}};
        const uint16_t indices[4] = {0, 1, 2, 3};
        RecompD3dPresenterDrawCommand draw = baseDraw();
        draw.fvf = 0x44u;
        draw.primitive_type = 6u;
        draw.vertex_stride = sizeof(RhwDiffuse);
        draw.vertex_count = 4u;
        draw.index_count = 4u;
        draw.triangle_count = 2u;
        draw.vertex_bytes = vertices;
        draw.index_bytes = indices;
        draw.blend.blend_enable = true;
        draw.blend.src_factor = RECOMP_D3D_BLEND_SRC_ALPHA;
        draw.blend.dst_factor = RECOMP_D3D_BLEND_INV_SRC_ALPHA;
        draw.blend.op = RECOMP_D3D_BLEND_OP_ADD;
        drawCommand(draw);
        image = present();
        expect(pixelIs(image, 320, 240, 0x80, 0, 0x7f, 4), "half red over blue");
    }

    std::printf("render target sampled by the next draw\n");
    {
        RecompD3dPresenterTarget target{};
        target.offscreen = true;
        target.no_depth = true;
        target.color.format_byte = 0x12u;
        target.color.bits_per_pixel = 32u;
        target.color.linear = true;
        target.color.render_target = true;
        target.color.width = 64u;
        target.color.height = 64u;
        target.color.pitch = 256u;
        target.color.data = 0x300000u;
        clear(0xff00ff00u, target, false);  // the target turns green
        // Top half of the target red.
        const RhwDiffuse top[4] = {{0, 0, 0.5f, 1, 0xffff0000u}, {64, 0, 0.5f, 1, 0xffff0000u},
            {0, 32, 0.5f, 1, 0xffff0000u}, {64, 32, 0.5f, 1, 0xffff0000u}};
        const uint16_t indices[4] = {0, 1, 2, 3};
        RecompD3dPresenterDrawCommand draw = baseDraw();
        draw.fvf = 0x44u;
        draw.primitive_type = 6u;
        draw.vertex_stride = sizeof(RhwDiffuse);
        draw.vertex_count = 4u;
        draw.index_count = 4u;
        draw.triangle_count = 2u;
        draw.vertex_bytes = top;
        draw.index_bytes = indices;
        draw.target = target;
        expect(drawCommand(draw) == RECOMP_D3D_PRESENTER_OK, "draw into the target accepted");
        clear(0xff000000u);
        // Xbox linear textures (0x12) take texel coordinates, not normalized ones.
        const RhwTex quad[4] = {{0, 0, 0.5f, 1, 0, 0}, {256, 0, 0.5f, 1, 64, 0}, {0, 256, 0.5f, 1, 0, 64},
            {256, 256, 0.5f, 1, 64, 64}};
        RecompD3dPresenterDrawCommand sample = baseDraw();
        sample.fvf = 0x104u;
        sample.primitive_type = 6u;
        sample.vertex_stride = sizeof(RhwTex);
        sample.vertex_count = 4u;
        sample.index_count = 4u;
        sample.triangle_count = 2u;
        sample.vertex_bytes = quad;
        sample.index_bytes = indices;
        sample.has_texture = true;
        sample.texture = target.color;
        sample.texture_bytes = nullptr;
        sample.texture_byte_count = 0u;
        sample.address_u = sample.address_v = 3u;
        expect(drawCommand(sample) == RECOMP_D3D_PRESENTER_OK, "sampling draw accepted");
        image = present();
        expect(pixelIs(image, 128, 40, 0xff, 0, 0, 8), "the target's top half (red) is on top");
        expect(pixelIs(image, 128, 220, 0, 0xff, 0, 8), "the target's bottom half (green) is below");
    }

    std::printf("pixel-exact edges (D3D top-left rule)\n");
    if (scale == 1.0f) {
        clear(0xff000000u);
        // Pretransformed quad x 4..12, y 4..12: D3D integer pixel centres
        // cover columns 4..11 and rows 4..11 exactly.
        const RhwDiffuse vertices[4] = {{4, 4, 0.5f, 1, 0xffffffffu}, {12, 4, 0.5f, 1, 0xffffffffu},
            {4, 12, 0.5f, 1, 0xffffffffu}, {12, 12, 0.5f, 1, 0xffffffffu}};
        const uint16_t indices[4] = {0, 1, 2, 3};
        RecompD3dPresenterDrawCommand draw = baseDraw();
        draw.fvf = 0x44u;
        draw.primitive_type = 6u;
        draw.vertex_stride = sizeof(RhwDiffuse);
        draw.vertex_count = 4u;
        draw.index_count = 4u;
        draw.triangle_count = 2u;
        draw.vertex_bytes = vertices;
        draw.index_bytes = indices;
        drawCommand(draw);
        image = present();
        expect(pixelIs(image, 4, 4, 0xff, 0xff, 0xff), "column 4, row 4 covered");
        expect(pixelIs(image, 11, 11, 0xff, 0xff, 0xff), "column 11, row 11 covered");
        expect(pixelIs(image, 12, 8, 0, 0, 0), "column 12 not covered");
        expect(pixelIs(image, 8, 12, 0, 0, 0), "row 12 not covered");
        expect(pixelIs(image, 3, 8, 0, 0, 0), "column 3 not covered");
        expect(pixelIs(image, 8, 3, 0, 0, 0), "row 3 not covered");
    }

    std::printf("visibility test\n");
    {
        clear(0xff000000u);
        RecompD3dPresenterCommand begin{};
        begin.type = RECOMP_D3D_PRESENTER_COMMAND_VISIBILITY_BEGIN;
        submit(begin);
        const RhwDiffuse vertices[4] = {{10, 10, 0.5f, 1, 0xffffffffu}, {110, 10, 0.5f, 1, 0xffffffffu},
            {10, 60, 0.5f, 1, 0xffffffffu}, {110, 60, 0.5f, 1, 0xffffffffu}};
        const uint16_t indices[4] = {0, 1, 2, 3};
        RecompD3dPresenterDrawCommand draw = baseDraw();
        draw.fvf = 0x44u;
        draw.primitive_type = 6u;
        draw.vertex_stride = sizeof(RhwDiffuse);
        draw.vertex_count = 4u;
        draw.index_count = 4u;
        draw.triangle_count = 2u;
        draw.vertex_bytes = vertices;
        draw.index_bytes = indices;
        drawCommand(draw);
        RecompD3dPresenterCommand end{};
        end.type = RECOMP_D3D_PRESENTER_COMMAND_VISIBILITY_END;
        end.data.visibility_index = 7u;
        submit(end);
        present();
        bool complete = false;
        uint32_t pixels = 0u;
        for (int i = 0; i < 200 && !complete; ++i) {
            recomp_d3d_presenter_flush(presenter, true);
            recomp_d3d_presenter_visibility_result(presenter, 7u, &complete, &pixels);
        }
        std::printf("       visible pixels %u\n", pixels);
        expect(complete && pixels == 100u * 50u, "100x50 rectangle counts 5000 pixels");
    }

    std::printf("coverage probe\n");
    {
        const XyzDiffuse quad[4] = {{-1, 1, 0.5f, 0}, {1, 1, 0.5f, 0}, {-1, -1, 0.5f, 0}, {1, -1, 0.5f, 0}};
        const uint16_t indices[4] = {0, 1, 2, 3};
        RecompD3dPresenterDrawCommand draw = baseDraw();
        draw.fvf = 0x42u;
        draw.primitive_type = 6u;
        draw.vertex_stride = sizeof(XyzDiffuse);
        draw.vertex_count = 4u;
        draw.index_count = 4u;
        draw.triangle_count = 2u;
        draw.vertex_bytes = quad;
        draw.index_bytes = indices;
        draw.has_transform = true;
        for (int i = 0; i < 4; ++i) draw.transform[i * 5] = 1.0f;
        bool covered = false;
        const auto error = recomp_d3d_presenter_draw_coverage(presenter, &draw, &covered);
        expect(error == RECOMP_D3D_PRESENTER_OK && covered, "a full-screen quad covers");
        for (int i = 0; i < 4; ++i) draw.transform[12 + i] = i == 0 ? 5.0f : draw.transform[12 + i];
        covered = true;
        recomp_d3d_presenter_draw_coverage(presenter, &draw, &covered);
        expect(!covered, "a quad moved off-screen does not");
    }

    std::printf("gamma ramp\n");
    {
        RecompD3dPresenterCommand gamma{};
        gamma.type = RECOMP_D3D_PRESENTER_COMMAND_GAMMA;
        for (int c = 0; c < 3; ++c)
            for (int i = 0; i < 256; ++i) gamma.data.gamma[c][i] = static_cast<uint8_t>(255 - i);
        submit(gamma);
        clear(0xff000000u);
        image = present();
        // The dump holds the guest pixels; gamma is display state only.
        expect(pixelIs(image, 10, 10, 0, 0, 0), "guest pixels stay uncorrected");
    }

    recomp_d3d_presenter_destroy(&presenter);
    std::printf("%s (%d failures)\n", failures ? "FAILED" : "all passed", failures);
    return failures ? 1 : 0;
}
