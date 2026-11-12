// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <chrono>
#include <cstdlib>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>

#include "common/common_types.h"
#include "common/logging.h"
#include "common/nx2_switches.h"

// TEST (EDEN_NX2_PARK_TRACE=1, or the switch guest_wait_trace): how the game waits for the GPU. For each
// wait of the game on a syncpoint (nvhost_ctrl event wait), how long it waits, and how long the
// GPU took from the game's submission of the list that increments it to the signal (the chain:
// front, back, lanes, Vulkan, GPU, fence). For each list a channel submits, how long after the
// end of the game's last wait.
namespace VideoCommon::Nx2GuestWaitTrace {

inline const bool g_enabled = std::getenv("EDEN_NX2_PARK_TRACE") != nullptr;

[[nodiscard]] inline bool Enabled() {
    return g_enabled || Common::Nx2Switches::On(Common::Nx2Switches::Id::GuestWaitTrace);
}

[[nodiscard]] inline u64 Now() {
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count());
}

class Trace {
public:
    static Trace& Get() {
        static Trace trace;
        return trace;
    }

    /// The game submitted a list of `channel` that raises syncpoint `id` to `value`.
    void NoteSubmit(s32 channel, u32 id, u32 value) {
        std::scoped_lock lock{mutex};
        const u64 now = Now();
        if (value != 0) {
            auto& list = submits[id];
            list.push_back({value, now});
            if (list.size() > 256) {
                list.pop_front();
            }
        }
        Channel& stats = channels[channel];
        ++stats.submits;
        if (last_wait_end != 0) {
            const u64 gap = now - last_wait_end;
            stats.gap_ns += gap;
            stats.after_wait += gap < 200'000 ? 1 : 0;
        }
        MaybeReport(now);
    }

    /// The game found the syncpoint already met, or polled without waiting.
    void NoteMet(u32 id) {
        std::scoped_lock lock{mutex};
        ++syncpoints[id].met;
    }
    void NotePoll(u32 id) {
        std::scoped_lock lock{mutex};
        ++syncpoints[id].polls;
    }

    /// The game waits on event `slot` for syncpoint `id` to reach `value`.
    void NoteWaitStart(u32 slot, u32 id, u32 value) {
        std::scoped_lock lock{mutex};
        waits[slot] = {Now(), id, value};
    }
    /// The GPU signalled it.
    void NoteWaitEnd(u32 slot) {
        std::scoped_lock lock{mutex};
        const auto it = waits.find(slot);
        if (it == waits.end()) {
            return;
        }
        const Wait wait = it->second;
        waits.erase(it);
        const u64 now = Now();
        last_wait_end = now;
        Syncpoint& stats = syncpoints[wait.id];
        ++stats.waits;
        stats.wait_ns += now - wait.start_ns;
        // When the game submitted the list that raises the syncpoint to this value.
        if (const auto list = submits.find(wait.id); list != submits.end()) {
            for (const auto& [value, time] : list->second) {
                if (static_cast<s32>(value - wait.value) >= 0) {
                    stats.chain_ns += now - time;
                    ++stats.chains;
                    break;
                }
            }
        }
        MaybeReport(now);
    }

private:
    struct Wait {
        u64 start_ns;
        u32 id;
        u32 value;
    };
    struct Syncpoint {
        u64 waits{}, wait_ns{}, chains{}, chain_ns{}, met{}, polls{};
    };
    struct Channel {
        u64 submits{}, gap_ns{}, after_wait{};
    };

    void MaybeReport(u64 now) {
        // The first report, or the first since the switch was turned back on: a new window.
        if (window_ns == 0 || now - window_ns > 4'000'000'000ULL) {
            syncpoints.clear();
            channels.clear();
            window_ns = now;
            return;
        }
        if (now - window_ns < 2'000'000'000ULL) {
            return;
        }
        const double seconds = static_cast<double>(now - window_ns) / 1e9;
        std::string text;
        for (const auto& [id, s] : syncpoints) {
            if (s.waits == 0 && s.met == 0 && s.polls == 0) {
                continue;
            }
            text += fmt::format(
                " | syncpoint {}: {:.0f} waits/s, {:.1f} ms/s waited ({:.0f} us each), submit to "
                "signal {:.0f} us, {:.0f} met at once/s, {:.0f} polls/s",
                id, static_cast<double>(s.waits) / seconds,
                static_cast<double>(s.wait_ns) / 1e6 / seconds,
                s.waits != 0 ? static_cast<double>(s.wait_ns) / 1e3 / s.waits : 0.0,
                s.chains != 0 ? static_cast<double>(s.chain_ns) / 1e3 / s.chains : 0.0,
                static_cast<double>(s.met) / seconds, static_cast<double>(s.polls) / seconds);
        }
        std::string channel_text;
        for (const auto& [channel, c] : channels) {
            channel_text += fmt::format(
                " | channel {}: {:.0f} lists/s, {:.0f} us after the game's last wait ({:.0f}% "
                "within 200 us)",
                channel, static_cast<double>(c.submits) / seconds,
                c.submits != 0 ? static_cast<double>(c.gap_ns) / 1e3 / c.submits : 0.0,
                c.submits != 0 ? 100.0 * static_cast<double>(c.after_wait) / c.submits : 0.0);
        }
        LOG_INFO(HW_GPU, "Game waits per second:{}{}", text, channel_text);
        syncpoints.clear();
        channels.clear();
        window_ns = now;
    }

    std::mutex mutex;
    std::unordered_map<u32, std::deque<std::pair<u32, u64>>> submits;
    std::unordered_map<u32, Wait> waits;
    std::map<u32, Syncpoint> syncpoints;
    std::map<s32, Channel> channels;
    u64 last_wait_end{};
    u64 window_ns{};
};

} // namespace VideoCommon::Nx2GuestWaitTrace
