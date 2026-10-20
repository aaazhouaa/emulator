// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include "common/common_types.h"
#include "core/tools/memory_scanner.h"

namespace Core {
class System;
}

namespace Core::Memory {
class Memory;
}

namespace Core::Timing {
class CoreTiming;
struct EventType;
} // namespace Core::Timing

namespace Tools {

/// Guest memory regions that a frozen address can be relocated against.
/// Stored instead of an absolute address so a saved table survives the address space
/// randomization applied on every boot.
enum class FreezeRegion : u8 {
    Absolute = 0, ///< Address is used verbatim (main NSO text/rodata, which is not randomized).
    Heap = 1,     ///< Address = heap base + offset.
    Alias = 2,    ///< Address = alias region base + offset.
    Code = 3,     ///< Address = code region base + offset.
};

[[nodiscard]] std::string_view NameOf(FreezeRegion region);
[[nodiscard]] bool ParseFreezeRegion(std::string_view name, FreezeRegion& out);

/**
 * This class allows the user to prevent an application from writing new values to certain memory
 * locations. This has a variety of uses when attempting to reverse a game.
 *
 * One example could be a cheat to prevent Mario from taking damage in SMO. One could freeze the
 * memory address that the game uses to store Mario's health so when he takes damage (and the game
 * tries to write the new health value to memory), the value won't change.
 */
class Freezer {
public:
    struct Entry {
        VAddr address{};
        u32 width{4};
        u64 value{};
        std::string description;
        ScanType type{ScanType::U32};
        FreezeRegion region{FreezeRegion::Absolute};
        bool enabled{true};
    };

    /// Receives the region bases needed to relocate saved entries.
    struct RegionBases {
        VAddr heap{};
        VAddr alias{};
        VAddr code{};
    };

    explicit Freezer(Core::Timing::CoreTiming& core_timing_, Core::System& system_);
    ~Freezer();

    // Enables or disables the entire memory freezer.
    void SetActive(bool is_active);

    // Returns whether or not the freezer is active.
    bool IsActive() const;

    // Removes all entries from the freezer.
    void Clear();

    // Freezes a value to its current memory address. The value the memory is kept at will be the
    // value that is read during this function. Width can be 1, 2, 4, or 8 (in bytes).
    u64 Freeze(VAddr address, u32 width);

    // Freezes a value to the given value instead of the value currently in memory. Adding an
    // address that is already frozen updates that entry in place.
    void FreezeAt(VAddr address, ScanType type, u64 value, std::string description);

    // Unfreezes the memory value at address. If the address isn't frozen, this is a no-op.
    void Unfreeze(VAddr address);

    // Returns whether or not the address is frozen.
    bool IsFrozen(VAddr address) const;

    // Sets the value that address should be frozen to. This doesn't change the width set by using
    // Freeze(). If the value isn't frozen, this will not freeze it and is thus a no-op.
    void SetFrozenValue(VAddr address, u64 value);

    // Renames an existing entry. No-op if the address is not frozen.
    void SetDescription(VAddr address, std::string description);

    // Enables or disables a single entry, leaving the rest of the freezer untouched.
    void SetEntryEnabled(VAddr address, bool enabled);

    // Returns the entry corresponding to the address if the address is frozen, otherwise
    // std::nullopt.
    std::optional<Entry> GetEntry(VAddr address) const;

    // Returns all the entries in the freezer, an empty vector means nothing is frozen.
    std::vector<Entry> GetEntries() const;

    // Reads the live value at the entry's address. Returns std::nullopt if the address is not
    // currently readable. Used by the frontend to display the current value next to the frozen one.
    std::optional<u64> GetCurrentValue(VAddr address);

    // Assigns a region to an entry so that it can be relocated on the next boot.
    void SetEntryRegion(VAddr address, FreezeRegion region, const RegionBases& bases);

    // Serializes the table to <LoadDir>/cheat_tables/<program_id>.json. Returns false on I/O error.
    bool SaveToDisk(u64 program_id) const;

    // Loads and relocates a previously saved table. Entries whose stored program id does not
    // match are discarded. Returns false if the file is absent or unreadable; the freezer is
    // left untouched in that case.
    bool LoadFromDisk(u64 program_id, const RegionBases& bases);

    // Replaces the whole entry list, used by the loader right after a successful load.
    void LoadEntries(std::vector<Entry> loaded);

    // Builds the on-disk path for a program id.
    [[nodiscard]] static std::string TablePath(u64 program_id);

private:
    using Entries = std::vector<Entry>;

    Entries::iterator FindEntry(VAddr address);
    Entries::const_iterator FindEntry(VAddr address) const;

    void FrameCallback(std::chrono::nanoseconds ns_late);
    void FillEntryReads();

    /// Reads through the current application process, so entries never hold a stale
    /// reference to a Memory instance that has been replaced by a process switch.
    [[nodiscard]] Core::Memory::Memory* CurrentMemory() const;

    std::atomic_bool active{false};

    /// Set once the entries have been populated so that re-activating the freezer does not
    /// overwrite values the user has chosen with whatever happens to be in memory.
    std::atomic_bool entries_primed{false};

    mutable std::mutex entries_mutex;
    Entries entries;

    std::shared_ptr<Core::Timing::EventType> event;
    Core::Timing::CoreTiming& core_timing;
    Core::System& system;
};

} // namespace Tools
