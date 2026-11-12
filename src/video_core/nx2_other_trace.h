// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <string>

#include "common/common_types.h"
#include "common/logging.h"
#include "common/nx2_switches.h"
#include "video_core/nx2_profile.h"

// TEST (switch other_trace, live): what the split's back spends outside every profile zone (the
// profile's "other"). Every ring entry the back runs is counted by kind; one in 16 is timed, the
// time of the zones it opened taken off (Nx2Profile's child time at the top level), so a draw's
// zones stay in their own rows. Logged every 10 s; exact with the profile's zones on (gpu_profile).
namespace VideoCommon::Nx2OtherTrace {

enum Class : u32 {
    EngineSwitch,  ///< Ring wrap or engine switch
    StateRegister, ///< 3D register write, not executable: stored, dirty flags set
    ConstantWords, ///< 3D constant buffer data words
    Executable3D,  ///< 3D method with side effects (draws, clears, reports, binds, ...)
    Macro3D,       ///< 3D macro method (0xE00 and up)
    OtherMethod,   ///< A method of another engine (compute, copy, 2D, inline)
    Multi3D,       ///< A run of 3D method data (uploads, constant buffer runs)
    MultiOther,    ///< A run of another engine's method data
    Call,          ///< A deferred call (fences, queries, flushes, ...)
    Count,
};

inline constexpr std::array<const char*, Count> class_names{
    "engine switch", "3D state register", "3D constant words", "3D executable method",
    "3D macro method", "other engine method", "3D data run", "other data run", "deferred call",
};

struct Window {
    std::array<u64, Count> entries{};
    std::array<u64, Count> words{};
    std::array<u64, Count> samples{};
    std::array<u64, Count> sample_ticks{};
    u64 start_ns{};
    u64 start_ticks{};
    u32 counter{};
};

/// The back's window (only the split's back runs entries).
inline thread_local Window t_window;

[[nodiscard]] inline bool Enabled() {
    return Common::Nx2Switches::On(Common::Nx2Switches::Id::OtherTrace);
}

/// Whether this entry is timed (one in 16).
[[nodiscard]] inline bool Sample() {
    return (++t_window.counter & 15) == 0;
}

inline void Note(Class kind, size_t words, bool sampled, u64 ticks) {
    Window& window = t_window;
    ++window.entries[kind];
    window.words[kind] += words;
    if (sampled) {
        ++window.samples[kind];
        window.sample_ticks[kind] += ticks;
    }
}

/// The back, between batches: logs every 10 s. A window older than 15 s (the switch was off)
/// starts over without a report.
inline void MaybeReport() {
    Window& window = t_window;
    const u64 now = Nx2Profile::Now();
    const u64 ticks = Nx2Profile::Ticks();
    if (window.start_ns == 0 || now - window.start_ns > 15'000'000'000ULL) {
        window = Window{};
        window.start_ns = now;
        window.start_ticks = ticks;
        return;
    }
    if (now - window.start_ns < 10'000'000'000ULL) {
        return;
    }
    const double seconds = static_cast<double>(now - window.start_ns) / 1e9;
    const double ns_per_tick = static_cast<double>(now - window.start_ns) /
                               static_cast<double>((std::max)(ticks - window.start_ticks, u64{1}));
    std::string text;
    double total_ms = 0.0;
    for (u32 kind = 0; kind < Count; ++kind) {
        if (window.entries[kind] == 0) {
            continue;
        }
        const double each_ns =
            window.samples[kind] != 0
                ? static_cast<double>(window.sample_ticks[kind]) * ns_per_tick /
                      static_cast<double>(window.samples[kind])
                : 0.0;
        const double per_second = static_cast<double>(window.entries[kind]) / seconds;
        const double ms = each_ns * per_second / 1e6;
        total_ms += ms;
        text += fmt::format(" | {}: {:.0f}/s ({:.0f} words/s), {:.1f} ms/s ({:.0f} ns each)",
                            class_names[kind], per_second,
                            static_cast<double>(window.words[kind]) / seconds, ms, each_ns);
    }
    LOG_INFO(HW_GPU, "Back entries per second{}:{} | total {:.1f} ms/s",
             Nx2Profile::g_zones_enabled ? " (zones taken off)"
                                         : " (zones off: zone time included)",
             text, total_ms);
    window = Window{};
    window.start_ns = now;
    window.start_ticks = ticks;
}

} // namespace VideoCommon::Nx2OtherTrace
