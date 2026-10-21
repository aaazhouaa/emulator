// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/assert.h"
#include "common/logging.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/hle/kernel/k_process.h"
#include "core/memory.h"
#include "core/tools/freezer.h"

#include <utility>

namespace Tools {
namespace {

constexpr auto memory_freezer_ns = std::chrono::nanoseconds{1000000000 / 60};

u64 MemoryReadWidth(Core::Memory::Memory& memory, u32 width, VAddr addr) {
    switch (width) {
    case 1:
        return memory.Read8(addr);
    case 2:
        return memory.Read16(addr);
    case 4:
        return memory.Read32(addr);
    case 8:
        return memory.Read64(addr);
    default:
        std::unreachable();
    }
}

void MemoryWriteWidth(Core::Memory::Memory& memory, u32 width, VAddr addr, u64 value) {
    switch (width) {
    case 1:
        memory.Write8(addr, static_cast<u8>(value));
        break;
    case 2:
        memory.Write16(addr, static_cast<u16>(value));
        break;
    case 4:
        memory.Write32(addr, static_cast<u32>(value));
        break;
    case 8:
        memory.Write64(addr, value);
        break;
    default:
        std::unreachable();
    }
}

} // Anonymous namespace

Freezer::Freezer(Core::Timing::CoreTiming& core_timing_, Core::System& system_)
    : core_timing{core_timing_}, system{system_} {
    event = Core::Timing::CreateEvent("MemoryFreezer::FrameCallback",
                                      [this](s64 time, std::chrono::nanoseconds ns_late)
                                          -> std::optional<std::chrono::nanoseconds> {
                                          FrameCallback(ns_late);
                                          return std::nullopt;
                                      });
    core_timing.ScheduleEvent(memory_freezer_ns, event);
}

Freezer::~Freezer() {
    core_timing.UnscheduleEvent(event);
}

Core::Memory::Memory* Freezer::CurrentMemory() const {
    auto* process = system.ApplicationProcess();
    if (process == nullptr) {
        return nullptr;
    }
    return &process->GetMemory();
}

void Freezer::SetActive(bool is_active) {
    // Compare-and-return instead of exchange: exchange() reports the *old* value, so asking for
    // the state we are already in (notably SetActive(false) while inactive) used to fall into
    // the activation branch, reschedule the event and re-prime the entries.
    if (active.load(std::memory_order_relaxed) == is_active) {
        return;
    }
    active.store(is_active, std::memory_order_relaxed);

    if (is_active) {
        // Only capture the initial values the first time. Re-priming on every activation would
        // overwrite the values the user has deliberately set.
        if (!entries_primed.exchange(true)) {
            FillEntryReads();
        }
        core_timing.ScheduleEvent(memory_freezer_ns, event);
        LOG_DEBUG(Common_Memory, "Memory freezer activated!");
    } else {
        // The already-queued callback is left in place: FrameCallback sees !IsActive() and
        // returns without rescheduling, so the loop ends on its own without touching the
        // CoreTiming queue from this thread.
        LOG_DEBUG(Common_Memory, "Memory freezer deactivated!");
    }
}

bool Freezer::IsActive() const {
    return active.load(std::memory_order_relaxed);
}

void Freezer::Clear() {
    std::scoped_lock lock{entries_mutex};

    LOG_DEBUG(Common_Memory, "Clearing all frozen memory values.");

    entries.clear();
    entries_primed.store(false, std::memory_order_relaxed);
}

u64 Freezer::Freeze(VAddr address, u32 width) {
    std::scoped_lock lock{entries_mutex};

    auto* memory = CurrentMemory();
    if (memory == nullptr) {
        return 0;
    }

    const auto current_value = MemoryReadWidth(*memory, width, address);
    entries.push_back({address, width, current_value, {}, ScanType::U32, true});

    LOG_DEBUG(Common_Memory,
              "Freezing memory for address={:016X}, width={:02X}, current_value={:016X}", address,
              width, current_value);

    return current_value;
}

void Freezer::FreezeAt(VAddr address, ScanType type, u64 value, std::string description) {
    const std::size_t width = MemoryScanner::WidthOf(type);
    if (width == 0) {
        // String/Aob entries have no scalar value and cannot be frozen this way.
        return;
    }

    {
        std::scoped_lock lock{entries_mutex};

        auto iter = FindEntry(address);
        if (iter != entries.end()) {
            iter->value = value;
            iter->type = type;
            iter->width = static_cast<u32>(width);
            iter->enabled = true;
            if (!description.empty()) {
                iter->description = std::move(description);
            }
            LOG_DEBUG(Common_Memory, "Updated frozen value for address={:016X} to {:016X}",
                      address, value);
        } else {
            entries.push_back({address, static_cast<u32>(width), value, std::move(description),
                               type, true});
            LOG_DEBUG(Common_Memory, "Freezing address={:016X}, width={:02X}, value={:016X}",
                      address, width, value);
        }
        // The user just set the value explicitly; never let the first activation re-read
        // memory and overwrite it.
        entries_primed.store(true, std::memory_order_relaxed);
    }

    // Apply the value immediately so the user sees the result even while emulation is paused.
    // The per-frame callback would otherwise only run once the game resumes.
    auto* memory = CurrentMemory();
    if (memory != nullptr && memory->IsValidVirtualAddress(Common::ProcessAddress(address))) {
        try {
            MemoryWriteWidth(*memory, static_cast<u32>(width), address, value);
        } catch (...) {
            LOG_DEBUG(Common_Memory, "Failed to apply freeze immediately at address={:016X}",
                      address);
        }
    }
}

void Freezer::Unfreeze(VAddr address) {
    std::scoped_lock lock{entries_mutex};

    LOG_DEBUG(Common_Memory, "Unfreezing memory for address={:016X}", address);

    // Dropping the entry stops the write-back, so whatever value the game writes next wins.
    entries.erase(std::remove_if(entries.begin(), entries.end(),
                                 [address](const Entry& entry) {
                                     return entry.address == address;
                                 }),
                  entries.end());
}

bool Freezer::IsFrozen(VAddr address) const {
    std::scoped_lock lock{entries_mutex};

    return FindEntry(address) != entries.cend();
}

void Freezer::SetFrozenValue(VAddr address, u64 value) {
    u32 width = 4;
    {
        std::scoped_lock lock{entries_mutex};

        const auto iter = FindEntry(address);
        if (iter == entries.cend()) {
            LOG_ERROR(Common_Memory,
                      "Tried to set freeze value for address={:016X} that is not frozen!",
                      address);
            return;
        }

        width = iter->width;
        LOG_DEBUG(Common_Memory,
                  "Manually overridden freeze value for address={:016X}, width={:02X} to "
                  "value={:016X}",
                  iter->address, iter->width, value);
        iter->value = value;
        entries_primed.store(true, std::memory_order_relaxed);
    }

    // Apply immediately so the edit is visible even while emulation is paused.
    auto* memory = CurrentMemory();
    if (memory != nullptr && memory->IsValidVirtualAddress(Common::ProcessAddress(address))) {
        try {
            MemoryWriteWidth(*memory, width, address, value);
        } catch (...) {
            LOG_DEBUG(Common_Memory, "Failed to apply freeze edit at address={:016X}", address);
        }
    }
}

void Freezer::SetDescription(VAddr address, std::string description) {
    std::scoped_lock lock{entries_mutex};

    const auto iter = FindEntry(address);
    if (iter == entries.cend()) {
        return;
    }
    iter->description = std::move(description);
}

void Freezer::SetEntryEnabled(VAddr address, bool enabled) {
    std::scoped_lock lock{entries_mutex};

    const auto iter = FindEntry(address);
    if (iter == entries.cend()) {
        return;
    }
    iter->enabled = enabled;
}

void Freezer::SyncActiveWithEntries() {
    const bool any_enabled = [this] {
        std::scoped_lock lock{entries_mutex};
        return std::any_of(entries.begin(), entries.end(),
                           [](const Entry& entry) { return entry.enabled; });
    }();

    // SetActive reschedules the callback when it turns the freezer on, and a freshly loaded
    // table is primed so activation does not overwrite the values the user saved.
    SetActive(any_enabled);
}

std::optional<Freezer::Entry> Freezer::GetEntry(VAddr address) const {
    std::scoped_lock lock{entries_mutex};

    const auto iter = FindEntry(address);
    if (iter == entries.cend()) {
        return std::nullopt;
    }

    return *iter;
}

std::vector<Freezer::Entry> Freezer::GetEntries() const {
    std::scoped_lock lock{entries_mutex};

    return entries;
}

std::optional<u64> Freezer::GetCurrentValue(VAddr address) {
    std::scoped_lock lock{entries_mutex};

    const auto iter = FindEntry(address);
    if (iter == entries.cend()) {
        return std::nullopt;
    }

    auto* memory = CurrentMemory();
    if (memory == nullptr || !memory->IsValidVirtualAddress(Common::ProcessAddress(address))) {
        return std::nullopt;
    }

    try {
        return MemoryReadWidth(*memory, iter->width, address);
    } catch (...) {
        return std::nullopt;
    }
}

Freezer::Entries::iterator Freezer::FindEntry(VAddr address) {
    return std::find_if(entries.begin(), entries.end(),
                        [address](const Entry& entry) { return entry.address == address; });
}

Freezer::Entries::const_iterator Freezer::FindEntry(VAddr address) const {
    return std::find_if(entries.begin(), entries.end(),
                        [address](const Entry& entry) { return entry.address == address; });
}

void Freezer::FrameCallback(std::chrono::nanoseconds ns_late) {
    if (!IsActive()) {
        LOG_DEBUG(Common_Memory, "Memory freezer has been deactivated, ending callback events.");
        return;
    }

    auto* memory = CurrentMemory();
    if (memory == nullptr) {
        // The process is gone or shutting down; keep the loop alive but do nothing this frame.
        core_timing.ScheduleEvent(memory_freezer_ns - ns_late, event);
        return;
    }

    {
        std::scoped_lock lock{entries_mutex};

        for (const auto& entry : entries) {
            if (!entry.enabled) {
                continue;
            }
            // Compare before writing: an unconditional write would dirty GPU-tracked pages and
            // force a rasterizer download every single frame for no benefit.
            u64 current{};
            try {
                current = MemoryReadWidth(*memory, entry.width, entry.address);
            } catch (...) {
                continue;
            }
            if (current == entry.value) {
                continue;
            }

            LOG_DEBUG(Common_Memory,
                      "Enforcing memory freeze at address={:016X}, value={:016X}, width={:02X}",
                      entry.address, entry.value, entry.width);
            try {
                MemoryWriteWidth(*memory, entry.width, entry.address, entry.value);
            } catch (...) {
                LOG_DEBUG(Common_Memory, "Failed to enforce freeze at address={:016X}",
                          entry.address);
            }
        }
    }

    core_timing.ScheduleEvent(memory_freezer_ns - ns_late, event);
}

void Freezer::FillEntryReads() {
    std::scoped_lock lock{entries_mutex};

    auto* memory = CurrentMemory();
    if (memory == nullptr) {
        return;
    }

    LOG_DEBUG(Common_Memory, "Updating memory freeze entries to current values.");

    for (auto& entry : entries) {
        if (!memory->IsValidVirtualAddress(Common::ProcessAddress(entry.address))) {
            continue;
        }
        try {
            entry.value = MemoryReadWidth(*memory, entry.width, entry.address);
        } catch (...) {
            LOG_DEBUG(Common_Memory, "Failed to read initial value at address={:016X}",
                      entry.address);
        }
    }
}

} // namespace Tools
