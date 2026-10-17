#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

"""Exercise production NV event wait/cancel accounting with a controlled GPU.

Compile the actual ioctl methods with small event, syncpoint, and system stand-ins.
This checks when the application-wide fallback is entered and preserves its fence
wait. It does not model Android scheduling, Vulkan performance, or ioctl packing.
"""

import argparse
import os
from pathlib import Path
import subprocess
import tempfile


def function(source, marker):
    start = source.index(marker)
    opening = source.index('{', start)
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


STUBS = r'''
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <utility>
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;
using s32 = int32_t;

template <typename... Args> void Log(const char*, Args&&...) {}
#define LOG_DEBUG(category, ...) Log(__VA_ARGS__)
#define LOG_INFO(category, ...) Log(__VA_ARGS__)
#define LOG_WARNING(category, ...) Log(__VA_ARGS__)
template <typename F> struct ScopeExit {
    F callback;
    ~ScopeExit() { callback(); }
};
struct MakeScopeExit {
    template <typename F> auto operator+(F&& f) {
        return ScopeExit<F>{std::forward<F>(f)};
    }
};
#define SCOPE_EXIT const auto scope_exit = MakeScopeExit{} + [&]()
void Check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

namespace Service::Nvidia::Devices {
constexpr u32 MaxSyncPoints = 192;
constexpr u32 MaxNvEvents = 4;
enum class NvResult { Success, Timeout, BadParameter };
enum class EventState { Available, Waiting, Cancelling, Signalling, Signalled, Cancelled };
struct NvFence { s32 id; u32 value; };
struct Field { void Assign(u32) {} };
struct EventValue {
    u32 raw{};
    u32 slot{};
    Field syncpoint_id, syncpoint_id_for_allocation, event_allocated;
};
struct TestEvent {
    bool signalled{};
    void Signal(int) { signalled = true; }
    void Clear(int) { signalled = false; }
};
struct TestHostSyncpoints {
    u32 completed{};
    u32 waits{};
    u32 waited_id{};
    u32 waited_value{};
    std::function<void()> action;
    u32 RegisterHostAction(u32, u32, std::function<void()> callback) {
        action = std::move(callback);
        return 1;
    }
    void DeregisterHostAction(u32, u32) { action = {}; }
    void Complete(u32 value) {
        completed = value;
        if (auto callback = std::exchange(action, {})) callback();
    }
    void WaitHost(u32 id, u32 value) {
        ++waits;
        waited_id = id;
        waited_value = value;
        Complete(value);
    }
};
struct TestHost1x {
    TestHostSyncpoints syncpoints;
    auto& GetSyncpointManager() { return syncpoints; }
};
struct TestSystem {
    TestHost1x host1x;
    std::mutex suspend_mutex;
    u32 pauses{}, resumes{};
    bool paused{};
    auto& Host1x() { return host1x; }
    int Kernel() { return 0; }
    auto StallApplication() {
        ++pauses;
        paused = true;
        return std::unique_lock{suspend_mutex};
    }
    void UnstallApplication() { ++resumes; paused = false; }
};
struct TestSyncpoints {
    TestHostSyncpoints& host;
    u32 minimum{};
    bool IsSyncpointAllocated(s32) { return true; }
    bool IsFenceSignalled(NvFence fence) { return minimum >= fence.value; }
    u32 ReadSyncpointMinValue(u32) { return minimum; }
    u32 UpdateMin(u32) { return minimum = host.completed; }
};
struct nvhost_ctrl {
    struct InternalEvent {
        TestEvent* kevent{};
        std::atomic<EventState> status{EventState::Available};
        u32 fails{}, assigned_syncpt{}, assigned_value{}, wait_handle{};
        bool registered{true};
        bool IsBeingUsed() const {
            auto state = status.load(std::memory_order_acquire);
            return state == EventState::Waiting || state == EventState::Cancelling ||
                   state == EventState::Signalling;
        }
    };
    struct IocCtrlEventWaitParams { NvFence fence; u32 timeout; EventValue value; };
    struct IocCtrlEventClearParams { EventValue event_id; };
    TestSystem system;
    TestSyncpoints syncpoint_manager{system.host1x.syncpoints};
    std::array<TestEvent, MaxNvEvents> kernel_events;
    std::array<InternalEvent, MaxNvEvents> events;
    std::mutex events_mutex;
    nvhost_ctrl() {
        for (u32 i = 0; i < MaxNvEvents; ++i) events[i].kevent = &kernel_events[i];
    }
    auto NvEventsLock() { return std::unique_lock{events_mutex}; }
    u32 FindFreeNvEvent(u32) { return 0; }
    NvResult IocCtrlEventWait(IocCtrlEventWaitParams& params, bool is_allocation);
    NvResult IocCtrlClearEventWait(IocCtrlEventClearParams& params);
    NvResult Wait(u32 value, bool allocation = false, u32 timeout = 100) {
        IocCtrlEventWaitParams params{{1, value}, timeout, {}};
        return IocCtrlEventWait(params, allocation);
    }
    void Clear() {
        IocCtrlEventClearParams params{};
        Check(IocCtrlClearEventWait(params) == NvResult::Success, "clear failed");
    }
    void CancelPending(u32 value, bool allocation = false) {
        Check(Wait(value, allocation) == NvResult::Timeout, "wait did not arm");
        Clear();
    }
};
'''

TESTS = r'''
void AsyncCompletionBreaksFailureHistory(bool allocation) {
    nvhost_ctrl device;
    device.CancelPending(1, allocation);
    device.CancelPending(1, allocation);
    Check(device.Wait(1, allocation) == NvResult::Timeout, "third wait did not arm");
    device.system.host1x.syncpoints.Complete(1);
    Check(device.kernel_events[0].signalled, "completion did not signal guest");
    device.CancelPending(2, allocation);
    Check(device.Wait(2, allocation) == NvResult::Timeout,
          "successful async completion left stale failures and paused the application");
    Check(device.system.pauses == 0, "application paused after nonconsecutive failures");
}
void CompletedClearsAreNotFailures() {
    nvhost_ctrl device;
    for (u32 value = 1; value <= 3; ++value) {
        Check(device.Wait(value) == NvResult::Timeout, "wait did not arm");
        device.system.host1x.syncpoints.Complete(value);
        device.Clear();
        Check(!device.kernel_events[0].signalled, "clear left event signalled");
    }
    Check(device.Wait(4) == NvResult::Timeout,
          "clearing completed events incorrectly triggered the application-wide fallback");
    Check(device.system.pauses == 0, "completed events counted as failures");
}
void ConsecutiveFailuresKeepFallback() {
    nvhost_ctrl device;
    for (int i = 0; i < 3; ++i) device.CancelPending(7);
    Check(device.Wait(7) == NvResult::Success, "fallback did not complete");
    const auto& host = device.system.host1x.syncpoints;
    Check(host.waits == 1 && host.waited_id == 1 && host.waited_value == 7,
          "fallback bypassed the requested GPU fence");
    Check(device.system.pauses == 1 && device.system.resumes == 1 && !device.system.paused,
          "fallback did not preserve pause/resume");
    Check(device.Wait(8) == NvResult::Timeout, "fallback did not reset failures");
}
void ReadyFenceKeepsFastPath() {
    nvhost_ctrl device;
    device.CancelPending(1);
    device.CancelPending(1);
    device.system.host1x.syncpoints.Complete(1);
    Check(device.Wait(1) == NvResult::Success, "ready fence lost success fast path");
    device.CancelPending(2);
    device.CancelPending(2);
    Check(device.Wait(2) == NvResult::Timeout, "ready fence failed to reset history");
    Check(device.system.pauses == 0, "ready-fence fast path caused an application pause");
}
void IdleClearsAreNotFailures() {
    nvhost_ctrl device;
    for (int i = 0; i < 3; ++i) device.Clear();
    Check(device.Wait(1) == NvResult::Timeout, "idle clears triggered fallback");
    Check(device.system.pauses == 0, "idle clears counted as failed waits");
}
void ZeroTimeoutKeepsFallback() {
    nvhost_ctrl device;
    for (int i = 0; i < 3; ++i) device.CancelPending(1);
    Check(device.Wait(1, false, 0) == NvResult::Success, "zero-timeout fallback was removed");
    Check(device.system.host1x.syncpoints.waits == 1, "zero-timeout fallback skipped fence");
}
} // namespace Service::Nvidia::Devices

int main() {
    using namespace Service::Nvidia::Devices;
    int failures = 0;
    const auto run = [&](const char* name, auto test) {
        try { test(); std::printf("PASS: %s\n", name); }
        catch (const std::exception& e) {
            ++failures;
            std::printf("FAIL: %s: %s\n", name, e.what());
        }
    };
    run("registered async completion resets history", [] { AsyncCompletionBreaksFailureHistory(false); });
    run("allocated async completion resets history", [] { AsyncCompletionBreaksFailureHistory(true); });
    run("completed event clears", CompletedClearsAreNotFailures);
    run("consecutive failures preserve fallback and fence", ConsecutiveFailuresKeepFallback);
    run("ready-fence success fast path", ReadyFenceKeepsFastPath);
    run("idle event clears", IdleClearsAreNotFailures);
    run("zero-timeout fallback", ZeroTimeoutKeepsFallback);
    return failures ? 1 : 0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-root', type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument('--sanitize', action='store_true')
    args = parser.parse_args()
    source = (args.source_root / 'src/core/hle/service/nvdrv/devices/nvhost_ctrl.cpp').read_text()
    methods = '\n'.join(function(source, 'NvResult nvhost_ctrl::' + name)
                        for name in ['IocCtrlEventWait(', 'IocCtrlClearEventWait('])
    with tempfile.TemporaryDirectory(prefix='eden-nvhost-test-') as tmp:
        cpp, binary = Path(tmp) / 'nvhost.cpp', Path(tmp) / 'nvhost-test'
        cpp.write_text(STUBS + methods + TESTS)
        flags = ['-std=c++20', '-O2', '-pthread', '-Wall', '-Wextra', '-Werror',
                 '-DEDEN_POCOPHONE_X7_PRO=1']
        if args.sanitize:
            flags += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
        subprocess.run(['g++', *flags, str(cpp), '-o', str(binary)], check=True)
        env = os.environ.copy()
        if args.sanitize:
            env['ASAN_OPTIONS'] = 'detect_leaks=0'
        return subprocess.run([str(binary)], env=env, timeout=20).returncode


if __name__ == '__main__':
    raise SystemExit(main())
