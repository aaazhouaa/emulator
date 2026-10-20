// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/assert.h"
#include "common/fs/file.h"
#include "common/fs/fs.h"
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/hle/kernel/k_process.h"
#include "core/memory.h"
#include "core/tools/freezer.h"

#include <nlohmann/json.hpp>

namespace Tools {
namespace {

constexpr auto memory_freezer_ns = std::chrono::nanoseconds{1000000000 / 60};
constexpr int TableFormatVersion = 1;

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

VAddr BaseOfRegion(FreezeRegion region, const Freezer::RegionBases& bases) {
    switch (region) {
    case FreezeRegion::Heap:
        return bases.heap;
    case FreezeRegion::Alias:
        return bases.alias;
    case FreezeRegion::Code:
        return bases.code;
    case FreezeRegion::Absolute:
        break;
    }
    return 0;
}

} // Anonymous namespace

std::string_view NameOf(FreezeRegion region) {
    switch (region) {
    case FreezeRegion::Absolute:
        return "absolute";
    case FreezeRegion::Heap:
        return "heap";
    case FreezeRegion::Alias:
        return "alias";
    case FreezeRegion::Code:
        return "code";
    }
    return "absolute";
}

bool ParseFreezeRegion(std::string_view name, FreezeRegion& out) {
    for (const auto candidate :
         {FreezeRegion::Absolute, FreezeRegion::Heap, FreezeRegion::Alias, FreezeRegion::Code}) {
        if (NameOf(candidate) == name) {
            out = candidate;
            return true;
        }
    }
    return false;
}

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
    if (!active.exchange(is_active)) {
        // Only capture the initial values the first time. Re-priming on every activation would
        // overwrite the values the user has deliberately set.
        if (!entries_primed.exchange(true)) {
            FillEntryReads();
        }
        core_timing.ScheduleEvent(memory_freezer_ns, event);
        LOG_DEBUG(Common_Memory, "Memory freezer activated!");
    } else {
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
    entries.push_back({address, width, current_value, {}, ScanType::U32, FreezeRegion::Absolute,
                       true});

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
                               type, FreezeRegion::Absolute, true});
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

void Freezer::SetEntryRegion(VAddr address, FreezeRegion region, const RegionBases& bases) {
    std::scoped_lock lock{entries_mutex};

    const auto iter = FindEntry(address);
    if (iter == entries.cend()) {
        return;
    }

    const VAddr base = BaseOfRegion(region, bases);
    if (region != FreezeRegion::Absolute && (base == 0 || address < base)) {
        // The address does not live in the requested region; keep it absolute rather than
        // storing a nonsensical offset.
        iter->region = FreezeRegion::Absolute;
        return;
    }

    iter->region = region;
    iter->address = region == FreezeRegion::Absolute ? address : address - base;
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

void Freezer::LoadEntries(std::vector<Entry> loaded) {
    std::scoped_lock lock{entries_mutex};

    entries = std::move(loaded);
    // Loaded values are authoritative; do not let the first activation overwrite them.
    entries_primed.store(true, std::memory_order_relaxed);
}

std::string Freezer::TablePath(u64 program_id) {
    const auto dir = Common::FS::GetCitronPath(Common::FS::CitronPath::LoadDir) / "cheat_tables";
    return (dir / fmt::format("{:016X}.json", program_id)).string();
}

bool Freezer::SaveToDisk(u64 program_id) const {
    std::scoped_lock lock{entries_mutex};

    nlohmann::json root;
    root["version"] = TableFormatVersion;
    root["program_id"] = fmt::format("{:016X}", program_id);

    auto entries_json = nlohmann::json::array();
    for (const auto& entry : entries) {
        auto item = nlohmann::json::object();
        item["region"] = std::string{NameOf(entry.region)};
        item["offset"] = fmt::format("{:016X}", entry.address);
        item["type"] = std::string{MemoryScanner::NameOf(entry.type)};
        item["frozen"] = entry.enabled;
        item["value"] = fmt::format("{:016X}", entry.value);
        item["description"] = entry.description;
        entries_json.push_back(std::move(item));
    }
    root["entries"] = std::move(entries_json);

    const auto path = TablePath(program_id);
    if (!Common::FS::CreateParentDirs(path)) {
        LOG_ERROR(Common_Memory, "Failed to create the cheat table directory for {:016X}",
                  program_id);
        return false;
    }

    const auto text = root.dump(2);
    // Write to a sibling file first so an interrupted save cannot truncate a good table.
    const auto temporary = path + ".tmp";
    if (Common::FS::WriteStringToFile(temporary, Common::FS::FileType::TextFile, text) !=
        text.size()) {
        LOG_ERROR(Common_Memory, "Failed to write the cheat table for {:016X}", program_id);
        void(Common::FS::RemoveFile(temporary));
        return false;
    }

    std::error_code rename_error;
    std::filesystem::rename(temporary, path, rename_error);
    if (rename_error) {
        LOG_ERROR(Common_Memory, "Failed to replace the cheat table for {:016X}: {}", program_id,
                  rename_error.message());
        void(Common::FS::RemoveFile(temporary));
        return false;
    }

    LOG_INFO(Common_Memory, "Saved {} frozen entries for {:016X}", entries.size(), program_id);
    return true;
}

bool Freezer::LoadFromDisk(u64 program_id, const RegionBases& bases) {
    const auto path = TablePath(program_id);
    if (!Common::FS::Exists(path)) {
        return false;
    }

    const auto text = Common::FS::ReadStringFromFile(path, Common::FS::FileType::TextFile);
    if (text.empty()) {
        LOG_WARNING(Common_Memory, "Cheat table for {:016X} is empty or unreadable", program_id);
        return false;
    }

    nlohmann::json root;
    try {
        root = nlohmann::json::parse(text);
    } catch (const nlohmann::json::exception& e) {
        LOG_ERROR(Common_Memory, "Failed to parse the cheat table for {:016X}: {}", program_id,
                  e.what());
        return false;
    }

    if (!root.is_object()) {
        return false;
    }

    // A table saved for a different title must not be applied to this one.
    const auto stored_program = root.value("program_id", std::string{});
    if (stored_program != fmt::format("{:016X}", program_id)) {
        LOG_WARNING(Common_Memory, "Ignoring cheat table {:016X}: it belongs to {}", program_id,
                    stored_program);
        return false;
    }

    const int version = root.value("version", 0);
    if (version != TableFormatVersion) {
        LOG_WARNING(Common_Memory, "Ignoring cheat table {:016X}: unsupported version {}",
                    program_id, version);
        return false;
    }

    std::vector<Entry> loaded;
    const auto entries_json = root.find("entries");
    if (entries_json == root.end() || !entries_json->is_array()) {
        return false;
    }

    for (const auto& item : *entries_json) {
        if (!item.is_object()) {
            continue;
        }

        Entry entry;

        FreezeRegion region{FreezeRegion::Absolute};
        if (!ParseFreezeRegion(item.value("region", std::string{"absolute"}), region)) {
            region = FreezeRegion::Absolute;
        }

        ScanType type{ScanType::U32};
        if (!MemoryScanner::ParseScanType(item.value("type", std::string{"u32"}), type)) {
            type = ScanType::U32;
        }
        const auto width = MemoryScanner::WidthOf(type);
        if (width == 0) {
            continue;
        }

        u64 offset{};
        try {
            offset = std::stoull(item.value("offset", std::string{"0"}), nullptr, 16);
        } catch (const std::exception&) {
            continue;
        }

        u64 value{};
        try {
            value = std::stoull(item.value("value", std::string{"0"}), nullptr, 16);
        } catch (const std::exception&) {
            value = 0;
        }

        const VAddr base = BaseOfRegion(region, bases);
        if (region != FreezeRegion::Absolute && base == 0) {
            // The region does not exist in this session, so the entry cannot be relocated.
            LOG_DEBUG(Common_Memory, "Dropping frozen entry: region {} is unavailable",
                      NameOf(region));
            continue;
        }

        entry.address = region == FreezeRegion::Absolute ? offset : base + offset;
        entry.width = static_cast<u32>(width);
        entry.type = type;
        entry.value = value;
        entry.description = item.value("description", std::string{});
        entry.enabled = item.value("frozen", true);
        entry.region = region;

        loaded.push_back(std::move(entry));
    }

    LOG_INFO(Common_Memory, "Loaded {} frozen entries for {:016X}", loaded.size(), program_id);
    LoadEntries(std::move(loaded));
    return true;
}

} // namespace Tools
