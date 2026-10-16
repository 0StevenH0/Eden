# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

if (NOT ANDROID OR NOT ARCHITECTURE_arm64)
    message(FATAL_ERROR "The POCO X7 Pro profile requires Android arm64-v8a")
endif()

include(CheckCCompilerFlag)
include(CheckCXXCompilerFlag)

# Keep the Android NEON baseline; do not assume the kernel exposes SVE or SME.
set(poco_cpu_flags "-march=armv8.2-a+fp16+dotprod+lse+rcpc -mtune=cortex-a725")
check_c_compiler_flag("${poco_cpu_flags}" POCO_C_FLAGS_SUPPORTED)
check_cxx_compiler_flag("${poco_cpu_flags}" POCO_CXX_FLAGS_SUPPORTED)
if (NOT POCO_C_FLAGS_SUPPORTED OR NOT POCO_CXX_FLAGS_SUPPORTED)
    message(FATAL_ERROR "The POCO X7 Pro profile requires a compiler with Cortex-A725 support (NDK r28c)")
endif()

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${poco_cpu_flags}")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${poco_cpu_flags}")
add_compile_definitions(EDEN_POCOPHONE_X7_PRO=1)

# Upstream updates would replace this device-specific build with a generic APK.
set(ENABLE_UPDATE_CHECKER OFF CACHE BOOL "Enable update checker (for Qt and Android)" FORCE)
message(STATUS "POCO X7 Pro profile: Cortex-A725 tuning, system Mali Vulkan driver")
