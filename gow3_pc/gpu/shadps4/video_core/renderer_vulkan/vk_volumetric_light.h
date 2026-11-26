// SPDX-FileCopyrightText: Copyright 2026 gow3_pc contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// gow3 24 Nov 2026 (The Last of Us Remastered #55; the technique is Guistevan's, PR #230): the
// volumetric light from a grid of camera rays instead of the game's epipolar lines, which leave
// radial steps, halos at silhouettes and boxes at the volume's edges. GOW3_VOLUMETRIC_RAYS=1
// (the launcher's switch) turns it on: off by default, it is not the PS4's look and costs more.
//
// - After each dispatch of the line samplers (cs bb1d430e sun, 05866787 moon, 06e1a806 spots,
//   cdecdf59 flashlights), the port runs host_shaders/vol_directional_integral.comp or
//   vol_spot_integral.comp (the variant for the shadow map bound: 2D or array) with their
//   pipeline layout and descriptors, which writes the grid over their lines.
// - The resolves (ps ad16e118 sun and moon, ca25dbad spots and flashlights) are
//   host_shaders/vol_directional_resolve.frag and vol_spot_resolve.frag: the translations with a
//   path for the grid (the epipolar path kept as translated).
// Each is used only when its bindings, push constants and outputs are those of the translated
// shader (checked in the SPIR-V); otherwise the game's shaders run as they are, with a log line.
// A light's grid is written only once its resolve is the port's, so the two never mismatch.

#include <span>
#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

VK_DEFINE_HANDLE(VmaAllocator)

namespace Shader {
struct Info;
}

namespace Vulkan::VolumetricLight {

bool On();

/// The SPIR-V to create this program's module from: the translation, or the port's resolve.
/// Also records which grid variants the line samplers' translations fit.
std::span<const u32> ModuleCode(const Shader::Info& info, std::span<const u32> translated);

/// The grid variant that follows this dispatch of a line sampler, or -1 for none.
int GridVariant(const Shader::Info& cs);

/// The grid pass's pipeline for that variant and the line sampler's pipeline layout (created on
/// first use; null if it cannot be).
vk::Pipeline GridPipeline(vk::Device device, vk::PipelineLayout layout, int variant);

/// gow3 25 Nov 2026: while the game is paused or in photo mode it dispatches the same line
/// sampler (and draws its resolve) ~24 times a frame with the same inputs (traced: same
/// constants, same images). The game's dispatch always runs (it also clears an image another
/// pass accumulates into: skipping it left a black patch at the sun), but the grid is computed
/// once per frame and key (shader, the constant words it reads, the bound images) and copied
/// back over the lines for the repeats. GOW3_VOL_REPEATS=0 computes every one.
bool IsLineSampler(u64 pgm_hash);
/// The constant dwords (from its buffer's start) the line sampler reads, as [begin, end)
/// ranges (from the translations; the buffer's other words change while paused, unread).
std::span<const std::pair<u32, u32>> ReadDwords(u64 pgm_hash);
struct GridCopy {
    vk::Image image; ///< null: no cache (compute the grid, keep nothing)
    bool hit;        ///< holds this key's grid of this frame: copy it, no grid pass
    bool fresh;      ///< just created: its layout is still undefined
};
/// The cache image for this key's grid in this frame (format: the line sampler's image's).
GridCopy GridCache(vk::Device device, VmaAllocator allocator, u64 key, vk::Format format);
/// The presenter's flip: the next frame's grids are computed anew.
void NewFrame();

/// Workgroups of the grid pass (8x8 threads, one a cell of 512x512).
constexpr u32 GridGroups = 512 / 8;

/// Exposed for tests: whether every resource, the push constants and the outputs `replacement`
/// declares are declared the same way by `translated`.
bool InterfaceFits(std::span<const u32> translated, std::span<const u32> replacement);

} // namespace Vulkan::VolumetricLight
