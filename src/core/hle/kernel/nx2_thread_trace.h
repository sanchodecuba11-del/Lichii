// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/common_types.h"
#include "common/logging.h"
#include "common/nx2_switches.h"

// TEST (EDEN_NX2_THREAD_TRACE=1): where the game's threads wait. For each guest thread, the time
// it spends blocked, by why (sleep, waiting on objects, condition variable, address arbitration,
// IPC reply) and, for object waits, on which object (events services made carry their name: the
// GPU's syncpoint slots, the display's vsync...). Logged every 2 s, the most blocked threads first.
namespace Kernel::Nx2ThreadTrace {

inline const bool g_enabled = std::getenv("EDEN_NX2_THREAD_TRACE") != nullptr;

[[nodiscard]] inline bool Enabled() {
    return g_enabled || Common::Nx2Switches::On(Common::Nx2Switches::Id::ThreadTrace);
}

class Trace {
public:
    static Trace& Get() {
        static Trace trace;
        return trace;
    }

    void NameObject(const void* object, std::string name) {
        std::scoped_lock lock{mutex};
        names[object] = std::move(name);
    }

    /// The thread waits on `count` objects, the first `object`.
    void NoteWaitObjects(const void* thread, const void* object, s32 count) {
        std::scoped_lock lock{mutex};
        Thread& t = threads[thread];
        t.object = object;
        t.object_count = count;
    }

    /// The scheduler switches a core to the thread: how long after its wait ended.
    void NoteRun(const void* thread) {
        std::scoped_lock lock{mutex};
        const auto it = threads.find(thread);
        if (it == threads.end() || it->second.ready_at == 0) {
            return;
        }
        Thread& t = it->second;
        const u64 latency = Now() - t.ready_at;
        t.ready_at = 0;
        const size_t bucket = latency < 20'000 ? 0 : latency < 100'000 ? 1 : latency < 500'000 ? 2 : 3;
        ++run_latency[t.ready_reason][bucket];
        run_latency_ns[t.ready_reason] += latency;
    }

    /// A guest sleep (svcSleepThread): what it asked for and what it got.
    void NoteSleep(s64 requested_ns, u64 actual_ns) {
        std::scoped_lock lock{mutex};
        const size_t bucket = requested_ns < 100'000 ? 0 : requested_ns < 1'000'000 ? 1 : 2;
        ++sleeps[bucket];
        sleep_requested_ns[bucket] += static_cast<u64>(requested_ns);
        sleep_actual_ns[bucket] += actual_ns;
    }

    /// A state change: `reason` is the wait's (ThreadWaitReasonForDebugging) when it ends.
    void NoteState(const void* thread, u64 thread_id, bool was_waiting, bool now_waiting,
                   u32 reason) {
        if (was_waiting == now_waiting) {
            return;
        }
        std::scoped_lock lock{mutex};
        const u64 now = Now();
        Thread& t = threads[thread];
        t.id = thread_id;
        if (now_waiting) {
            t.wait_start = now;
            return;
        }
        if (t.wait_start == 0) {
            return;
        }
        const u64 waited = now - t.wait_start;
        t.wait_start = 0;
        const size_t index = (std::min)(static_cast<size_t>(reason), Reasons - 1);
        t.ready_at = now;
        t.ready_reason = index;
        t.blocked_ns[index] += waited;
        ++t.waits[index];
        if (index == SynchronizationReason && t.object != nullptr) {
            const auto name = names.find(t.object);
            std::string label = name != names.end() ? name->second : "unnamed object";
            if (t.object_count > 1) {
                label += fmt::format(" (+{})", t.object_count - 1);
            }
            t.objects[label] += waited;
        }
        t.object = nullptr;
        MaybeReport(now);
    }

private:
    static constexpr size_t Reasons = 7;
    static constexpr size_t SynchronizationReason = 3;

    struct Thread {
        u64 id{};
        u64 wait_start{};
        u64 ready_at{};
        size_t ready_reason{};
        const void* object{};
        s32 object_count{};
        std::array<u64, Reasons> blocked_ns{};
        std::array<u64, Reasons> waits{};
        std::map<std::string, u64> objects;
    };

    static u64 Now() {
        return static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                    std::chrono::steady_clock::now().time_since_epoch())
                                    .count());
    }

    void MaybeReport(u64 now) {
        // The first report, or the first since the switch was turned back on: a new window.
        if (window == 0 || now - window > 4'000'000'000ULL) {
            for (auto& [key, t] : threads) {
                t.blocked_ns = {};
                t.waits = {};
                t.objects.clear();
            }
            sleeps = {};
            sleep_requested_ns = {};
            sleep_actual_ns = {};
            run_latency = {};
            run_latency_ns = {};
            window = now;
            return;
        }
        if (now - window < 2'000'000'000ULL) {
            return;
        }
        const double seconds = static_cast<double>(now - window) / 1e9;
        std::vector<const Thread*> sorted;
        for (const auto& [key, t] : threads) {
            sorted.push_back(&t);
        }
        const auto total = [](const Thread* t) {
            u64 sum = 0;
            for (const u64 ns : t->blocked_ns) {
                sum += ns;
            }
            return sum;
        };
        // The threads that wait most often: the game's busy ones (idle threads wait for seconds).
        const auto waits = [](const Thread* t) {
            u64 sum = 0;
            for (const u64 count : t->waits) {
                sum += count;
            }
            return sum;
        };
        std::ranges::sort(sorted, [&](const Thread* a, const Thread* b) { return waits(a) > waits(b); });
        static constexpr std::array<const char*, Reasons> reason_names{
            "?", "sleep", "IPC", "objects", "condvar", "arbitration", "suspended"};
        std::string text;
        for (size_t i = 0; i < (std::min)(sorted.size(), size_t{14}); ++i) {
            const Thread& t = *sorted[i];
            text += fmt::format(" || thread {}: blocked {:.0f} ms/s", t.id,
                                static_cast<double>(total(&t)) / 1e6 / seconds);
            for (size_t r = 0; r < Reasons; ++r) {
                if (t.waits[r] != 0) {
                    text += fmt::format(", {} {:.0f} ms/s ({:.0f}/s)", reason_names[r],
                                        static_cast<double>(t.blocked_ns[r]) / 1e6 / seconds,
                                        static_cast<double>(t.waits[r]) / seconds);
                }
            }
            std::vector<std::pair<u64, std::string>> objects;
            for (const auto& [label, ns] : t.objects) {
                objects.emplace_back(ns, label);
            }
            std::ranges::sort(objects, std::greater{});
            for (size_t o = 0; o < (std::min)(objects.size(), size_t{3}); ++o) {
                text += fmt::format(" [{}: {:.0f} ms/s]", objects[o].second,
                                    static_cast<double>(objects[o].first) / 1e6 / seconds);
            }
        }
        static constexpr std::array<const char*, 3> buckets{"< 100 us", "100 us - 1 ms", ">= 1 ms"};
        std::string sleep_text;
        for (size_t b = 0; b < buckets.size(); ++b) {
            if (sleeps[b] != 0) {
                sleep_text += fmt::format(" | asked {}: {:.0f}/s, asked {:.0f} us, slept {:.0f} us each",
                                          buckets[b], static_cast<double>(sleeps[b]) / seconds,
                                          static_cast<double>(sleep_requested_ns[b]) / 1e3 / sleeps[b],
                                          static_cast<double>(sleep_actual_ns[b]) / 1e3 / sleeps[b]);
            }
        }
        sleeps = {};
        sleep_requested_ns = {};
        sleep_actual_ns = {};
        std::string run_text;
        for (size_t r = 0; r < Reasons; ++r) {
            u64 count = 0;
            for (const u64 c : run_latency[r]) {
                count += c;
            }
            if (count == 0) {
                continue;
            }
            run_text += fmt::format(" | after {}: {:.0f}/s, {:.0f} us each (< 20 us {}, < 100 us {}, "
                                    "< 500 us {}, more {})",
                                    reason_names[r], static_cast<double>(count) / seconds,
                                    static_cast<double>(run_latency_ns[r]) / 1e3 / count,
                                    run_latency[r][0], run_latency[r][1], run_latency[r][2],
                                    run_latency[r][3]);
        }
        run_latency = {};
        run_latency_ns = {};
        LOG_INFO(Kernel, "Guest threads from ready to running per second:{}", run_text);
        LOG_INFO(Kernel, "Guest sleeps per second:{}", sleep_text);
        LOG_INFO(Kernel, "Guest threads waiting per second:{}", text);
        for (auto& [key, t] : threads) {
            t.blocked_ns = {};
            t.waits = {};
            t.objects.clear();
        }
        window = now;
    }

    std::array<std::array<u64, 4>, Reasons> run_latency{};
    std::array<u64, Reasons> run_latency_ns{};
    std::array<u64, 3> sleeps{};
    std::array<u64, 3> sleep_requested_ns{};
    std::array<u64, 3> sleep_actual_ns{};
    std::mutex mutex;
    std::unordered_map<const void*, std::string> names;
    std::unordered_map<const void*, Thread> threads;
    u64 window{};
};

} // namespace Kernel::Nx2ThreadTrace
