#ifndef DOA2U_RECOMP_D3D_SHADER_COMPILER_H
#define DOA2U_RECOMP_D3D_SHADER_COMPILER_H

#include <cstdint>
#include <string>
#include <vector>

/* Compiles the presenter's HLSL for SDL_GPU: shaderc (glslang's HLSL front
   end) produces SPIR-V, which Vulkan takes as is and SPIRV-Cross turns into
   MSL for Metal. Resources must already carry Vulkan bindings in SDL_GPU's
   layout: vertex uniform buffers in set 1, fragment textures and samplers in
   set 2 (each texture shares its sampler's binding), fragment uniform buffers
   in set 3. */

/* Glsl is GLSL 4.30 core for an OpenGL backend (the Switch port), built
   with glslang and SPIRV-Cross linked in (RECOMP_SHADER_GLSLANG). There,
   uniform buffers take GL binding = slot in the vertex stage and slot + 2 in
   the fragment stage (each block renamed per stage, so a program links), each
   texture and its sampler become one combined sampler on the texture's slot,
   and stage interfaces keep their [[vk::location]] numbers. */
enum class RecompShaderTarget { Spirv, Msl, Glsl };

struct RecompCompiledShader {
    std::vector<uint8_t> code;  // SPIR-V words, or MSL/GLSL text without a terminator
    std::string entry_point;
};

/* Whether shaderc could be loaded; the first call reports why it could not. */
bool recomp_shader_compiler_available();

/* Gives the draw shader from d3d_draw_shader.cpp SDL_GPU's bindings: the
   Transform constant buffer is slot 0 of its stage, the vertex program
   constants vc[] move to their own buffer in vertex slot 1 (SDL's Vulkan
   backend takes at most 4 KB per slot), the four textures take sampler slots
   0-3 (the volume texture gets its own sampler), vertex diffuse is read in
   SDL's byte order, and every interpolant and vertex input gets the next
   location in declaration order. */
std::string recomp_shader_draw_bindings(const std::string &hlsl);

/* Metal samplers have no LOD bias, and SDL drops SDL_GPU's mip_lod_bias there.
   This adds the bias of each sampler slot in the fragment shader instead: a
   SamplerBias constant buffer (fragment slot 1) holds one bias per slot, and
   each Sample becomes SampleBias with it. */
std::string recomp_shader_emulate_sampler_bias(const std::string &hlsl);

/* Vertex shaders see RECOMP_UNIFORM_SET as 1 and fragment shaders as 3, so
   [[vk::binding(n, RECOMP_UNIFORM_SET)]] places a constant buffer in either. */

/* defines are NAME or NAME=VALUE preprocessor definitions. Thread-safe.
   With RECOMP_SHADER_CACHE set to a folder, results are kept there. */
bool recomp_shader_compile(const std::string &hlsl, const char *entry_point, bool vertex,
    RecompShaderTarget target, const std::vector<std::string> &defines,
    RecompCompiledShader &out, std::string &error);

#endif
