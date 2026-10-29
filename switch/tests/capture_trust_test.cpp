// SPDX-License-Identifier: GPL-3.0-or-later
// The Switch's trusted texture reuse in D3dCapturePacket: a texture seen at
// the same address earlier in a packet is reused without a compare, but
// vertices are always compared, and an invalidation or release ends the trust.
#include "d3d_presenter_capture.h"

#include <cstdio>
#include <cstring>

namespace {

bool check(bool condition, const char *what)
{
    if (!condition) std::fprintf(stderr, "FAIL %s\n", what);
    return condition;
}

RecompD3dPresenterCommand draw(const unsigned char *vertices, const unsigned char *texture)
{
    RecompD3dPresenterCommand command{};
    command.type = RECOMP_D3D_PRESENTER_COMMAND_DRAW;
    command.data.draw.vertex_bytes = vertices;
    command.data.draw.vertex_count = 4u;
    command.data.draw.vertex_stride = 16u;
    command.data.draw.texture_bytes = texture;
    command.data.draw.texture_byte_count = 256u;
    return command;
}

const unsigned char *texture(const D3dCapturePacket &packet, size_t i)
{
    return static_cast<const unsigned char *>(packet.command(i).data.draw.texture_bytes);
}

const unsigned char *vertices(const D3dCapturePacket &packet, size_t i)
{
    return static_cast<const unsigned char *>(packet.command(i).data.draw.vertex_bytes);
}

bool run(bool trust)
{
    D3dCapturePacket::setTrustTextures(trust);
    unsigned char vb[64], tex[256];
    std::memset(vb, 1, sizeof vb);
    std::memset(tex, 2, sizeof tex);
    D3dCapturePacket packet;
    bool ok = check(packet.add(draw(vb, tex)) == RECOMP_D3D_PRESENTER_OK, "add 0");
    // Change bytes the span key does not sample (it reads 8 spread words).
    tex[100] = 9;
    vb[20] = 9;
    ok &= check(packet.add(draw(vb, tex)) == RECOMP_D3D_PRESENTER_OK, "add 1");
    ok &= check(packet.addInvalidate(0u, 0u) == RECOMP_D3D_PRESENTER_OK, "invalidate");
    ok &= check(packet.add(draw(vb, tex)) == RECOMP_D3D_PRESENTER_OK, "add 2");
    tex[101] = 7;
    ok &= check(packet.addRelease(0u, 0u) == RECOMP_D3D_PRESENTER_OK, "release");
    ok &= check(packet.add(draw(vb, tex)) == RECOMP_D3D_PRESENTER_OK, "add 3");
    packet.seal();
    size_t commands[4], n = 0u;
    for (size_t i = 0u; i < packet.count(); ++i)
        if (packet.record(i).kind == D3dCapturePacket::COMMAND) commands[n++] = i;
    ok &= check(n == 4u, "four commands");
    ok &= check(vertices(packet, commands[1])[20] == 9, "vertices are always compared");
    if (trust) {
        ok &= check(texture(packet, commands[1]) == texture(packet, commands[0]), "trusted reuse");
        ok &= check(texture(packet, commands[1])[100] == 2, "trusted reuse keeps the first copy");
    } else {
        ok &= check(texture(packet, commands[1])[100] == 9, "compared reuse sees the change");
    }
    ok &= check(texture(packet, commands[2])[100] == 9, "invalidation ends the trust");
    ok &= check(texture(packet, commands[3])[101] == 7, "release ends the trust");
    const D3dCaptureStats stats = d3d_capture_take_stats();
    ok &= check(stats.commands == 4u, "stats count commands");
    ok &= check((stats.trusted_bytes != 0u) == trust, "stats count trusted bytes");
    ok &= check(d3d_capture_take_stats().commands == 0u, "take resets stats");
    return ok;
}

} // namespace

int main()
{
    const bool ok = run(false) && run(true);
    std::printf(ok ? "PASS capture trust\n" : "FAIL capture trust\n");
    return ok ? 0 : 1;
}
