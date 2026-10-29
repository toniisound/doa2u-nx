#include "d3d_draw_shader.h"
#include "d3d_shader_compiler.h"
#include "d3d_vertex_program.h"
#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>
#include <cstdio>
#include <random>
#include <string>
static bool validate(const std::string &vs, const std::string &fs, std::string &log)
{
    glslang::TShader v(EShLangVertex), f(EShLangFragment);
    const char *vsrc = vs.c_str(), *fsrc = fs.c_str();
    v.setStrings(&vsrc, 1); f.setStrings(&fsrc, 1);
    for (auto *s : {&v, &f}) {
        s->setEnvInput(glslang::EShSourceGlsl, s == &v ? EShLangVertex : EShLangFragment, glslang::EShClientOpenGL, 430);
        s->setEnvClient(glslang::EShClientOpenGL, glslang::EShTargetOpenGL_450);
        s->setEnvTarget(glslang::EShTargetNone, glslang::EShTargetSpv_1_0);
    }
    if (!v.parse(GetDefaultResources(), 430, ECoreProfile, false, true, EShMsgDefault)) { log = v.getInfoLog(); return false; }
    if (!f.parse(GetDefaultResources(), 430, ECoreProfile, false, true, EShMsgDefault)) { log = f.getInfoLog(); return false; }
    glslang::TProgram p; p.addShader(&v); p.addShader(&f);
    if (!p.link(EShMsgDefault)) { log = p.getInfoLog(); return false; }
    return true;
}
int main()
{
    glslang::InitializeProcess();
    std::mt19937 rng(1234);
    const uint32_t fvfs[] = {0x112u, 0x152u, 0x212u, 0x142u, 0x252u};
    int valid = 0, ok = 0, tries = 0, sourced = 0;
    while (valid < 300 && tries < 20000000) {
        ++tries;
        uint32_t count = 1 + rng() % 12;
        static uint32_t prog[136][4];
        for (uint32_t i = 0; i < count; ++i) {
            prog[i][0] = 0; prog[i][1] = rng(); prog[i][2] = rng(); prog[i][3] = rng() & ~3u;
            if (i + 1 == count) prog[i][3] |= 1u;
        }
        const uint32_t fvf = fvfs[rng() % 5];
        std::string body;
        if (!recomp_d3d_vertex_program_source(prog, count, fvf, body)) continue;
        ++valid;
        RecompD3dDrawShaderKey key{fvf, count, prog, false, false, false};
        RecompD3dVertexLayout layout; std::string hlsl;
        if (!recomp_d3d_draw_shader_source(key, layout, hlsl)) continue;
        ++sourced;
        hlsl = recomp_shader_draw_bindings(hlsl);
        RecompCompiledShader vs, fs; std::string error, log;
        if (!recomp_shader_compile(hlsl, "vs_main", true, RecompShaderTarget::Glsl, {}, vs, error) ||
            !recomp_shader_compile(hlsl, "ps_main", false, RecompShaderTarget::Glsl, {}, fs, error)) {
            if (valid - ok < 4) printf("compile fail fvf=%03x count=%u: %.300s\n", fvf, count, error.c_str());
            continue;
        }
        if (!validate(std::string(vs.code.begin(), vs.code.end()), std::string(fs.code.begin(), fs.code.end()), log)) {
            printf("invalid: %.300s\n", log.c_str()); continue;
        }
        ++ok;
    }
    printf("tries=%d accepted=%d sourced=%d glsl_ok=%d\n", tries, valid, sourced, ok);
}
