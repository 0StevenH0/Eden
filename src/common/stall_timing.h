// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>

namespace Common {

// Fixed-size stage timestamps for the POCO stall diagnostics. Call Record in order,
// then report only after releasing the measured locks.
template <std::size_t Stages>
class StallTiming {
public:
    explicit StallTiming(std::int64_t started_us = NowUs()) {
        timestamps[0] = started_us;
    }

    static std::int64_t NowUs() {
        return std::chrono::duration_cast<std::chrono::microseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }

    void Record(std::size_t stage) {
        timestamps[stage] = NowUs();
    }

    std::int64_t StartUs() const {
        return timestamps[0];
    }

    std::int64_t StageUs(std::size_t stage) const {
        return timestamps[stage + 1] - timestamps[stage];
    }

    std::int64_t TotalUs() const {
        return timestamps[Stages] - timestamps[0];
    }

    // The caller owns this limiter on the same thread that records the stages.
    bool ShouldReport(std::int64_t& last_report_us) const {
        if (TotalUs() < 20'000 ||
            (last_report_us != 0 && timestamps[Stages] - last_report_us < 250'000)) {
            return false;
        }
        last_report_us = timestamps[Stages];
        return true;
    }

private:
    std::array<std::int64_t, Stages + 1> timestamps{};
};

} // namespace Common
