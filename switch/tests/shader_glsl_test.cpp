#include "d3d_draw_shader.h"
#include "d3d_shader_compiler.h"
#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>

// Validates the GLSL as OpenGL 4.30 core and links both stages.
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
    const EShMessages m = EShMsgDefault;
    if (!v.parse(GetDefaultResources(), 430, ECoreProfile, false, true, m)) { log = std::string("VS: ") + v.getInfoLog(); return false; }
    if (!f.parse(GetDefaultResources(), 430, ECoreProfile, false, true, m)) { log = std::string("FS: ") + f.getInfoLog(); return false; }
    glslang::TProgram p; p.addShader(&v); p.addShader(&f);
    if (!p.link(m)) { log = std::string("LINK: ") + p.getInfoLog(); return false; }
    return true;
}

int main(int argc, char **argv)
{
    glslang::InitializeProcess();
    int failures = 0, total = 0;
    const bool dump = argc > 1;
    for (const uint32_t fvf : kRecompD3dBootDrawFvfs) {
        for (int variant = 0; variant < 3; ++variant) {
            RecompD3dDrawShaderKey key{fvf, 0u, nullptr, false, variant >= 1, variant == 2};
            RecompD3dVertexLayout layout;
            std::string hlsl;
            if (!recomp_d3d_draw_shader_source(key, layout, hlsl)) { printf("fvf %03x v%d: no source\n", fvf, variant); continue; }
            hlsl = recomp_shader_draw_bindings(hlsl);
            std::vector<std::string> defines;
            if (variant == 2) defines.push_back("RECOMP_SAMPLE_COUNT=1");
            RecompCompiledShader vs, fs; std::string error;
            ++total;
            if (!recomp_shader_compile(hlsl, "vs_main", true, RecompShaderTarget::Glsl, defines, vs, error) ||
                !recomp_shader_compile(hlsl, "ps_main", false, RecompShaderTarget::Glsl, defines, fs, error)) {
                printf("fvf %03x v%d: COMPILE FAILED: %s\n", fvf, variant, error.c_str()); ++failures; continue;
            }
            std::string vtext(vs.code.begin(), vs.code.end()), ftext(fs.code.begin(), fs.code.end()), log;
            if (!validate(vtext, ftext, log)) { printf("fvf %03x v%d: GLSL INVALID: %s\n", fvf, variant, log.c_str()); ++failures; }
            if (dump && fvf == 0x152u + 0 ) {}
            if (dump && variant == 0) {
                char name[64]; snprintf(name, sizeof name, "out/%03x.vert", fvf); std::ofstream(name) << vtext;
                snprintf(name, sizeof name, "out/%03x.frag", fvf); std::ofstream(name) << ftext;
                snprintf(name, sizeof name, "out/%03x.hlsl", fvf); std::ofstream(name) << hlsl;
            }
        }
    }
    printf("%d/%d shader pairs ok\n", total - failures, total);
    return failures != 0;
}
