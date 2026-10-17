// SPDX-FileCopyrightText: Copyright 2022 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <string_view>

#include "audio_core/audio_out_manager.h"
#include "audio_core/out/audio_out.h"
#include "common/logging.h"
#include "core/core.h"
#include "core/hle/kernel/k_event.h"

namespace AudioCore::AudioOut {

namespace {
using AudioClock = std::chrono::steady_clock;

class AudioOutTiming {
public:
    AudioOutTiming(bool enabled_, std::string_view operation_)
        : enabled{enabled_}, operation{operation_} {
        if (enabled) {
            started = AudioClock::now();
        }
    }

    void Locked(AudioClock::time_point& previous_call) {
        if (!enabled) {
            return;
        }
        acquired = AudioClock::now();
        if (previous_call != AudioClock::time_point{} && started >= previous_call) {
            gap_us = Microseconds(started - previous_call);
        }
        if (started > previous_call) {
            previous_call = started;
        }
    }

    void Done(AudioClock::time_point& previous_report) {
        if (!enabled) {
            return;
        }
        const auto finished = AudioClock::now();
        wait_us = Microseconds(acquired - started);
        work_us = Microseconds(finished - acquired);
        report = (gap_us >= 40'000 || wait_us >= 20'000 || work_us >= 20'000) &&
                 (previous_report == AudioClock::time_point{} ||
                  finished - previous_report >= std::chrono::milliseconds(250));
        if (report) {
            previous_report = finished;
        }
    }

    ~AudioOutTiming() {
        if (report) {
            LOG_INFO(Audio_Sink,
                     "AudioOut upstream: operation={} host_us={} call_gap_us={} lock_wait_us={} work_us={}",
                     operation, Microseconds(started.time_since_epoch()), gap_us, wait_us, work_us);
        }
    }

private:
    static s64 Microseconds(AudioClock::duration duration) {
        return std::chrono::duration_cast<std::chrono::microseconds>(duration).count();
    }

    bool enabled{};
    bool report{};
    std::string_view operation;
    AudioClock::time_point started{};
    AudioClock::time_point acquired{};
    s64 gap_us{};
    s64 wait_us{};
    s64 work_us{};
};
} // namespace

Out::Out(Core::System& system_, Manager& manager_, Kernel::KEvent* event_, size_t session_id_)
    : manager{manager_}, parent_mutex{manager.mutex}, event{event_}, system{system_, event,
                                                                            session_id_},
      audio_diagnostics{system_.GetApplicationProcessProgramID() == 0x010069401ADB8000ULL} {}

void Out::Free() {
    std::scoped_lock l{parent_mutex};
    manager.ReleaseSessionId(system.GetSessionId());
}

System& Out::GetSystem() {
    return system;
}

AudioOut::State Out::GetState() {
    std::scoped_lock l{parent_mutex};
    return system.GetState();
}

Result Out::StartSystem() {
    std::scoped_lock l{parent_mutex};
    return system.Start();
}

void Out::StartSession() {
    std::scoped_lock l{parent_mutex};
    system.StartSession();
}

Result Out::StopSystem() {
    std::scoped_lock l{parent_mutex};
    return system.Stop();
}

Result Out::AppendBuffer(const AudioOutBuffer& buffer, const u64 tag) {
    AudioOutTiming timing{audio_diagnostics, "append"};
    std::scoped_lock l{parent_mutex};
    timing.Locked(audio_diag_last_call[0]);
    const bool appended = system.AppendBuffer(buffer, tag);
    timing.Done(audio_diag_last_report[0]);
    return appended ? ResultSuccess : Service::Audio::ResultBufferCountReached;
}

void Out::ReleaseAndRegisterBuffers() {
    AudioOutTiming timing{audio_diagnostics, "manager"};
    std::scoped_lock l{parent_mutex};
    timing.Locked(audio_diag_last_call[1]);
    if (system.GetState() == State::Started) {
        system.ReleaseBuffers();
        system.RegisterBuffers();
    }
    timing.Done(audio_diag_last_report[1]);
}

bool Out::FlushAudioOutBuffers() {
    std::scoped_lock l{parent_mutex};
    return system.FlushAudioOutBuffers();
}

u32 Out::GetReleasedBuffers(std::span<u64> tags) {
    AudioOutTiming timing{audio_diagnostics, "collect"};
    std::scoped_lock l{parent_mutex};
    timing.Locked(audio_diag_last_call[2]);
    const u32 released = system.GetReleasedBuffers(tags);
    timing.Done(audio_diag_last_report[2]);
    return released;
}

Kernel::KReadableEvent& Out::GetBufferEvent() {
    std::scoped_lock l{parent_mutex};
    return event->GetReadableEvent();
}

f32 Out::GetVolume() const {
    std::scoped_lock l{parent_mutex};
    return system.GetVolume();
}

void Out::SetVolume(const f32 volume) {
    std::scoped_lock l{parent_mutex};
    system.SetVolume(volume);
}

bool Out::ContainsAudioBuffer(const u64 tag) const {
    std::scoped_lock l{parent_mutex};
    return system.ContainsAudioBuffer(tag);
}

u32 Out::GetBufferCount() const {
    std::scoped_lock l{parent_mutex};
    return system.GetBufferCount();
}

u64 Out::GetPlayedSampleCount() const {
    std::scoped_lock l{parent_mutex};
    return system.GetPlayedSampleCount();
}

} // namespace AudioCore::AudioOut
