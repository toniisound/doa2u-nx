// SPDX-License-Identifier: GPL-3.0-or-later
// shader_specialize on upstream's real draw shaders: the Transform layout is
// read as d3d_draw_shader.h sizes it, literals replace only single-component
// flag reads, and every boot FVF still compiles to valid GLSL with flags
// fixed to each value they take.
#include "d3d_draw_shader.h"
#include "d3d_shader_compiler.h"
#include "shader_specialize.h"

#include <cstdio>
#include <random>
#include <string>
#include <vector>

namespace {
int failures = 0;
void expect(bool ok, const char *what)
{
    if (!ok) { std::printf("FAIL %s\n", what); ++failures; }
}
} // namespace

int main()
{
    using namespace shader_specialize;
    // Synthetic text: declaration untouched, xy swizzle kept, .x replaced.
    const std::string text = "cbuffer Transform : register(b0) {\n    row_major float4x4 wvp[4];\n"
        "    float4 draw_flags;\n    float4 blend_flags;\n}\nstruct VSIn { float x; };\n"
        "float a = draw_flags.x + draw_flags.xy.x + blend_flags.y + mydraw_flags.x;\n";
    std::vector<FlagVector> flags;
    uint32_t floats = 0u;
    expect(parseFlagVectors(text, flags, floats) && floats == 72u && flags.size() == 2u &&
        flags[0].offset == 64u && flags[1].offset == 68u, "synthetic layout");
    float transform[72] = {};
    transform[64] = 1.0f;   // draw_flags.x
    transform[65] = 0.25f;  // continuous: stays a uniform
    transform[69] = 3.0f;   // blend_flags.y
    std::vector<Literal> literals;
    collect(flags, transform, floats, literals);
    const std::string out = apply(text, flags, literals);
    expect(out.find("float a = (1.0f) + draw_flags.xy.x + (3.0f) + mydraw_flags.x;") != std::string::npos,
        "synthetic replacement");
    expect(out.find("float4 draw_flags;") != std::string::npos, "declaration kept");

    // Real shaders.
    std::mt19937 random(7u);
    int total = 0;
    for (const uint32_t fvf : kRecompD3dBootDrawFvfs) {
        RecompD3dDrawShaderKey key{fvf, 0u, nullptr, false, false, false, false};
        RecompD3dVertexLayout layout;
        std::string hlsl;
        if (!recomp_d3d_draw_shader_source(key, layout, hlsl)) continue;
        hlsl = recomp_shader_draw_bindings(hlsl);
        std::vector<FlagVector> real;
        expect(parseFlagVectors(hlsl, real, floats), "real layout parsed");
        expect(floats == kRecompD3dDrawConstantFloats - 192u * 4u, "real layout size matches the constants");
        for (int trial = 0; trial < 6; ++trial) {
            std::vector<float> values(floats, 0.5f);
            for (const FlagVector &flag : real)
                for (unsigned c = 0u; c < 4u; ++c) values[flag.offset + c] = static_cast<float>(random() % (trial == 0 ? 1u : 9u));
            std::vector<Literal> set;
            collect(real, values.data(), floats, set);
            const std::string specialized = apply(hlsl, real, set);
            RecompCompiledShader vs, fs;
            std::string error;
            ++total;
            if (!recomp_shader_compile(specialized, "vs_main", true, RecompShaderTarget::Glsl, {}, vs, error) ||
                !recomp_shader_compile(specialized, "ps_main", false, RecompShaderTarget::Glsl, {}, fs, error)) {
                std::printf("fvf %03x trial %d: %s\n", fvf, trial, error.c_str());
                ++failures;
            }
        }
    }
    std::printf("%s shader specialization (%d variants compiled)\n", failures ? "FAIL" : "PASS", total);
    return failures != 0;
}
