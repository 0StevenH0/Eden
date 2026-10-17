// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2022 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "audio_core/audio_core.h"
#include "audio_core/audio_manager.h"
#include "audio_core/device/audio_buffer.h"
#include "audio_core/device/device_session.h"
#include "audio_core/sink/sink_stream.h"
#include "common/logging.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/guest_memory.h"
#include "core/memory.h"

#include "core/hle/kernel/k_process.h"

namespace AudioCore {

using namespace std::literals;
constexpr auto INCREMENT_TIME{5ms};

DeviceSession::DeviceSession(Core::System& system_)
    : system{system_}, thread_event{Core::Timing::CreateEvent(
                           "AudioOutSampleTick",
                           [this](s64 time, std::chrono::nanoseconds) { return ThreadFunc(); })} {}

DeviceSession::~DeviceSession() {
    Finalize();
}

Result DeviceSession::Initialize(std::string_view name_, SampleFormat sample_format_,
                                 u16 channel_count_, size_t session_id_, Kernel::KProcess* handle_,
                                 u64 applet_resource_user_id_, Sink::StreamType type_) {
    if (stream) {
        Finalize();
    }
    name = fmt::format("{}-{}", name_, session_id_);
    type = type_;
    sample_format = sample_format_;
    channel_count = channel_count_;
    session_id = session_id_;
    handle = handle_;
    handle->Open(system.Kernel());
    applet_resource_user_id = applet_resource_user_id_;

    if (type == Sink::StreamType::In) {
        sink = &system.AudioCore().GetInputSink();
    } else {
        sink = &system.AudioCore().GetOutputSink();
    }
    stream = sink->AcquireSinkStream(system, channel_count, name, type);
    initialized = true;
    return ResultSuccess;
}

void DeviceSession::Finalize() {
    if (initialized) {
        Stop();
        sink->CloseStream(stream);
        stream = nullptr;
    }

    if (handle) {
        handle->Close(system.Kernel());
        handle = nullptr;
    }
}

void DeviceSession::Start() {
    if (stream) {
        if (type == Sink::StreamType::Out) {
            audio_diag_last_time = std::chrono::steady_clock::now();
            audio_diag_next_time = audio_diag_last_time + std::chrono::seconds(1);
            audio_diag_last_appended = stream->GetAppendedFrames();
            audio_diag_last_requested = stream->GetRequestedFrames();
            audio_diag_last_delivered = stream->GetDeliveredFrames();
            audio_diag_last_underruns = stream->GetUnderrunFrames();
            audio_diag_last_underrun_callbacks = stream->GetUnderrunCallbackCount();
            audio_diag_last_short_reads = stream->GetShortReadCount();
        }
        stream->Start();
        system.CoreTiming().ScheduleLoopingEvent(std::chrono::nanoseconds::zero(), INCREMENT_TIME,
                                                 thread_event);
    }
}

void DeviceSession::Stop() {
    if (stream) {
        stream->Stop();
        system.CoreTiming().UnscheduleEvent(thread_event, {});
    }
}

void DeviceSession::ClearBuffers() {
    if (stream) {
        stream->ClearQueue();
    }
}

void DeviceSession::AppendBuffers(std::span<const AudioBuffer> buffers) {
    for (const auto& buffer : buffers) {
        Sink::SinkBuffer new_buffer{
            .frames = buffer.size / (channel_count * sizeof(s16)),
            .frames_played = 0,
            .tag = buffer.tag,
            .consumed = false,
        };

        tmp_samples.resize_destructive(buffer.size / sizeof(s16));
        if (type == Sink::StreamType::In) {
            stream->AppendBuffer(new_buffer, tmp_samples);
        } else {
            Core::Memory::CpuGuestMemory<s16, Core::Memory::GuestMemoryFlags::UnsafeRead> samples(
                handle->GetMemory(), buffer.samples, buffer.size / sizeof(s16));
            stream->AppendBuffer(new_buffer, samples);
        }
    }
}

void DeviceSession::ReleaseBuffer(const AudioBuffer& buffer) const {
    if (type == Sink::StreamType::In) {
        auto samples{stream->ReleaseBuffer(buffer.size / sizeof(s16))};
        handle->GetMemory().WriteBlockUnsafe(buffer.samples, samples.data(), buffer.size);
    }
}

bool DeviceSession::IsBufferConsumed(const AudioBuffer& buffer) const {
    return played_sample_count >= buffer.end_timestamp;
}

void DeviceSession::SetVolume(f32 volume) const {
    if (stream) {
        stream->SetSystemVolume(volume);
    }
}

u64 DeviceSession::GetPlayedSampleCount() const {
    return played_sample_count;
}

std::optional<std::chrono::nanoseconds> DeviceSession::ThreadFunc() {
    played_sample_count = stream->GetExpectedPlayedSampleCount();
    if (type == Sink::StreamType::Out) {
        // Host-second snapshots, taken off the real-time audio callback.
        const auto now = std::chrono::steady_clock::now();
        if (now >= audio_diag_next_time) {
            const auto interval_ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(now - audio_diag_last_time).count();
            const u64 appended = stream->GetAppendedFrames();
            const u64 requested = stream->GetRequestedFrames();
            const u64 delivered = stream->GetDeliveredFrames();
            const u64 underruns = stream->GetUnderrunFrames();
            const u64 underrun_callbacks = stream->GetUnderrunCallbackCount();
            const u64 short_reads = stream->GetShortReadCount();
            LOG_INFO(Audio_Sink,
                     "AudioOut rates: interval_ms={} appended={} requested={} delivered={} underrun_frames={} underrun_callbacks={} short_reads={} queued_buffers={}",
                     interval_ms, appended - audio_diag_last_appended,
                     requested - audio_diag_last_requested,
                     delivered - audio_diag_last_delivered,
                     underruns - audio_diag_last_underruns,
                     underrun_callbacks - audio_diag_last_underrun_callbacks,
                     short_reads - audio_diag_last_short_reads, stream->GetQueueSize());
            audio_diag_last_time = now;
            audio_diag_next_time = now + std::chrono::seconds(1);
            audio_diag_last_appended = appended;
            audio_diag_last_requested = requested;
            audio_diag_last_delivered = delivered;
            audio_diag_last_underruns = underruns;
            audio_diag_last_underrun_callbacks = underrun_callbacks;
            audio_diag_last_short_reads = short_reads;
        }
        system.AudioCore().GetAudioManager().SetEvent(Event::Type::AudioOutManager, true);
    } else {
        system.AudioCore().GetAudioManager().SetEvent(Event::Type::AudioInManager, true);
    }
    return std::nullopt;
}

void DeviceSession::SetRingSize(u32 ring_size) {
    stream->SetRingSize(ring_size);
}

} // namespace AudioCore
