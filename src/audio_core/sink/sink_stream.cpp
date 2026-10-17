// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <atomic>
#include <memory>
#include <span>
#include <vector>

#include "audio_core/audio_core.h"
#include "audio_core/common/common.h"
#include "audio_core/sink/sink_stream.h"
#include "common/common_types.h"
#include "common/fixed_point.h"
#include "common/scope_exit.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/core_timing.h"

namespace AudioCore::Sink {

namespace {
u64 HostMicroseconds() {
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
void RecordPeak(std::atomic<u64>& peak, u64 value) {
    auto prior = peak.load(std::memory_order_relaxed);
    while (value > prior &&
           !peak.compare_exchange_weak(prior, value, std::memory_order_relaxed)) {}
}
} // namespace

std::vector<SinkStream::UnderrunTrace> SinkStream::DrainUnderrunTraces(u64& cursor) const {
    const u64 end = underrun_sequence.load(std::memory_order_acquire);
    if (end <= cursor) {
        return {};
    }
    constexpr u64 MaxPerReport = 12;
    const u64 first = (std::max)(cursor + 1, end >= MaxPerReport ? end - MaxPerReport + 1 : 1);
    std::vector<UnderrunTrace> events;
    events.reserve(static_cast<std::size_t>(end - first + 1));
    for (u64 seq = first; seq <= end; ++seq) {
        const auto& slot = underrun_trace[(seq - 1) % TraceCapacity];
        if (slot.sequence.load(std::memory_order_acquire) != seq) {
            continue;
        }
        UnderrunTrace event{seq, slot.host_us.load(std::memory_order_relaxed),
                            slot.missing_frames.load(std::memory_order_relaxed),
                            slot.callback_gap_us.load(std::memory_order_relaxed),
                            slot.producer_age_us.load(std::memory_order_relaxed),
                            slot.pending_buffers.load(std::memory_order_relaxed)};
        if (slot.sequence.load(std::memory_order_acquire) == seq) {
            events.push_back(event);
        }
    }
    cursor = end;
    return events;
}

void SinkStream::AppendBuffer(SinkBuffer& buffer, std::span<s16> samples) {
    if (type == StreamType::In)
        return;

    constexpr s32 min = (std::numeric_limits<s16>::min)();
    constexpr s32 max = (std::numeric_limits<s16>::max)();

    auto yuzu_volume = Settings::Volume();
    if (yuzu_volume > 1.0f)
        yuzu_volume = 0.6f + 20.0f * std::log10(yuzu_volume);
    auto const volume = system_volume * device_volume * yuzu_volume;

    if (system_channels > device_channels) {
        static constexpr std::array<f32, 4> tcoeff{1.0f, 0.596f, 0.354f, 0.707f};
        for (u32 r_offs = 0, w_offs = 0; r_offs < samples.size(); r_offs += system_channels, w_offs += device_channels) {
            std::array<f32, 6> ccoeff{0.f};
            for (u32 i = 0; i < system_channels; ++i)
                ccoeff[i] = f32(samples[r_offs + i]);

            std::array<f32, 6> rcoeff{
                ccoeff[u32(Channels::FrontLeft)],
                ccoeff[u32(Channels::BackLeft)],
                ccoeff[u32(Channels::Center)],
                ccoeff[u32(Channels::LFE)],
                ccoeff[u32(Channels::BackRight)],
                ccoeff[u32(Channels::FrontRight)],
            };

            const f32 left  = rcoeff[0] * tcoeff[0] + rcoeff[2] * tcoeff[1] + rcoeff[3] * tcoeff[2] + rcoeff[1] * tcoeff[3];
            const f32 right = rcoeff[5] * tcoeff[0] + rcoeff[2] * tcoeff[1] + rcoeff[3] * tcoeff[2] + rcoeff[4] * tcoeff[3];

            samples[w_offs + 0] = s16(std::clamp(s32(left * volume),  min, max));
            samples[w_offs + 1] = s16(std::clamp(s32(right * volume), min, max));
        }

        samples_buffer.Push(samples.subspan(0, samples.size() / system_channels * device_channels));
    } else if (system_channels < device_channels) {
        std::vector<s16> new_samples(samples.size() / system_channels * device_channels);
        for (u32 r_offs = 0, w_offs = 0; r_offs < samples.size(); r_offs += system_channels, w_offs += device_channels)
            for (u32 channel = 0; channel < system_channels; ++channel)
                new_samples[w_offs + channel] = s16(std::clamp(s32(f32(samples[r_offs + channel]) * volume), min, max));

        samples_buffer.Push(new_samples);
    } else {
        if (volume != 1.0f) {
            for (u32 i = 0; i < samples.size(); ++i)
                samples[i] = s16(std::clamp(s32(f32(samples[i]) * volume), min, max));
        }

        samples_buffer.Push(samples);
    }

    appended_frames.fetch_add(buffer.frames, std::memory_order_relaxed);
    // Publish the descriptor only after its samples and queue count are ready.
    ++queued_buffers;
    queue.EmplaceWait(buffer);
    const u64 now_us = HostMicroseconds();
    const u64 previous_us = last_append_us.exchange(now_us, std::memory_order_relaxed);
    if (previous_us != 0 && now_us >= previous_us) {
        const u64 gap = now_us - previous_us;
        RecordPeak(peak_append_gap_us, gap);
        if (gap >= 40'000) {
            append_gaps_over_40ms.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

std::vector<s16> SinkStream::ReleaseBuffer(u64 num_samples) {
    auto samples{samples_buffer.Pop(num_samples)};

    // TODO: Up-mix to 6 channels if the game expects it.
    // For audio input this is unlikely to ever be the case though.

    // Incoming mic volume seems to always be very quiet, so multiply by an additional 8 here.
    // TODO: Play with this and find something that works better.
    constexpr s32 min = (std::numeric_limits<s16>::min)();
    constexpr s32 max = (std::numeric_limits<s16>::max)();
    auto volume{system_volume * device_volume * 8};
    for (u32 i = 0; i < samples.size(); i++)
        samples[i] = s16(std::clamp(s32(f32(samples[i]) * volume), min, max));

    if (samples.size() < num_samples)
        samples.resize(num_samples, 0);
    return samples;
}

void SinkStream::ClearQueue() {
    std::scoped_lock lk{release_mutex};

    samples_buffer.Pop();
    SinkBuffer tmp;
    while (queue.TryPop(tmp));

    queued_buffers = 0;
    playing_buffer = {};
    playing_buffer.consumed = true;
}

void SinkStream::ProcessAudioIn(std::span<const s16> input_buffer, std::size_t num_frames) {
    const std::size_t num_channels = GetDeviceChannels();
    const std::size_t frame_size = num_channels;
    const std::size_t frame_size_bytes = frame_size * sizeof(s16);
    size_t frames_written{0};

    // If we're paused or going to shut down, we don't want to consume buffers as coretiming is
    // paused and we'll desync, so just return.
    if (system.IsPaused() || system.IsShuttingDown())
        return;

    while (frames_written < num_frames) {
        // If the playing buffer has been consumed or has no frames, we need a new one
        if (playing_buffer.consumed || playing_buffer.frames == 0) {
            if (!queue.TryPop(playing_buffer)) {
                // If no buffer was available we've underrun, just push the samples and
                // continue.
                samples_buffer.Push(&input_buffer[frames_written * frame_size], (num_frames - frames_written) * frame_size);
                frames_written = num_frames;
                continue;
            }
            // Successfully dequeued a new buffer.
            queued_buffers--;
        }

        // Get the minimum frames available between the currently playing buffer, and the
        // amount we have left to fill
        size_t frames_available{std::min<u64>(playing_buffer.frames - playing_buffer.frames_played, num_frames - frames_written)};

        samples_buffer.Push(&input_buffer[frames_written * frame_size], frames_available * frame_size);

        frames_written += frames_available;
        playing_buffer.frames_played += frames_available;

        // If that's all the frames in the current buffer, add its samples and mark it as
        // consumed
        if (playing_buffer.frames_played >= playing_buffer.frames)
            playing_buffer.consumed = true;
    }

    if (frames_written > 0) {
        std::memcpy(&last_frame[0], &input_buffer[(frames_written - 1) * frame_size], frame_size_bytes);
    }

    // update sample counts fuer audio-ins
    {
        std::scoped_lock lk{sample_count_lock};
        last_sample_count_update_time = system.CoreTiming().GetGlobalTimeNs();
        min_played_sample_count = max_played_sample_count;
        max_played_sample_count += frames_written;
    }
}

void SinkStream::ProcessAudioOutAndRender(std::span<s16> output_buffer, std::size_t num_frames) {
    if (num_frames == 0) {
        return;
    }

    const std::size_t num_channels = GetDeviceChannels();
    const std::size_t frame_size = num_channels;
    const std::size_t frame_size_bytes = frame_size * sizeof(s16);
    size_t frames_written{0};
    size_t actual_frames_written{0};

    if (system.IsPaused() || system.IsShuttingDown()) {
        if (system.IsShuttingDown()) {
            std::scoped_lock lk{release_mutex};
            queued_buffers.store(0);
            release_cv.notify_one();
        }
        for (size_t i = 0; i < num_frames; i++)
            std::memset(&output_buffer[i * frame_size], 0, frame_size_bytes);
        return;
    }

    const u64 callback_us = HostMicroseconds();
    const u64 previous_callback_us = last_callback_us.exchange(callback_us, std::memory_order_relaxed);
    const u64 callback_gap_us = previous_callback_us != 0 && callback_us >= previous_callback_us
                                    ? callback_us - previous_callback_us : 0;
    if (callback_gap_us != 0) {
        RecordPeak(peak_callback_gap_us, callback_gap_us);
        if (callback_gap_us >= 40'000) {
            callback_gaps_over_40ms.fetch_add(1, std::memory_order_relaxed);
        }
    }
    requested_frames.fetch_add(num_frames, std::memory_order_relaxed);
    while (frames_written < num_frames) {
        if (playing_buffer.consumed || playing_buffer.frames == 0) {
            std::unique_lock lk{release_mutex};

            if (!queue.TryPop(playing_buffer)) {
                break;
            }

            --queued_buffers;
            lk.unlock();
            release_cv.notify_one();
        }

        const size_t frames_available = std::min<u64>(playing_buffer.frames - playing_buffer.frames_played, num_frames - frames_written);

        const size_t samples_read = samples_buffer.Pop(
            &output_buffer[frames_written * frame_size], frames_available * frame_size);
        const size_t frames_read = samples_read / frame_size;

        frames_written += frames_read;
        actual_frames_written += frames_read;
        playing_buffer.frames_played += frames_read;

        if (playing_buffer.frames_played >= playing_buffer.frames)
            playing_buffer.consumed = true;

        if (samples_read != frames_available * frame_size) {
            short_reads.fetch_add(1, std::memory_order_relaxed);
            break;
        }
    }

    delivered_frames.fetch_add(actual_frames_written, std::memory_order_relaxed);

    // PCM integrity counters are diagnostic-only for Unicorn Overlord.
    // Scan samples already delivered, excluding synthesized underrun padding.
    if (type == StreamType::Out &&
        system.GetApplicationProcessProgramID() == 0x010069401ADB8000ULL) {
        u64 clipped = 0;
        u64 large_steps = 0;
        for (std::size_t frame = 0; frame < actual_frames_written; ++frame) {
            for (std::size_t channel = 0; channel < num_channels; ++channel) {
                const s32 sample = output_buffer[frame * frame_size + channel];
                const s32 previous = pcm_last_real_frame[channel];
                if (sample >= 32760 || sample <= -32760) {
                    ++clipped;
                }
                if (has_pcm_last_real_frame &&
                    (sample > previous ? sample - previous : previous - sample) > 26000) {
                    ++large_steps;
                }
                pcm_last_real_frame[channel] = static_cast<s16>(sample);
            }
            has_pcm_last_real_frame = true;
        }
        if (clipped > 0) {
            clipped_samples.fetch_add(clipped, std::memory_order_relaxed);
        }
        if (large_steps > 0) {
            large_pcm_steps.fetch_add(large_steps, std::memory_order_relaxed);
        }
    }
    if (frames_written > 0) {
        std::memcpy(last_frame.data(), &output_buffer[(frames_written - 1) * frame_size],
                    frame_size_bytes);
    }
    if (frames_written < num_frames) {
        underrun_callbacks.fetch_add(1, std::memory_order_relaxed);
        underrun_frames.fetch_add(num_frames - frames_written, std::memory_order_relaxed);
        const u64 producer_us = last_append_us.load(std::memory_order_relaxed);
        const u64 producer_age_us = producer_us != 0 && callback_us >= producer_us
                                        ? callback_us - producer_us : 0;
        const u64 seq = underrun_sequence.fetch_add(1, std::memory_order_relaxed) + 1;
        auto& slot = underrun_trace[(seq - 1) % TraceCapacity];
        slot.sequence.store(0, std::memory_order_release);
        slot.host_us.store(callback_us, std::memory_order_relaxed);
        slot.missing_frames.store(num_frames - frames_written, std::memory_order_relaxed);
        slot.callback_gap_us.store(callback_gap_us, std::memory_order_relaxed);
        slot.producer_age_us.store(producer_age_us, std::memory_order_relaxed);
        slot.pending_buffers.store(queued_buffers.load(std::memory_order_relaxed),
                                   std::memory_order_relaxed);
        slot.sequence.store(seq, std::memory_order_release);
        for (size_t i = frames_written; i < num_frames; i++) {
            std::memcpy(&output_buffer[i * frame_size], last_frame.data(), frame_size_bytes);
        }
    }

    {
        std::scoped_lock lk{sample_count_lock};
        last_sample_count_update_time = system.CoreTiming().GetGlobalTimeNs();
        min_played_sample_count = max_played_sample_count;
        max_played_sample_count += actual_frames_written;
    }
}

u64 SinkStream::GetExpectedPlayedSampleCount() {
    std::scoped_lock lk{sample_count_lock};
    auto cur_time{system.CoreTiming().GetGlobalTimeNs()};
    auto time_delta{cur_time - last_sample_count_update_time};
    auto exp_played_sample_count{min_played_sample_count + (TargetSampleRate * time_delta) / std::chrono::seconds{1}};

    // The 125ms Unicorn Overlord experiment did not eliminate audio underruns
    // on Android. Restore the original 25ms lead until the cause is diagnosed.
    return std::min<u64>(exp_played_sample_count, max_played_sample_count) + TargetSampleCount * 5;
}

void SinkStream::WaitFreeSpace(std::stop_token stop_token) {
    std::unique_lock lk{release_mutex};

    auto can_continue = [this]() {
        return paused || queued_buffers < max_queue_size;
    };

    release_cv.wait_for(lk, std::chrono::milliseconds(7), can_continue);

    if (queued_buffers > max_queue_size + 3) {
        release_cv.wait(lk, stop_token, can_continue);
    }
}

void SinkStream::SignalPause() {
    {
        std::scoped_lock lk{release_mutex};
        paused = true;
    }
    release_cv.notify_one();
}

} // namespace AudioCore::Sink
