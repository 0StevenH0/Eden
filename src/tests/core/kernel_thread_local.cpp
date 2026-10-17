// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <cstddef>
#include <memory>
#include <semaphore>
#include <thread>

#include <catch2/catch_test_macros.hpp>

#include "common/fiber.h"
#include "core/core.h"
#include "core/hle/kernel/kernel.h"

TEST_CASE("Kernel TLS follows a migrating guest fiber", "[core][kernel][fiber]") {
    Core::System system;
    auto& kernel = system.Kernel();
    kernel.SetMulticore(true);

    std::array<std::shared_ptr<Common::Fiber>, 2> hosts;
    std::shared_ptr<Common::Fiber> guest;
    std::binary_semaphore suspended{0};
    std::binary_semaphore finished{0};
    std::array<std::size_t, 2> host_ids{};
    std::array<std::size_t, 2> core_ids{};

    guest = std::make_shared<Common::Fiber>([&] {
        host_ids[0] = kernel.GetCurrentHostThreadID();
        core_ids[0] = kernel.CurrentPhysicalCoreIndex();
        Common::Fiber::YieldTo(guest, *hosts[0]);

        host_ids[1] = kernel.GetCurrentHostThreadID();
        core_ids[1] = kernel.CurrentPhysicalCoreIndex();
        Common::Fiber::YieldTo(guest, *hosts[1]);
    });

    std::thread first{[&] {
        kernel.RegisterCoreThread(0);
        hosts[0] = Common::Fiber::ThreadToFiber();
        Common::Fiber::YieldTo(hosts[0], *guest);
        suspended.release();

        // Keep the original host's TLS alive until the migrated fiber has finished.
        finished.acquire();
        hosts[0]->Exit();
    }};
    std::thread second{[&] {
        kernel.RegisterCoreThread(1);
        hosts[1] = Common::Fiber::ThreadToFiber();
        suspended.acquire();
        Common::Fiber::YieldTo(hosts[1], *guest);
        hosts[1]->Exit();
        finished.release();
    }};
    first.join();
    second.join();

    REQUIRE(host_ids[0] == 0);
    REQUIRE(host_ids[1] == 1);
    REQUIRE(core_ids[0] == 0);
    REQUIRE(core_ids[1] == 1);
}
