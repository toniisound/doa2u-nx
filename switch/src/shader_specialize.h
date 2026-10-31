// SPDX-License-Identifier: GPL-3.0-or-later
/* Draw shader specialization for the Switch's OpenGL backend.

   Upstream's draw shaders are uber shaders: every fixed-function feature is a
   branch on a flag in the Transform constant buffer, and the vertex lighting
   loop runs to a count read from it. A specialized variant replaces the flag
   components a draw actually has with literals of the same value, so it
   computes exactly what the uber shader computes for that draw, while the
   GPU's compiler can drop the dead branches, unroll the light loop and keep
   fewer registers live. */
#ifndef DOA2U_NX_SHADER_SPECIALIZE_H
#define DOA2U_NX_SHADER_SPECIALIZE_H

#include <cstdint>
#include <string>
#include <vector>

namespace shader_specialize {

/* One flag vector of the Transform cbuffer and its offset in floats. */
struct FlagVector {
    std::string name;
    uint32_t offset;
};

/* The flag vectors, located in the HLSL's "cbuffer Transform" declaration.
   Returns false when the declaration cannot be read (nothing is specialized
   then). */
bool parseFlagVectors(const std::string &hlsl, std::vector<FlagVector> &flags, uint32_t &transform_floats);

/* A component fixed to a literal: flags[vector] component (0-3 for xyzw). */
struct Literal {
    uint16_t vector;
    uint8_t component;
    uint8_t value;  // a small whole number (0-8)
};

/* The literals for one draw: each flag component whose value in the
   Transform floats is a whole number from 0 to 8 (continuous values such as
   scales and alpha references stay uniforms). Returns a hash of them. */
uint64_t collect(const std::vector<FlagVector> &flags, const float *transform, uint32_t transform_floats,
    std::vector<Literal> &literals);

/* The HLSL with each literal's single-component reads (name.x and the like)
   replaced by the literal. Swizzles of several components stay uniform reads. */
std::string apply(const std::string &hlsl, const std::vector<FlagVector> &flags,
    const std::vector<Literal> &literals);

} // namespace shader_specialize

#endif
