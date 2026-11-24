// SPDX-FileCopyrightText: Copyright 2026 gow3_pc contributors
// SPDX-License-Identifier: GPL-2.0-or-later

// gow3 24 Nov 2026 (The Last of Us Remastered, #58; found by Guistevan, PR #232): vertical stripes
// fixed to screen columns on walls, on NVIDIA RTX GPUs (an RTX 5060 measured, 4070 and 5070 Ti
// reported). They are already in the half-size ambient occlusion mask (cs aaae0737), every 15
// mask pixels at 1080p, before FXAA and the window's scaling.
//
// Each thread of that shader computes one mask texel x and reads the center depth and normal of
// the full-size buffers at u = (x + 0.5) * s, s being 1 / the mask's width, from the shader's
// constants. For a half-size mask that is exactly the boundary between the source texels 2x and
// 2x + 1: s is not exact in floating point, so u * width lands a little before or after it,
// depending on x, and the sampler takes one or the other neighbor in a repeating pattern. The
// GPUs that show it seem to drop the coordinate's error the other way from those that do not.
//
// This pass reads those two texels at the center of the texel the exact arithmetic gives,
// floor((2x + 1) * width / (2 * mask width)), with the mask's width the shader's own 1 / s,
// rounded. The sample stays a sample (same sampler, same conversions); at a texel's center it
// takes that texel whatever the coordinate's last bit. The occlusion kernel's other reads (of
// another buffer, at offsets from the center) are left as they are. GOW3_AO_CENTER_TEXEL=0: the
// game's coordinates.

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "shader_recompiler/info.h"
#include "shader_recompiler/ir/ir_emitter.h"
#include "shader_recompiler/ir/passes/ir_passes.h"
#include "shader_recompiler/ir/program.h"

namespace Shader::Optimization {

namespace {

constexpr u64 AmbientOcclusion = 0xaaae0737ULL; // update 1.11

bool AoCenterTexelOn() {
    static const bool on = [] {
        const char* env = std::getenv("GOW3_AO_CENTER_TEXEL");
        return !(env && std::strcmp(env, "0") == 0);
    }();
    return on;
}

void Report(u64 pgm_hash, const char* what) {
    std::printf("Shader: cs %#llx (ambient occlusion centers): %s\n",
                static_cast<unsigned long long>(pgm_hash), what);
    std::fflush(stdout);
}

bool IsImmediate(const IR::Value& value, float f) {
    return value.IsImmediate() && value.Type() == IR::Type::F32 && value.F32() == f;
}

/// A coordinate of the form s * (0.5 + float(id)), in either order.
struct CenterCoord {
    IR::Value texel_size;
    IR::Value thread;
};

bool MatchCenterCoord(const IR::Value& coord, CenterCoord& out) {
    IR::Inst* const mul = coord.TryInst();
    if (!mul || mul->GetOpcode() != IR::Opcode::FPMul32) {
        return false;
    }
    for (u32 side = 0; side < 2; ++side) {
        IR::Inst* const add = mul->Arg(side).TryInst();
        if (!add || add->GetOpcode() != IR::Opcode::FPAdd32) {
            continue;
        }
        for (u32 term = 0; term < 2; ++term) {
            IR::Inst* const convert = add->Arg(term).TryInst();
            if (IsImmediate(add->Arg(1 - term), 0.5f) && convert &&
                convert->GetOpcode() == IR::Opcode::ConvertF32U32) {
                out.texel_size = mul->Arg(1 - side);
                out.thread = convert->Arg(0);
                return true;
            }
        }
    }
    return false;
}

} // namespace

bool AoCenterTexel(u64 pgm_hash) {
    return pgm_hash == AmbientOcclusion && AoCenterTexelOn();
}

void AoCenterTexelPass(IR::Program& program) {
    if (!AoCenterTexel(program.info.pgm_hash)) {
        return;
    }
    // The shader as update 1.11 has it: two samples (depth, normals) at the texel's own center,
    // the kernel's at offsets from it.
    struct Center {
        IR::Inst* sample;
        IR::Inst* address;
        std::array<CenterCoord, 2> coords;
    };
    std::vector<Center> centers;
    for (IR::Block* const block : program.blocks) {
        for (IR::Inst& inst : block->Instructions()) {
            if (inst.GetOpcode() != IR::Opcode::ImageSampleRaw) {
                continue;
            }
            const auto flags = inst.Flags<IR::TextureInstInfo>();
            IR::Inst* const address = inst.Arg(2).TryInst();
            Center center{&inst, address, {}};
            if (!flags.is_unnormalized && !flags.has_offset && !flags.is_gather && address &&
                address->GetOpcode() == IR::Opcode::CompositeConstructF32x4 &&
                MatchCenterCoord(address->Arg(0), center.coords[0]) &&
                MatchCenterCoord(address->Arg(1), center.coords[1])) {
                centers.push_back(center);
            }
        }
    }
    if (centers.size() != 2) {
        Report(program.info.pgm_hash, "not the expected code, left as it is");
        return;
    }
    for (const Center& center : centers) {
        IR::Inst* const sample = center.sample;
        IR::IREmitter ir{*sample->GetParent(), IR::Block::InstructionList::s_iterator_to(*sample)};
        IR::TextureInstInfo query_flags{};
        const auto flags = sample->Flags<IR::TextureInstInfo>();
        query_flags.is_array.Assign(flags.is_array.Value());
        query_flags.is_r128.Assign(flags.is_r128.Value());
        const IR::Value size =
            ir.ImageQueryDimension(sample->Arg(0), ir.Imm32(0u), ir.Imm1(false), query_flags);
        // A copy of the address (other reads may share it), with the two centers replaced.
        IR::Value coords[4];
        for (u32 i = 0; i < 4; ++i) {
            coords[i] = center.address->Arg(i);
        }
        for (u32 axis = 0; axis < 2; ++axis) {
            const CenterCoord& coord = center.coords[axis];
            const IR::U32 source{ir.CompositeExtract(size, axis)};
            const IR::F32 texel_size{coord.texel_size};
            const IR::U32 target = ir.UMax(
                IR::U32{ir.ConvertFToU(32, ir.FPRoundEven(ir.FPRecip(texel_size)))},
                ir.Imm32(1u));
            const IR::U32 twice_plus_one{ir.IAdd(
                IR::U32{ir.ShiftLeftLogical(IR::U32{coord.thread}, ir.Imm32(1u))}, ir.Imm32(1u))};
            const IR::U32 numerator{ir.IMul(twice_plus_one, source)};
            const IR::U32 denominator{ir.ShiftLeftLogical(target, ir.Imm32(1u))};
            const IR::U32 texel = ir.UMin(ir.IDiv(numerator, denominator),
                                          IR::U32{ir.ISub(source, ir.Imm32(1u))});
            const IR::F32 center{ir.FPAdd(IR::F32{ir.ConvertUToF(32, 32, texel)}, ir.Imm32(0.5f))};
            coords[axis] = ir.FPDiv(center, IR::F32{ir.ConvertUToF(32, 32, source)});
        }
        sample->SetArg(2, ir.CompositeConstruct(coords[0], coords[1], coords[2], coords[3]));
    }
    static bool reported = false;
    if (!reported) {
        reported = true;
        Report(program.info.pgm_hash, "the depth and normals read at the center of the texel "
                                      "each mask pixel covers (#58)");
    }
}

} // namespace Shader::Optimization
