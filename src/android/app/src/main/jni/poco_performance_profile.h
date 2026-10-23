// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "android_settings.h"
#include "common/settings.h"

namespace PocoX7Pro {

inline void ApplyPerformanceProfile(bool global) {
    const auto set = [global](auto& setting, const auto& value) {
        const bool was_global = setting.UsingGlobal();
        setting.SetGlobal(global);
        setting.SetValue(value);
        // Applying globally must not replace an already selected per-game override.
        if (global) {
            setting.SetGlobal(was_global);
        }
    };

    auto& values = Settings::values;
    set(values.use_multi_core, true);
    set(values.cpu_backend, values.cpu_backend.GetDefault());
    set(values.cpu_accuracy, Settings::CpuAccuracy::Auto);
    set(values.cpuopt_fastmem, true);
    set(values.cpuopt_fastmem_exclusives, true);
    set(values.cpu_clock, Settings::CpuClock::Normal);
    set(values.use_custom_cpu_ticks, false);
    set(values.use_speed_limit, true);
    set(values.speed_limit, 100);
    set(values.use_docked_mode, Settings::ConsoleMode::Handheld);
    set(values.memory_layout_mode, Settings::MemoryLayout::Memory_4Gb);

    set(values.renderer_backend, Settings::RendererBackend::Vulkan);
    set(values.resolution_setup, Settings::ResolutionSetup::Res1_2X);
    set(values.scaling_filter, Settings::ScalingFilter::Bilinear);
    set(values.anti_aliasing, Settings::AntiAliasing::None);
    set(values.max_anisotropy, Settings::AnisotropyMode::Default);
    set(values.sample_shading, 0u);
    set(values.frame_gen, false);
    set(values.post_shader_enabled, false);
    set(values.vsync_mode, Settings::VSyncMode::Fifo);
    set(values.frame_pacing_mode, Settings::FramePacingMode::Target_Auto);

    set(values.use_asynchronous_gpu_emulation, true);
    set(values.async_presentation, true);
    set(values.use_disk_shader_cache, true);
    set(values.use_vulkan_driver_pipeline_cache, true);
    set(AndroidSettings::values.pipeline_worker_count, 2);
    set(values.vram_usage_mode, Settings::VramUsageMode::Conservative);
    set(values.accelerate_astc, Settings::AstcDecodeMode::Gpu);
    set(values.astc_recompression, Settings::AstcRecompression::Uncompressed);
    set(values.nvdec_emulation, Settings::NvdecEmulation::Cpu);

    // Retain completion/readback correctness while reducing the rendering workload.
    set(values.gpu_accuracy, Settings::GpuAccuracy::High);
    set(values.use_reactive_flushing, true);
    set(values.dma_accuracy, Settings::DmaAccuracy::Default);
    set(values.gpu_fence_behavior, Settings::GpuFenceBehavior::Default);
    set(values.use_asynchronous_shaders, false);
    set(values.skip_cpu_inner_invalidation, false);
    set(values.nce_invalidation_gpu_readback, false);
    set(values.gpu_clock, Settings::GpuClock::Normal);
    set(values.renderer_force_max_clock, false);
}

} // namespace PocoX7Pro
