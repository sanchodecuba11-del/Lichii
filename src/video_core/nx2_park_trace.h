// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <chrono>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>

#include "common/common_types.h"
#include "common/logging.h"
#include "common/nx2_switches.h"

// TEST (EDEN_NX2_PARK_TRACE=1): for each channel the front parks on a semaphore acquire, what
// wrote the value that let it go on, and how long it waited:
// - a release the front read in a command list: one the game sent before the park (the front
//   had not read it yet) or after it (the game had not sent it yet);
// - GPU work: the value lane 0 predicts when a compute dispatch is recorded, a fence that landed
//   once the GPU ran the work, a report (query) that landed;
// - nothing seen: the game's CPU wrote it.
namespace VideoCommon::Nx2ParkTrace {

inline const bool g_enabled = std::getenv("EDEN_NX2_PARK_TRACE") != nullptr;

/// Also on with the switch park_trace (live).
[[nodiscard]] inline bool Enabled() {
    return g_enabled || Common::Nx2Switches::On(Common::Nx2Switches::Id::ParkTrace);
}

[[nodiscard]] inline u64 Now() {
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count());
}

enum class Source : u8 {
    FrontRelease, ///< A release the front read (and sent to the back)
    Predicted,    ///< Lane 0 recorded a compute dispatch: its release's value
    Landed,       ///< A fence's release written once the GPU ran the work
    Report,       ///< A report or payload release (query) written once the GPU ran it
};

/// When the command list the front reads now was made (the game's submission), 0 if unknown.
inline thread_local u64 t_list_submitted_ns{};
/// The channel the front reads now.
inline thread_local s32 t_front_channel{-1};

class Trace {
public:
    static Trace& Get() {
        static Trace trace;
        return trace;
    }

    void NoteWrite(u64 address, u32 value, Source source, s32 channel = -1,
                   u64 list_submitted_ns = 0) {
        std::scoped_lock lock{mutex};
        History& history = writes[address & ~u64{3}];
        Event& event = history.events[history.next++ % history.events.size()];
        event = {Now(), list_submitted_ns, value, channel, source};
    }

    void NotePark(const void* owner, s32 channel, u64 address, u32 value, u32 operation) {
        std::scoped_lock lock{mutex};
        parks[owner] = {Now(), address & ~u64{3}, value, operation, channel};
    }

    void NoteResumed(const void* owner) {
        std::scoped_lock lock{mutex};
        const auto it = parks.find(owner);
        if (it == parks.end()) {
            return;
        }
        const Park park = it->second;
        parks.erase(it);
        const u64 now = Now();
        // The first write after the park that meets the acquire.
        const Event* first = nullptr;
        if (const auto history = writes.find(park.address); history != writes.end()) {
            for (const Event& event : history->second.events) {
                if (event.time_ns >= park.since_ns && Meets(park, event.value) &&
                    (first == nullptr || event.time_ns < first->time_ns)) {
                    first = &event;
                }
            }
        }
        Category category = Category::Unseen;
        if (first != nullptr) {
            switch (first->source) {
            case Source::FrontRelease:
                category = first->list_submitted_ns > park.since_ns ? Category::SentAfter
                                                                    : Category::SentBefore;
                ++pairs[{park.channel, first->channel}];
                break;
            case Source::Predicted:
                category = Category::Predicted;
                break;
            case Source::Landed:
                category = Category::Landed;
                break;
            case Source::Report:
                category = Category::Report;
                break;
            }
            write_delay_ns[static_cast<size_t>(category)] += first->time_ns - park.since_ns;
        }
        ++counts[static_cast<size_t>(category)];
        parked_ns[static_cast<size_t>(category)] += now - park.since_ns;
        MaybeReport(now);
    }

private:
    enum class Category : size_t {
        SentBefore, ///< Released by a list the game sent before the park
        SentAfter,  ///< ... after the park
        Predicted,
        Landed,
        Report,
        Unseen, ///< No write seen: the game's CPU
        Count,
    };
    struct Event {
        u64 time_ns;
        u64 list_submitted_ns;
        u32 value;
        s32 channel;
        Source source;
    };
    struct History {
        std::array<Event, 8> events{};
        u32 next{};
    };
    struct Park {
        u64 since_ns;
        u64 address;
        u32 value;
        u32 operation;
        s32 channel;
    };

    static bool Meets(const Park& park, u32 value) {
        switch (park.operation) {
        case 0x1:
            return value == park.value;
        case 0x4:
            return value >= park.value;
        case 0x8:
            return (value & park.value) != 0;
        default:
            return true;
        }
    }

    void MaybeReport(u64 now) {
        // The first report, or the first since the switch was turned back on: a new window.
        if (window_ns == 0 || now - window_ns > 4'000'000'000ULL) {
            counts = {};
            parked_ns = {};
            write_delay_ns = {};
            pairs.clear();
            window_ns = now;
            return;
        }
        if (now - window_ns < 2'000'000'000ULL) {
            return;
        }
        const double seconds = static_cast<double>(now - window_ns) / 1e9;
        static constexpr std::array<const char*, static_cast<size_t>(Category::Count)> names{
            "release in a list sent before the park", "release in a list sent after the park",
            "compute release predicted", "compute release landed", "report landed",
            "no write seen (game CPU)"};
        std::string text;
        for (size_t i = 0; i < counts.size(); ++i) {
            if (counts[i] == 0) {
                continue;
            }
            text += fmt::format(" | {}: {:.0f}/s, parked {:.1f} ms/s ({:.0f} us each), written "
                                "{:.0f} us after the park",
                                names[i], static_cast<double>(counts[i]) / seconds,
                                static_cast<double>(parked_ns[i]) / 1e6 / seconds,
                                static_cast<double>(parked_ns[i]) / 1e3 / counts[i],
                                static_cast<double>(write_delay_ns[i]) / 1e3 / counts[i]);
        }
        std::string pair_text;
        for (const auto& [pair, count] : pairs) {
            pair_text += fmt::format(" {}<-{}:{}", pair.first, pair.second, count);
        }
        LOG_INFO(HW_GPU, "Park trace per second:{} | channel waits on channel:{}", text, pair_text);
        counts = {};
        parked_ns = {};
        write_delay_ns = {};
        pairs.clear();
        window_ns = now;
    }

    std::mutex mutex;
    std::unordered_map<u64, History> writes;
    std::unordered_map<const void*, Park> parks;
    std::array<u64, static_cast<size_t>(Category::Count)> counts{};
    std::array<u64, static_cast<size_t>(Category::Count)> parked_ns{};
    std::array<u64, static_cast<size_t>(Category::Count)> write_delay_ns{};
    std::map<std::pair<s32, s32>, u64> pairs;
    u64 window_ns{};
};

} // namespace VideoCommon::Nx2ParkTrace
