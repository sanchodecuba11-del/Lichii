// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <optional>
#include <string_view>

#include "common/common_types.h"

/// TEST (local game build): switches that turn a change or a measurement on and off while the game
/// runs, to compare frame rates and see what goes on without rebuilding or relaunching. Each one
/// is read where it decides (a relaxed load). Set at launch with
/// EDEN_NX2_SWITCHES="name=value,name=value" and live with the debug server's `switches` and
/// `set_switch` methods (tools/interrupteurs.py shows them as a panel).
namespace Common::Nx2Switches {

enum class Id : u32 {
    // Changes of our older pull requests (0 = off, the default: no fps effect measured in the cave).
    ListWords,         ///< #1336: the host GPU reads the list words it wrote (list draws, dispatches)
    FastDrawNeutral,   ///< #1216: more draws keep the fast path (stride words, render enable, ...)
    AstcEvenBc,        ///< #1194: uneven-mip ASTC re-compressed to BC without its uneven tail (AMD)
    AmdDrawBudget,     ///< #1088: 512 draws per submission on AMD's proprietary driver
    AmdQueryAvailability, ///< #1087: query results copied with their availability (AMD)
    ImagePool,         ///< #922: 8-16 MiB images placed in a bounded pool of 64 MiB blocks
    ListWaits,         ///< #1094 (no effect on #1161, which has its own version)
    AcqPark,           ///< #1477 (no effect on #1161, which has its own version)
    FastDrawAttribs,   ///< Vertex attribute writes that keep the pipeline key keep delta draws
    FastDrawViewports, ///< Viewport, scissor, blend constant, depth bias, stencil, line width writes keep delta draws
    FastDrawWrites,    ///< Up to 64 constant data ranges between two delta draws (16 before)
    FastDrawSerial,    ///< One serial bump per draw window on the drawing thread
    FastDrawUnread,    ///< Format writes of attributes the kept pipeline doesn't read keep delta draws
    WfiDedup,          ///< A wait-for-idle with nothing recorded since the last one records no barrier
    WfiKeepPass,       ///< A wait-for-idle in a render pass with no shader store since keeps it open
    ReportLight,       ///< Report operations fit std::function's inline storage
    DirectSink,        ///< Macro writes of state registers go through the sink
    Blit2dPlaced,      ///< The front places a 2D blit's writes on its destination surface
    ListCopyDefer,     ///< List copies after a dispatch are submitted with the next batch
    QmdWordExact,      ///< A QMD release reduction's base from the exact placed writes to its word
    VertexInputDedup,  ///< No vkCmdSetVertexInputEXT for the vertex input the command buffer has
    IdleBatch,         ///< The idle back waits out a deferred submission's window before submitting
    ShimAcquire,       ///< A greater-or-equal acquire the syncpoint shim meets now passes, no drain
    FrontMacroMemo,    ///< The front skips a macro call whose every register write would change nothing
    // Measurements (0 = off, the default). They log to the emulator's log.
    GpuProfile,        ///< Where the GPU threads spend their time, every 10 s
    StutterTrace,      ///< Each frame over 33 ms with its cause; a summary every 10 s
    FrameTrace,        ///< Per frame: CPU time of every thread, GPU time, presents (DKB)
    HostTiming,        ///< Late timers and the timing thread's waits, every second
    GuestWaitTrace,    ///< The game's syncpoint waits: how long, and the GPU chain behind them
    ParkTrace,         ///< What wakes a channel parked on a semaphore
    ThreadTrace,       ///< Where the game's threads wait (by reason), its sleeps, its wake-ups
    OtherTrace,        ///< The split's back outside every profile zone, by ring entry kind
    WorkerTrace,       ///< The Vulkan worker's time per command, by the source line that recorded it
    Count,
};

struct Info {
    const char* name;
    const char* description;
    int default_value;
    bool measurement;
    bool live; ///< False: read once at launch (the panel says so)
};

[[nodiscard]] const Info& Describe(Id id);

namespace detail {
inline std::array<std::atomic<int>, static_cast<size_t>(Id::Count)> g_values{};
inline std::atomic<bool> g_initialized{};
void Initialize();
} // namespace detail

/// The switch's value now.
[[nodiscard]] inline int Get(Id id) {
    if (!detail::g_initialized.load(std::memory_order_acquire)) [[unlikely]] {
        detail::Initialize();
    }
    return detail::g_values[static_cast<size_t>(id)].load(std::memory_order_relaxed);
}
[[nodiscard]] inline bool On(Id id) {
    return Get(id) != 0;
}

/// Sets a switch by name; false if no switch has that name.
bool Set(std::string_view name, int value);
void Set(Id id, int value);
[[nodiscard]] std::optional<Id> Find(std::string_view name);

} // namespace Common::Nx2Switches
