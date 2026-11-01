// SPDX-License-Identifier: GPL-3.0-or-later
#include "shader_specialize.h"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace shader_specialize {
namespace {

/* Vectors whose components are feature switches and modes. Others (matrices,
   colors, light data) are never specialized. Values that are not small whole
   numbers stay uniforms even here, so a continuous component in this list
   costs variants, never correctness. */
const char *const kFlagVectors[] = {
    "draw_flags", "blend_flags", "lighting_flags", "texture_flags", "reflection_flags",
    "directional_flags", "light_stage", "filter_flags", "filter_texel", "reflection_stage2_flags",
    "projected_stages",
};

bool isIdentifier(char c)
{
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

} // namespace

bool parseFlagVectors(const std::string &hlsl, std::vector<FlagVector> &flags, uint32_t &transform_floats)
{
    flags.clear();
    transform_floats = 0u;
    const size_t start = hlsl.find("cbuffer Transform");
    if (start == std::string::npos) return false;
    const size_t open = hlsl.find('{', start);
    const size_t close = hlsl.find('}', open);
    if (open == std::string::npos || close == std::string::npos) return false;
    uint32_t offset = 0u;
    size_t at = open + 1u;
    while (at < close) {
        const size_t end = hlsl.find(';', at);
        if (end == std::string::npos || end > close) break;
        std::string member = hlsl.substr(at, end - at);
        at = end + 1u;
        // "[row_major] float4|float4x4 name[count]"
        uint32_t size;
        size_t type_end;
        if ((type_end = member.find("float4x4")) != std::string::npos) {
            size = 16u;
            type_end += 8u;
        } else if ((type_end = member.find("float4")) != std::string::npos) {
            size = 4u;
            type_end += 6u;
        } else {
            return false;  // a member type this parser does not size
        }
        size_t name_begin = type_end;
        while (name_begin < member.size() && std::isspace(static_cast<unsigned char>(member[name_begin]))) ++name_begin;
        size_t name_end = name_begin;
        while (name_end < member.size() && isIdentifier(member[name_end])) ++name_end;
        const std::string name = member.substr(name_begin, name_end - name_begin);
        if (name.empty()) return false;
        uint32_t count = 1u;
        const size_t bracket = member.find('[', name_end);
        if (bracket != std::string::npos) count = static_cast<uint32_t>(std::strtoul(member.c_str() + bracket + 1u, nullptr, 10));
        if (count == 0u) return false;
        for (const char *flag : kFlagVectors) {
            if (name == flag && count == 1u && size == 4u) flags.push_back({name, offset});
        }
        offset += size * count;
    }
    transform_floats = offset;
    return !flags.empty();
}

namespace {
template <typename Visit>
uint64_t forEachLiteral(const std::vector<FlagVector> &flags, const float *transform, uint32_t transform_floats,
    Visit visit)
{
    uint64_t hash = 0xcbf29ce484222325ull;
    for (uint16_t v = 0u; v < flags.size(); ++v) {
        for (uint8_t c = 0u; c < 4u; ++c) {
            const uint32_t index = flags[v].offset + c;
            if (index >= transform_floats) continue;
            const float value = transform[index];
            if (!(value >= 0.0f && value <= 8.0f) || std::floor(value) != value) continue;
            const auto whole = static_cast<uint8_t>(value);
            visit(Literal{v, c, whole});
            hash = (hash ^ (static_cast<uint64_t>(v) << 16 | static_cast<uint64_t>(c) << 8 | whole)) *
                0x100000001b3ull;
        }
    }
    return hash;
}
} // namespace

uint64_t collect(const std::vector<FlagVector> &flags, const float *transform, uint32_t transform_floats,
    std::vector<Literal> &literals)
{
    literals.clear();
    return forEachLiteral(flags, transform, transform_floats, [&](const Literal &literal) { literals.push_back(literal); });
}

uint64_t hashLiterals(const std::vector<FlagVector> &flags, const float *transform, uint32_t transform_floats)
{
    return forEachLiteral(flags, transform, transform_floats, [](const Literal &) {});
}

std::string apply(const std::string &hlsl, const std::vector<FlagVector> &flags,
    const std::vector<Literal> &literals)
{
    // Literal text per (vector, component); empty keeps the uniform.
    std::vector<std::string> text(flags.size() * 4u);
    for (const Literal &literal : literals) {
        char value[24];
        std::snprintf(value, sizeof value, "(%u.0f)", static_cast<unsigned>(literal.value));
        text[literal.vector * 4u + literal.component] = value;
    }
    // Only the code after the cbuffer declarations is rewritten.
    const size_t body = hlsl.find("struct VSIn");
    std::string out = hlsl.substr(0u, body == std::string::npos ? 0u : body);
    const size_t from = body == std::string::npos ? 0u : body;
    for (size_t i = from; i < hlsl.size();) {
        bool replaced = false;
        if (isIdentifier(hlsl[i]) && (i == 0u || !isIdentifier(hlsl[i - 1u]))) {
            for (size_t v = 0u; v < flags.size() && !replaced; ++v) {
                const std::string &name = flags[v].name;
                if (hlsl.compare(i, name.size(), name) != 0) continue;
                const size_t dot = i + name.size();
                if (dot + 1u >= hlsl.size() || hlsl[dot] != '.') continue;
                const char swizzle = hlsl[dot + 1u];
                const char *const components = "xyzw";
                const char *component = std::strchr(components, swizzle);
                if (swizzle == '\0' || component == nullptr) continue;
                // A single component only: name.x, not name.xy.
                if (dot + 2u < hlsl.size() && isIdentifier(hlsl[dot + 2u])) continue;
                const std::string &literal = text[v * 4u + static_cast<size_t>(component - components)];
                if (literal.empty()) continue;
                out += literal;
                i = dot + 2u;
                replaced = true;
            }
        }
        if (!replaced) {
            // Copy a whole identifier at once so names are only matched at their start.
            if (isIdentifier(hlsl[i])) {
                size_t end = i;
                while (end < hlsl.size() && isIdentifier(hlsl[end])) ++end;
                out.append(hlsl, i, end - i);
                i = end;
            } else {
                out += hlsl[i++];
            }
        }
    }
    return out;
}

} // namespace shader_specialize
