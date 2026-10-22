// SPDX-FileCopyrightText: Copyright 2025 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2022 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/core.h"
#include "core/core_timing.h"
#include "core/hle/kernel/k_hardware_timer.h"
#include "core/hle/kernel/k_scheduler.h"

#ifdef EDEN_POCOPHONE_X7_PRO
#include "common/logging/log.h"
#include "common/stall_timing.h"
#endif

namespace Kernel {

void KHardwareTimer::Initialize() {
#ifdef EDEN_POCOPHONE_X7_PRO
    m_last_stall_report_us = 0;
#endif
    // Create the timing callback to register with CoreTiming.
    m_event_type = Core::Timing::CreateEvent("KHardwareTimer::Callback",
                                             [this](s64, std::chrono::nanoseconds) {
                                                 this->DoTask();
                                                 return std::nullopt;
                                             });
}

void KHardwareTimer::Finalize() {
    m_kernel.System().CoreTiming().UnscheduleEvent(m_event_type);
    m_wakeup_time = (std::numeric_limits<s64>::max)();
    m_event_type.reset();
}

void KHardwareTimer::DoTask() {
#ifdef EDEN_POCOPHONE_X7_PRO
    Common::StallTiming<4> timing;
#endif
    // Handle the interrupt.
    {
        KScopedSchedulerLock slk{m_kernel};
#ifdef EDEN_POCOPHONE_X7_PRO
        timing.Record(1);
#endif
        {
            KScopedSpinLock lk(this->GetLock());
#ifdef EDEN_POCOPHONE_X7_PRO
            timing.Record(2);
#endif

            if (this->GetInterruptEnabled()) {
                // CoreTiming has already popped this event; no unschedule is needed here.
                m_wakeup_time = (std::numeric_limits<s64>::max)();

                if (const s64 next_time = this->DoInterruptTaskImpl(GetTick());
                    0 < next_time && next_time <= m_wakeup_time) {
                    this->EnableInterrupt(next_time);
                }
            }
        }
#ifdef EDEN_POCOPHONE_X7_PRO
        timing.Record(3);
#endif
    }
#ifdef EDEN_POCOPHONE_X7_PRO
    timing.Record(4);
    if (timing.ShouldReport(m_last_stall_report_us)) {
        LOG_INFO(Kernel,
                 "Kernel timer stall: host_us={} scheduler_lock_us={} timer_lock_us={} "
                 "tasks_us={} reschedule_us={} total_us={}",
                 timing.StartUs(), timing.StageUs(0), timing.StageUs(1), timing.StageUs(2),
                 timing.StageUs(3), timing.TotalUs());
    }
#endif

    // Clear the timer interrupt.
    // Kernel::GetInterruptManager().ClearInterrupt(KInterruptName_NonSecurePhysicalTimer,
    //                                              GetCurrentCoreId());
}

void KHardwareTimer::EnableInterrupt(s64 wakeup_time) {
    this->DisableInterrupt();

    m_wakeup_time = wakeup_time;
    m_kernel.System().CoreTiming().ScheduleEvent(std::chrono::nanoseconds{m_wakeup_time},
                                                 m_event_type, true);
}

void KHardwareTimer::DisableInterrupt() {
    m_kernel.System().CoreTiming().UnscheduleEvent(m_event_type,
                                                   Core::Timing::UnscheduleEventType::NoWait);
    m_wakeup_time = (std::numeric_limits<s64>::max)();
}

s64 KHardwareTimer::GetTick() const {
    return m_kernel.System().CoreTiming().GetGlobalTimeNs().count();
}

bool KHardwareTimer::GetInterruptEnabled() {
    return m_wakeup_time != (std::numeric_limits<s64>::max)();
}

} // namespace Kernel
