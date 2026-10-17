#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

"""Exercise production audio queue/callback logic without an Android device.

The real sink methods, guest-memory adapter, device append method, and queue
containers are compiled with small Core/SDL stand-ins. A test queue wrapper pauses
the producer immediately after descriptor publication so a real consumer thread
can exercise that scheduling boundary.
This validates sample delivery, not Android driver timing or audible quality.
"""

import argparse
import os
from pathlib import Path
import re
import subprocess
import tempfile


def without_includes(source):
    return re.sub(r'^\s*#(?:include|pragma once)[^\n]*\n', '', source, flags=re.MULTILINE)


def function(source, marker):
    start = source.index(marker)
    opening = source.index('{', start)
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


PREFIX = r'''
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <semaphore>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>
using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;
using VAddr = uint64_t;
using s16 = int16_t;
using s32 = int32_t;
using s64 = int64_t;
using f32 = float;
namespace Core {
struct TestTiming {
    std::chrono::nanoseconds GetGlobalTimeNs() const { return {}; }
};
struct System {
    bool paused = false;
    bool shutting_down = false;
    TestTiming timing;
    u64 program_id = 0;
    bool IsPaused() const { return paused; }
    bool IsShuttingDown() const { return shutting_down; }
    TestTiming& CoreTiming() { return timing; }
    u64 GetApplicationProcessProgramID() const { return program_id; }
};
}
namespace Settings { float Volume() { return 1.0f; } }
std::function<void()> after_publish;
'''

QUEUE_WRAPPER = r'''
template <typename T, size_t Capacity>
struct ControlledQueue {
    Common::SPSCQueue<T, Capacity> real;
    void EmplaceWait(const T& item) {
        real.EmplaceWait(item);
        if (after_publish) after_publish();
    }
    bool TryPop(T& item) { return real.TryPop(item); }
};
'''

SDL_STUBS = r'''
void Check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
struct SDL_AudioStream {
    std::vector<s16> submitted;
    std::vector<s16> recorded;
    const void* last_source = nullptr;
};
int SDL_GetAudioStreamAvailable(SDL_AudioStream* stream) {
    return static_cast<int>(stream->recorded.size() * sizeof(s16));
}
int SDL_GetAudioStreamData(SDL_AudioStream* stream, void* destination, int bytes) {
    auto count = std::min(stream->recorded.size(), static_cast<size_t>(bytes) / sizeof(s16));
    std::memcpy(destination, stream->recorded.data(), count * sizeof(s16));
    stream->recorded.erase(stream->recorded.begin(), stream->recorded.begin() + count);
    return static_cast<int>(count * sizeof(s16));
}
bool SDL_PutAudioStreamData(SDL_AudioStream* stream, const void* source, int bytes) {
    Check(bytes > 0 && bytes % 4 == 0, "SDL received an incomplete stereo frame");
    auto* samples = static_cast<const s16*>(source);
    stream->submitted.insert(stream->submitted.end(), samples, samples + bytes / sizeof(s16));
    stream->last_source = source;
    return true;
}
using namespace AudioCore;
using namespace AudioCore::Sink;
class SDLSinkStream : public SinkStream {
public:
    using SinkStream::SinkStream;
    std::vector<s16> callback_buffer = std::vector<s16>(TargetSampleCount * MaxChannels * 2);
'''

TESTS = r'''
void GuestSamplesPreserved(u32 channels, f32 volume, const std::vector<s16>& original,
                           const std::vector<s16>& expected) {
    Core::System system;
    auto sink = std::make_unique<SinkStream>(system, StreamType::Out);
    sink->SetSystemChannels(channels);
    sink->SetSystemVolume(volume);
    TestProcess process;
    process.memory.samples = original;
    DeviceSession session{sink.get(), StreamType::Out, static_cast<u16>(channels), &process};
    AudioBuffer buffer{};
    buffer.size = original.size() * sizeof(s16);
    buffer.tag = 1;
    s16* scratch_storage = nullptr;
    for (int repeat = 0; repeat < 2; ++repeat) {
        session.AppendBuffers(std::span{&buffer, 1});
        Check(process.memory.samples == original, "playback conversion modified guest samples");
        if (repeat == 0) scratch_storage = session.tmp_samples.data();
        Check(session.tmp_samples.data() == scratch_storage, "audio scratch storage was reallocated");
        std::vector<s16> output(expected.size());
        sink->ProcessAudioOutAndRender(output, expected.size() / 2);
        Check(output == expected, "reusing a guest buffer changed its playback samples");
    }
}

void Append(SinkStream& sink, std::vector<s16> samples, u32 channels = 2) {
    sink.SetSystemChannels(channels);
    SinkBuffer buffer{samples.size() / channels, 0, 1, false};
    sink.AppendBuffer(buffer, samples);
}

void Handoff(u32 channels, const std::vector<s16>& samples, const std::vector<s16>& expected) {
    Core::System system;
    auto sink = std::make_unique<SinkStream>(system, StreamType::Out);
    std::vector<s16> output(expected.size(), -1234);
    std::binary_semaphore published{0}, consumed{0};
    u32 before = 0, after = 0;
    after_publish = [&] { published.release(); consumed.acquire(); };
    std::thread consumer([&] {
        published.acquire();
        before = sink->GetQueueSize();
        sink->ProcessAudioOutAndRender(output, expected.size() / 2);
        after = sink->GetQueueSize();
        consumed.release();
    });
    Append(*sink, samples, channels);
    consumer.join();
    after_publish = {};
    Check(output == expected, "descriptor became visible before its samples");
    Check(before == 1 && after == 0, "published queue count underflowed");
}

void Underrun() {
    Core::System system;
    auto sink = std::make_unique<SinkStream>(system, StreamType::Out);
    Append(*sink, {10, 20, 30, 40});
    std::vector<s16> output(8, -1234);
    sink->ProcessAudioOutAndRender(output, 4);
    Check(output == std::vector<s16>({10, 20, 30, 40, 30, 40, 30, 40}),
          "underrun did not hold the most recently delivered frame");
    Check(sink->max_played_sample_count == 2, "padding was counted as guest audio");
#if HAS_COUNTERS
    Check(sink->GetUnderrunFrames() == 2 && sink->GetShortReadCount() == 0,
          "incorrect starvation counters");
    sink->ProcessAudioOutAndRender({}, 0);
    Check(sink->GetUnderrunFrames() == 2, "zero-frame callback changed counters");
#endif
}

void ShortRead() {
    Core::System system;
    auto sink = std::make_unique<SinkStream>(system, StreamType::Out);
    std::array<s16, 4> first{10, 20, 30, 40};
    sink->samples_buffer.Push(first);
    ++sink->queued_buffers;
    sink->queue.EmplaceWait(SinkBuffer{4, 0, 1, false});
    std::vector<s16> output(8, -1234);
    sink->ProcessAudioOutAndRender(output, 4);
    Check(output == std::vector<s16>({10, 20, 30, 40, 30, 40, 30, 40}),
          "short read exposed stale output samples");
    Check(sink->max_played_sample_count == 2 && sink->playing_buffer.frames_played == 2,
          "short read advanced missing guest frames");
#if HAS_COUNTERS
    Check(sink->GetShortReadCount() == 1, "short read was not reported");
#endif
    std::array<s16, 4> remaining{50, 60, 70, 80};
    sink->samples_buffer.Push(remaining);
    output.resize(4);
    sink->ProcessAudioOutAndRender(output, 2);
    Check(std::equal(output.begin(), output.end(), remaining.begin()), "late samples lost order");
    Check(sink->max_played_sample_count == 4 && sink->playing_buffer.consumed,
          "late samples did not complete the original buffer");
}

void AudioTelemetryAndDefaultLead() {
    Core::System system;
    system.program_id = 0x010069401ADB8000ULL;
    SinkStream sink(system, StreamType::Out);
    Check(sink.GetExpectedPlayedSampleCount() == TargetSampleCount * 5,
          "Unicorn Overlord experiment must be reverted");
    Append(sink, {10, 20, 30, 40});
    Check(sink.GetAppendedFrames() == 2, "appended frame counter incorrect");
    std::vector<s16> output(8);
    sink.ProcessAudioOutAndRender(output, 4);
    Check(sink.GetRequestedFrames() == 4 && sink.GetDeliveredFrames() == 2,
          "requested/delivered telemetry incorrect");
    Check(sink.GetUnderrunFrames() == 2 && sink.GetUnderrunCallbackCount() == 1,
          "underrun diagnostics incorrect");
    u64 cursor = 0;
    auto events = sink.DrainUnderrunTraces(cursor);
    Check(events.size() == 1 && events[0].missing_frames == 2 &&
              cursor == 1 && events[0].pending_buffers == 0,
          "per-callback trace incorrectly captured");
    Check(sink.DrainUnderrunTraces(cursor).empty(),
          "draining trace twice must not duplicate events");
}

void NoAdditionalData() {
    Core::System system;
    auto sink = std::make_unique<SDLSinkStream>(system, StreamType::Out);
    Append(*sink, {10, 20, 30, 40});
    SDL_AudioStream stream;
    SDLSinkStream::DataCallback(sink.get(), &stream, 0, 1920);
    SDLSinkStream::DataCallback(sink.get(), &stream, -1, 1920);
    Check(stream.submitted.empty() && sink->GetQueueSize() == 1,
          "callback consumed guest audio when SDL requested no additional data");
}

void AlignedRequestAndReuse() {
    Core::System system;
    auto sink = std::make_unique<SDLSinkStream>(system, StreamType::Out);
    Append(*sink, {10, 20, 30, 40, 50, 60});
    SDL_AudioStream stream;
    SDLSinkStream::DataCallback(sink.get(), &stream, 3, 3);
    Check(stream.submitted == std::vector<s16>({10, 20}), "partial request split a frame");
    auto* storage = sink->callback_buffer.data();
    Check(stream.last_source == storage, "callback allocated separate temporary storage");
    SDLSinkStream::DataCallback(sink.get(), &stream, 8, 8);
    Check(stream.submitted == std::vector<s16>({10, 20, 30, 40, 50, 60}), "samples reordered");
    Check(sink->callback_buffer.data() == storage, "ordinary callback reallocated storage");
    std::vector<s16> large(8000, 123);
    Append(*sink, large);
    stream.submitted.clear();
    SDLSinkStream::DataCallback(sink.get(), &stream, 16000, 16000);
    Check(stream.submitted == large, "large callback truncated audio");
    storage = sink->callback_buffer.data();
    Append(*sink, large);
    stream.submitted.clear();
    SDLSinkStream::DataCallback(sink.get(), &stream, 16000, 16000);
    Check(sink->callback_buffer.data() == storage && stream.submitted == large,
          "large callback storage was not reused");
}

void PausedAndInput() {
    Core::System system;
    auto sink = std::make_unique<SDLSinkStream>(system, StreamType::Out);
    Append(*sink, {10, 20});
    system.paused = true;
    SDL_AudioStream output;
    SDLSinkStream::DataCallback(sink.get(), &output, 4, 4);
    Check(output.submitted == std::vector<s16>({0, 0}) && sink->GetQueueSize() == 1,
          "paused callback consumed guest audio");
    system.paused = false;
    auto input = std::make_unique<SDLSinkStream>(system, StreamType::In);
    SDL_AudioStream recording;
    recording.recorded = {10, 20, 30, 40};
    SDLSinkStream::DataCallback(input.get(), &recording, 8, 8);
    Check(input->samples_buffer.Pop(4) == std::vector<s16>({10, 20, 30, 40}),
          "capture path lost input frames");
}

int main() {
    int failures = 0;
    auto run = [&](const char* name, auto test) {
        try { test(); std::printf("PASS: %s\n", name); }
        catch (const std::exception& e) {
            ++failures;
            std::printf("FAIL: %s: %s\n", name, e.what());
        }
    };
    run("stereo handoff", [] { Handoff(2, {10, 20, 30, 40}, {10, 20, 30, 40}); });
    run("surround guest buffer preservation and reuse", [] {
        GuestSamplesPreserved(6, 1.0f, {100, 200, 0, 0, 0, 0, 300, 400, 0, 0, 0, 0},
                              {100, 200, 300, 400});
    });
    run("stereo volume preserves guest buffer", [] {
        GuestSamplesPreserved(2, 0.5f, {100, 200, 300, 400}, {50, 100, 150, 200});
    });
    run("mono handoff", [] { Handoff(1, {10, 20}, {10, 0, 20, 0}); });
    run("surround downmix handoff", [] { Handoff(6, {100, 200, 0, 0, 0, 0}, {100, 200}); });
    run("partial-buffer underrun", Underrun);
    run("short read and recovery", ShortRead);
    run("audio telemetry and default lead", AudioTelemetryAndDefaultLead);
    run("SDL zero additional request", NoAdditionalData);
#if HAS_COUNTERS
    run("SDL frame alignment and storage reuse", AlignedRequestAndReuse);
#endif
    run("paused output and capture", PausedAndInput);
    return failures ? 1 : 0;
}
'''

DEVICE_STUBS = r'''
namespace Core::Memory {
struct Memory {
    static constexpr bool HAS_FLUSH_INVALIDATION = false;
    std::vector<s16> samples;
    u8* GetSpan(u64 address, size_t bytes) {
        Check(address <= samples.size() * sizeof(s16) &&
                  bytes <= samples.size() * sizeof(s16) - address, "guest read out of bounds");
        return reinterpret_cast<u8*>(samples.data()) + address;
    }
    void ReadBlockUnsafe(u64 address, void* destination, size_t bytes) {
        std::memcpy(destination, GetSpan(address, bytes), bytes);
    }
};
template <typename T, GuestMemoryFlags FLAGS>
using CpuGuestMemory = GuestMemory<Memory, T, FLAGS>;
}
struct TestProcess {
    Core::Memory::Memory memory;
    Core::Memory::Memory& GetMemory() { return memory; }
};
namespace AudioCore {
struct DeviceSession {
    Sink::SinkStream* stream;
    Sink::StreamType type;
    u16 channel_count;
    TestProcess* handle;
    Common::ScratchBuffer<s16> tmp_samples{};
    void AppendBuffers(std::span<const AudioBuffer> buffers);
};
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-root', type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument('--sanitize', action='store_true')
    args = parser.parse_args()
    root = args.source_root
    read = lambda path: (root / path).read_text()
    header = without_includes(read('src/audio_core/sink/sink_stream.h'))
    has_counters = 'GetUnderrunFrames' in header
    header = header.replace('private:', 'public:')
    header = header.replace('Common::SPSCQueue<SinkBuffer, 0x10000>',
                            'ControlledQueue<SinkBuffer, 0x10000>')
    common = read('src/audio_core/common/common.h')
    channels = function(common, 'enum class Channels') + ';\n'
    constants = '\n'.join(line for line in common.splitlines()
                          if re.match(r'constexpr u32 (TargetSampleCount|TargetSampleRate|MaxChannels) =', line))
    source = PREFIX + f'\n#define HAS_COUNTERS {int(has_counters)}\n'
    source += 'namespace AudioCore {\n' + channels + constants + '\n}\n'
    source += without_includes(read('src/common/ring_buffer.h'))
    source += without_includes(read('src/common/bounded_threadsafe_queue.h'))
    source += QUEUE_WRAPPER + header
    source += without_includes(read('src/audio_core/sink/sink_stream.cpp'))
    source += SDL_STUBS
    source += function(read('src/audio_core/sink/sdl3_sink.cpp'), 'static void DataCallback')
    source += '\n};\n#define ASSERT(condition) Check(condition, "guest memory assertion")\n'
    source += without_includes(read('src/common/scratch_buffer.h'))
    source += without_includes(read('src/core/guest_memory.h'))
    source += without_includes(read('src/audio_core/device/audio_buffer.h'))
    source += DEVICE_STUBS
    source += 'namespace AudioCore {\n'
    source += function(read('src/audio_core/device/device_session.cpp'),
                       'void DeviceSession::AppendBuffers')
    source += '\n}\n' + TESTS
    with tempfile.TemporaryDirectory(prefix='eden-audio-test-') as tmp:
        tmp = Path(tmp)
        cpp, binary = tmp / 'audio.cpp', tmp / 'audio-test'
        cpp.write_text(source)
        flags = ['-std=c++20', '-O2', '-pthread', '-Wall', '-Wextra', '-Werror',
                 '-Wno-unused-parameter']
        if args.sanitize:
            flags += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
        subprocess.run(['g++', *flags, str(cpp), '-o', str(binary)], check=True)
        env = os.environ.copy()
        if args.sanitize:
            env['ASAN_OPTIONS'] = 'detect_leaks=0'
        return subprocess.run([str(binary)], env=env, timeout=20).returncode


if __name__ == '__main__':
    raise SystemExit(main())
