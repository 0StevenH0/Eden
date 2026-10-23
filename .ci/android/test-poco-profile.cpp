// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstdio>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>
#include "poco_performance_profile.h"

namespace Settings {
Values values;
}
namespace AndroidSettings {
Values values;
}

// The production generic INI read/write methods are inserted by the runner. Only
// their storage backend is replaced, so default flags and inheritance are exercised.
class Config {
public:
    explicit Config(bool global_) : global{global_} {}
    void ReadSettingGeneric(Settings::BasicSetting* setting);
    void WriteSettingGeneric(const Settings::BasicSetting* setting);

private:
    static std::string AdjustKey(const std::string& key) { return key; }
    bool ReadBooleanSetting(const std::string& key, std::optional<bool> fallback) {
        return ini.contains(key) ? ini.at(key) == "true" : fallback.value_or(false);
    }
    std::string ReadStringSetting(const std::string& key, const std::string& fallback) {
        return ini.contains(key) ? ini.at(key) : fallback;
    }
    void WriteBooleanSetting(const std::string& key, bool value) {
        ini[key] = value ? "true" : "false";
    }
    void WriteStringSetting(const std::string& key, const std::string& value) { ini[key] = value; }
    bool global;
    std::map<std::string, std::string> ini;
};

// CONFIG_METHODS

static std::vector<Settings::BasicSetting*> AllSettings() {
    std::vector<Settings::BasicSetting*> result;
    for (const auto* linkage : {&Settings::values.linkage, &AndroidSettings::values.linkage}) {
        for (const auto& [key, setting] : linkage->by_key) {
            result.push_back(setting);
        }
    }
    return result;
}

static void Check(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

static std::vector<std::string> GlobalValues() {
    std::vector<std::string> values;
    for (const auto* setting : AllSettings()) {
        values.push_back(setting->ToStringGlobal());
    }
    return values;
}

static void Reset() {
    for (auto* setting : AllSettings()) {
        setting->SetGlobal(true);
        setting->LoadString("");
    }
}

static void TestGlobalProfile() {
    auto& v = Settings::values;
    v.resolution_setup.SetValue(Settings::ResolutionSetup::Res1_4X);
    v.volume.SetValue(37);
    v.audio_output_device_id.SetValue("headphones");
    v.post_shader_chain.SetValue("saved-effect");
    v.players.GetValue()[0].profile_name = "controller-test";
    PocoX7Pro::ApplyPerformanceProfile(true);
    Check(v.resolution_setup.GetValue() == Settings::ResolutionSetup::Res1_2X,
          "profile must raise a saved 0.25x resolution to 0.5x");
    Check(v.cpu_backend.GetValue() == Settings::CpuBackend::Nce, "NCE disabled");
    Check(v.gpu_accuracy.GetValue() == Settings::GpuAccuracy::High &&
              v.use_reactive_flushing.GetValue(), "readback correctness disabled");
    Check(v.gpu_clock.GetValue() == Settings::GpuClock::Normal, "guest GPU boost retained");
    Check(!v.frame_gen.GetValue() && !v.post_shader_enabled.GetValue(), "extra effects enabled");
    Check(v.post_shader_chain.GetValue() == "saved-effect", "saved effect chain deleted");
    Check(v.volume.GetValue() == 37 && v.audio_output_device_id.GetValue() == "headphones",
          "audio preferences changed");
    Check(v.players.GetValue()[0].profile_name == "controller-test", "controller profile changed");
}

static void TestGameIsolation() {
    const auto original_globals = GlobalValues();
    PocoX7Pro::ApplyPerformanceProfile(false);
    Check(GlobalValues() == original_globals, "per-game profile modified global values");
    Check(!Settings::values.resolution_setup.UsingGlobal(), "game still inherits resolution");
    Check(Settings::values.resolution_setup.GetValue() == Settings::ResolutionSetup::Res1_2X,
          "game resolution not applied");
    Check(!AndroidSettings::values.pipeline_worker_count.UsingGlobal(), "workers still inherit");
    Check(Settings::values.volume.UsingGlobal(), "unrelated inheritance changed");
}

static void TestGlobalPreservesGame() {
    auto& resolution = Settings::values.resolution_setup;
    resolution.SetGlobal(false);
    resolution.SetValue(Settings::ResolutionSetup::Res3_4X);
    PocoX7Pro::ApplyPerformanceProfile(true);
    Check(!resolution.UsingGlobal() && resolution.GetValue() == Settings::ResolutionSetup::Res3_4X,
          "global apply replaced a selected game override");
    Check(resolution.GetValue(true) == Settings::ResolutionSetup::Res1_2X,
          "global profile did not update global resolution");
}

static void TestRoundTrip(bool global) {
    const auto original_globals = GlobalValues();
    PocoX7Pro::ApplyPerformanceProfile(global);
    Config config{global};
    for (const auto* setting : AllSettings()) {
        config.WriteSettingGeneric(setting);
    }
    Settings::values.resolution_setup.SetValue(Settings::ResolutionSetup::Res1_4X);
    Settings::values.gpu_clock.SetValue(Settings::GpuClock::Overclock);
    AndroidSettings::values.pipeline_worker_count.SetValue(8);
    for (auto* setting : AllSettings()) {
        config.ReadSettingGeneric(setting);
    }
    Check(Settings::values.resolution_setup.GetValue() == Settings::ResolutionSetup::Res1_2X,
          "resolution lost on reload");
    Check(Settings::values.gpu_clock.GetValue() == Settings::GpuClock::Normal,
          "clock lost on reload");
    Check(AndroidSettings::values.pipeline_worker_count.GetValue() == 2,
          "worker count lost on reload");
    if (!global) {
        Check(GlobalValues() == original_globals, "game round trip modified global values");
    }
}

static void TestUserEditAfterProfile() {
    PocoX7Pro::ApplyPerformanceProfile(true);
    Settings::values.resolution_setup.SetValue(Settings::ResolutionSetup::Res3_4X);
    Config config{true};
    config.WriteSettingGeneric(&Settings::values.resolution_setup);
    Settings::values.resolution_setup.SetValue(Settings::ResolutionSetup::Res1X);
    config.ReadSettingGeneric(&Settings::values.resolution_setup);
    Check(Settings::values.resolution_setup.GetValue() == Settings::ResolutionSetup::Res3_4X,
          "later user edit did not survive reload");
}

int main() {
    int failures = 0;
    const auto run = [&](const char* name, auto test) {
        try {
            Reset();
            test();
            std::printf("PASS: %s\n", name);
        } catch (const std::exception& error) {
            ++failures;
            std::printf("FAIL: %s: %s\n", name, error.what());
        }
    };
    run("global profile and unrelated preferences", TestGlobalProfile);
    run("per-game isolation", TestGameIsolation);
    run("global apply preserves game override", TestGlobalPreservesGame);
    run("global INI round trip", [] { TestRoundTrip(true); });
    run("per-game INI round trip", [] { TestRoundTrip(false); });
    run("user edits survive reload", TestUserEditAfterProfile);
    return failures != 0;
}
