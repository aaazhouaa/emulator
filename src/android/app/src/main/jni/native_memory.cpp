// SPDX-FileCopyrightText: Copyright 2025 Citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <jni.h>

#include <nlohmann/json.hpp>

#include "common/android/android_common.h"
#include "common/fs/file.h"
#include "common/fs/fs.h"
#include "common/logging.h"
#include "common/scope_exit.h"
#include "core/arm/debug.h"
#include "core/core.h"
#include "core/hle/kernel/k_process.h"
#include "core/memory.h"
#include "core/tools/freezer.h"
#include "core/tools/memory_scanner.h"
#include "jni/native.h"

namespace {

constexpr jint ResultSuccess = 0;
constexpr jint ResultNoProcess = 1;
constexpr jint ResultInvalidArgument = 2;
constexpr jint ResultAlreadyRunning = 3;
constexpr jint ResultNothingToRefine = 4;

/// Scan states reported through memScanPoll.
constexpr jlong ScanStateIdle = 0;
constexpr jlong ScanStateRunning = 1;
constexpr jlong ScanStateDone = 2;
constexpr jlong ScanStateCancelled = 3;
/// Guard against a runaway result set being copied across JNI in one call.
constexpr jint MaxTransferCount = 20000;

/// Hard cap on the bytes a single hex-view read may request.
constexpr jint MaxReadBytes = 0x100000;

/// Every region a frozen address can be anchored to, with its current base and size.
struct RegionSpan {
    Tools::FreezeRegion region;
    VAddr base;
    u64 size;
};

std::vector<RegionSpan> CurrentRegionSpans(Core::System& system) {
    std::vector<RegionSpan> spans;
    const auto* process = system.ApplicationProcess();
    if (process == nullptr) {
        return spans;
    }

    const auto& page_table = process->GetPageTable();
    const auto add = [&spans](Tools::FreezeRegion region, VAddr base, u64 size) {
        if (base != 0 && size != 0) {
            spans.push_back({region, base, size});
        }
    };

    add(Tools::FreezeRegion::Heap, GetInteger(page_table.GetHeapRegionStart()),
        page_table.GetHeapRegionSize());
    add(Tools::FreezeRegion::Alias, GetInteger(page_table.GetAliasRegionStart()),
        page_table.GetAliasRegionSize());
    add(Tools::FreezeRegion::Code, GetInteger(page_table.GetAliasCodeRegionStart()),
        page_table.GetAliasCodeRegionSize());

    return spans;
}

/// Finds the region that contains the address. Falls back to Absolute, which is correct for
/// the main NSO text/rodata, as those are not randomized.
Tools::FreezeRegion RegionContaining(Core::System& system, VAddr address, VAddr& out_base) {
    for (const auto& span : CurrentRegionSpans(system)) {
        if (address >= span.base && address < span.base + span.size) {
            out_base = span.base;
            return span.region;
        }
    }
    out_base = 0;
    return Tools::FreezeRegion::Absolute;
}

/// Converts a stored entry address back to an absolute one, honouring its region anchor.
VAddr AbsoluteAddressOf(Core::System& system, const Tools::Freezer::Entry& entry) {
    if (entry.region == Tools::FreezeRegion::Absolute) {
        return entry.address;
    }
    for (const auto& span : CurrentRegionSpans(system)) {
        if (span.region == entry.region) {
            return span.base + entry.address;
        }
    }
    return entry.address;
}

/**
 * Owns the scan state shared with the Kotlin side.
 *
 * The scan itself runs on a worker thread that never touches JNI, so no JNIEnv or local
 * reference may be captured by it. The frontend instead polls memScanPoll.
 */
class MemoryScanSession {
public:
    static MemoryScanSession& GetInstance() {
        static MemoryScanSession instance;
        return instance;
    }

    jint Start(Core::System& system, const Tools::ScanParams& params) {
        std::scoped_lock lock{mutex};

        if (running) {
            return ResultAlreadyRunning;
        }
        if (system.IsShuttingDown() || system.ApplicationProcess() == nullptr) {
            return ResultNoProcess;
        }

        // Bump the generation so a late-finishing worker cannot publish results that belong
        // to a newer scan.
        ++generation;
        const u64 my_generation = generation;

        running = true;
        cancel = false;
        cancelled = false;
        done = 0;
        total = params.size;
        results.clear();

        if (worker.joinable()) {
            worker.join();
        }

        worker = std::thread([this, &system, params, my_generation]() {
            const auto read = [&system](VAddr address, void* data, std::size_t size) {
                auto* process = system.ApplicationProcess();
                if (process == nullptr || system.IsShuttingDown()) {
                    return false;
                }
                auto& memory = process->GetMemory();
                if (!memory.IsValidVirtualAddressRange(Common::ProcessAddress(address), size)) {
                    return false;
                }
                return memory.ReadBlockUnsafe(Common::ProcessAddress(address), data, size);
            };

            const auto progress = [this, my_generation](u64 scanned, u64 scanned_total) {
                {
                    std::scoped_lock progress_lock{mutex};
                    if (generation != my_generation) {
                        return false;
                    }
                    done = scanned;
                    total = scanned_total;
                    if (cancel) {
                        cancelled = true;
                        return false;
                    }
                }
                return true;
            };

            auto found = Tools::MemoryScanner::Scan(read, params, progress);

            std::scoped_lock publish_lock{mutex};
            if (generation != my_generation) {
                return;
            }
            results = std::move(found);
            running = false;
        });

        return ResultSuccess;
    }

    jint Refine(Core::System& system, Tools::ScanType type, Tools::RefineType compare, u64 value,
                f64 epsilon) {
        std::scoped_lock lock{mutex};

        if (running) {
            return ResultAlreadyRunning;
        }
        if (results.empty()) {
            return ResultNothingToRefine;
        }
        if (system.IsShuttingDown() || system.ApplicationProcess() == nullptr) {
            return ResultNoProcess;
        }

        const auto read = [&system](VAddr address, void* data, std::size_t size) {
            auto* process = system.ApplicationProcess();
            if (process == nullptr || system.IsShuttingDown()) {
                return false;
            }
            auto& memory = process->GetMemory();
            if (!memory.IsValidVirtualAddressRange(Common::ProcessAddress(address), size)) {
                return false;
            }
            return memory.ReadBlockUnsafe(Common::ProcessAddress(address), data, size);
        };

        // The values observed at scan time are the reference for the change comparisons, so
        // they are captured before refine rewrites the candidate list.
        std::vector<u64> previous;
        previous.reserve(results.size());
        for (const auto address : results) {
            u64 current{};
            previous.push_back(Tools::MemoryScanner::ReadValue(read, type, address, current)
                                   ? current
                                   : 0);
        }

        results = Tools::MemoryScanner::Refine(read, std::move(results), previous, type, compare,
                                               value, epsilon);
        done = 0;
        total = 0;
        return ResultSuccess;
    }

    void Cancel() {
        std::scoped_lock lock{mutex};
        cancel = true;
    }

    void Clear() {
        std::scoped_lock lock{mutex};
        results.clear();
        done = 0;
        total = 0;
        cancelled = false;
    }

    void WaitForWorker() {
        std::thread pending;
        {
            std::scoped_lock lock{mutex};
            cancel = true;
            if (worker.joinable()) {
                pending = std::move(worker);
            }
        }
        if (pending.joinable()) {
            pending.join();
        }
    }

    std::array<jlong, 3> Poll() {
        std::scoped_lock lock{mutex};
        jlong state = ScanStateDone;
        if (running) {
            state = ScanStateRunning;
        } else if (cancelled) {
            state = ScanStateCancelled;
        } else if (results.empty() && done == 0 && total == 0) {
            // Nothing has been scanned yet.
            state = ScanStateIdle;
        } else if (results.empty() && cancelled) {
            state = ScanStateCancelled;
        }
        return {state, static_cast<jlong>(done), static_cast<jlong>(total)};
    }

    std::size_t Count() {
        std::scoped_lock lock{mutex};
        return results.size();
    }

    std::vector<VAddr> Snapshot(jint offset, jint count) {
        std::scoped_lock lock{mutex};
        std::vector<VAddr> slice;
        if (offset < 0 || count <= 0) {
            return slice;
        }
        const auto begin = static_cast<std::size_t>(offset);
        if (begin >= results.size()) {
            return slice;
        }
        const auto want =
            std::min<std::size_t>(static_cast<std::size_t>(count), results.size() - begin);
        slice.assign(results.begin() + static_cast<std::ptrdiff_t>(begin),
                     results.begin() + static_cast<std::ptrdiff_t>(begin + want));
        return slice;
    }

private:
    MemoryScanSession() = default;
    ~MemoryScanSession() {
        WaitForWorker();
    }

    std::mutex mutex;
    std::thread worker;
    bool running{};
    bool cancel{};
    bool cancelled{};
    u64 done{};
    u64 total{};
    u64 generation{};
    std::vector<VAddr> results;
};

/// Reads memory through the application process, guarding against shutdown races.
bool ReadGuest(Core::System& system, VAddr address, void* data, std::size_t size) {
    auto* process = system.ApplicationProcess();
    if (process == nullptr || system.IsShuttingDown()) {
        return false;
    }
    auto& memory = process->GetMemory();
    if (!memory.IsValidVirtualAddressRange(Common::ProcessAddress(address), size)) {
        return false;
    }
    return memory.ReadBlock(Common::ProcessAddress(address), data, size);
}

bool WriteGuest(Core::System& system, VAddr address, const void* data, std::size_t size) {
    auto* process = system.ApplicationProcess();
    if (process == nullptr || system.IsShuttingDown()) {
        return false;
    }
    auto& memory = process->GetMemory();
    if (!memory.IsValidVirtualAddressRange(Common::ProcessAddress(address), size)) {
        return false;
    }
    if (!memory.WriteBlock(Common::ProcessAddress(address), data, size)) {
        return false;
    }
    // Writing into the code region must invalidate the host instruction cache, otherwise the
    // guest keeps executing stale translations.
    Core::InvalidateInstructionCacheRange(process, address, size);
    return true;
}

Tools::ScanParams ParseScanParams(const std::string& json, bool& ok) {
    Tools::ScanParams params;
    ok = false;

    nlohmann::json root;
    try {
        root = nlohmann::json::parse(json);
    } catch (const nlohmann::json::exception& e) {
        LOG_ERROR(Frontend, "Memory scan: bad parameters: {}", e.what());
        return params;
    }
    if (!root.is_object()) {
        return params;
    }

    try {
        params.begin = std::stoull(root.value("begin", std::string{"0"}), nullptr, 16);
        params.size = std::stoull(root.value("size", std::string{"0"}), nullptr, 16);
    } catch (const std::exception&) {
        return params;
    }

    if (!Tools::MemoryScanner::ParseScanType(root.value("type", std::string{"u32"}), params.type)) {
        params.type = Tools::ScanType::U32;
    }

    try {
        params.value = std::stoull(root.value("value", std::string{"0"}), nullptr, 16);
    } catch (const std::exception&) {
        params.value = 0;
    }

    params.epsilon = root.value("epsilon", 0.001);

    if (root.contains("pattern") && root["pattern"].is_string()) {
        const auto hex = root["pattern"].get<std::string>();
        if (hex.size() % 2 != 0) {
            return params;
        }
        params.pattern.reserve(hex.size() / 2);
        for (std::size_t i = 0; i + 1 < hex.size(); i += 2) {
            const auto byte = hex.substr(i, 2);
            if (byte == "??") {
                params.pattern.push_back(0);
                params.aob_mask.push_back(true);
                continue;
            }
            try {
                params.pattern.push_back(static_cast<u8>(std::stoul(byte, nullptr, 16)));
            } catch (const std::exception&) {
                return params;
            }
            params.aob_mask.push_back(false);
        }
    }

    ok = params.size != 0;
    return params;
}

std::string DumpFreezeEntries(Core::System& system) {
    auto* freezer = system.GetFreezer();
    if (freezer == nullptr) {
        return "[]";
    }

    const auto entries = freezer->GetEntries();

    nlohmann::json out = nlohmann::json::array();
    for (const auto& entry : entries) {
        // Report an absolute address to the frontend; the stored value may be a region offset.
        const VAddr absolute = AbsoluteAddressOf(system, entry);

        u64 current{};
        const bool has_current = Tools::MemoryScanner::ReadValue(
            [&system](VAddr address, void* data, std::size_t size) {
                return ReadGuest(system, address, data, size);
            },
            entry.type, absolute, current);

        auto item = nlohmann::json::object();
        item["address"] = fmt::format("{:016X}", absolute);
        item["type"] = std::string{Tools::MemoryScanner::NameOf(entry.type)};
        item["value"] = fmt::format("{:016X}", entry.value);
        item["current"] = has_current ? fmt::format("{:016X}", current) : std::string{};
        item["width"] = entry.width;
        item["enabled"] = entry.enabled;
        item["description"] = entry.description;
        out.push_back(std::move(item));
    }
    return out.dump();
}

} // Anonymous namespace

extern "C" {

jstring Java_org_citron_citron_1emu_NativeLibrary_memGetRegions(JNIEnv* env, jobject jobj) {
    auto& system = EmulationSession::GetInstance().System();
    const auto* process = system.ApplicationProcess();
    if (process == nullptr) {
        return Common::Android::ToJString(env, "[]");
    }

    const auto& page_table = process->GetPageTable();

    nlohmann::json regions = nlohmann::json::array();
    const auto add = [&regions](std::string_view name, u64 base, u64 size) {
        if (size == 0) {
            return;
        }
        nlohmann::json item = nlohmann::json::object();
        item["name"] = std::string{name};
        item["base"] = fmt::format("{:016X}", base);
        item["size"] = fmt::format("{:016X}", size);
        regions.push_back(std::move(item));
    };

    add("main", GetInteger(page_table.GetAliasCodeRegionStart()),
        page_table.GetAliasCodeRegionSize());
    add("heap", GetInteger(page_table.GetHeapRegionStart()), page_table.GetHeapRegionSize());
    add("alias", GetInteger(page_table.GetAliasRegionStart()), page_table.GetAliasRegionSize());

    return Common::Android::ToJString(env, regions.dump());
}

jint Java_org_citron_citron_1emu_NativeLibrary_memScanStart(JNIEnv* env, jobject jobj,
                                                            jstring jparams) {
    bool ok = false;
    const auto params = ParseScanParams(Common::Android::GetJString(env, jparams), ok);
    if (!ok) {
        return ResultInvalidArgument;
    }
    auto& system = EmulationSession::GetInstance().System();
    return MemoryScanSession::GetInstance().Start(system, params);
}

jlongArray Java_org_citron_citron_1emu_NativeLibrary_memScanPoll(JNIEnv* env, jobject jobj) {
    const auto poll = MemoryScanSession::GetInstance().Poll();
    jlongArray result = env->NewLongArray(static_cast<jsize>(poll.size()));
    if (result == nullptr) {
        return nullptr;
    }
    env->SetLongArrayRegion(result, 0, static_cast<jsize>(poll.size()), poll.data());
    return result;
}

void Java_org_citron_citron_1emu_NativeLibrary_memScanCancel(JNIEnv* env, jobject jobj) {
    MemoryScanSession::GetInstance().Cancel();
}

jint Java_org_citron_citron_1emu_NativeLibrary_memScanCount(JNIEnv* env, jobject jobj) {
    return static_cast<jint>(MemoryScanSession::GetInstance().Count());
}

jlongArray Java_org_citron_citron_1emu_NativeLibrary_memScanResults(JNIEnv* env, jobject jobj,
                                                                    jint joffset, jint jcount) {
    const auto count = std::min<jint>(jcount, MaxTransferCount);
    const auto slice = MemoryScanSession::GetInstance().Snapshot(joffset, count);

    jlongArray result = env->NewLongArray(static_cast<jsize>(slice.size()));
    if (result == nullptr) {
        return nullptr;
    }
    if (!slice.empty()) {
        std::vector<jlong> converted;
        converted.reserve(slice.size());
        for (const auto address : slice) {
            converted.push_back(static_cast<jlong>(address));
        }
        env->SetLongArrayRegion(result, 0, static_cast<jsize>(converted.size()), converted.data());
    }
    return result;
}

jint Java_org_citron_citron_1emu_NativeLibrary_memScanRefine(JNIEnv* env, jobject jobj,
                                                             jstring jparams) {
    const auto json = Common::Android::GetJString(env, jparams);

    nlohmann::json root;
    try {
        root = nlohmann::json::parse(json);
    } catch (const nlohmann::json::exception&) {
        return ResultInvalidArgument;
    }
    if (!root.is_object()) {
        return ResultInvalidArgument;
    }

    Tools::ScanType type{};
    if (!Tools::MemoryScanner::ParseScanType(root.value("type", std::string{"u32"}), type)) {
        return ResultInvalidArgument;
    }
    if (!Tools::MemoryScanner::IsFixedWidth(type)) {
        // A pattern has no scalar value, so change comparisons are undefined for it.
        return ResultInvalidArgument;
    }

    Tools::RefineType compare{};
    if (!Tools::MemoryScanner::ParseRefineType(root.value("compare", std::string{"exact"}),
                                               compare)) {
        return ResultInvalidArgument;
    }

    u64 value{};
    try {
        value = std::stoull(root.value("value", std::string{"0"}), nullptr, 16);
    } catch (const std::exception&) {
        value = 0;
    }
    const f64 epsilon = root.value("epsilon", 0.001);

    auto& system = EmulationSession::GetInstance().System();
    return MemoryScanSession::GetInstance().Refine(system, type, compare, value, epsilon);
}

void Java_org_citron_citron_1emu_NativeLibrary_memScanClear(JNIEnv* env, jobject jobj) {
    MemoryScanSession::GetInstance().Clear();
}

jbyteArray Java_org_citron_citron_1emu_NativeLibrary_memReadBytes(JNIEnv* env, jobject jobj,
                                                                  jlong jaddress, jint jsize) {
    if (jsize <= 0 || jsize > MaxReadBytes) {
        return nullptr;
    }

    auto& system = EmulationSession::GetInstance().System();
    std::vector<u8> buffer(static_cast<std::size_t>(jsize));
    if (!ReadGuest(system, static_cast<VAddr>(jaddress), buffer.data(), buffer.size())) {
        return nullptr;
    }

    jbyteArray result = env->NewByteArray(jsize);
    if (result == nullptr) {
        return nullptr;
    }
    env->SetByteArrayRegion(result, 0, jsize,
                            reinterpret_cast<const jbyte*>(buffer.data()));
    return result;
}

jboolean Java_org_citron_citron_1emu_NativeLibrary_memWriteBytes(JNIEnv* env, jobject jobj,
                                                                 jlong jaddress, jbyteArray jdata) {
    if (jdata == nullptr) {
        return JNI_FALSE;
    }
    const auto size = env->GetArrayLength(jdata);
    if (size <= 0 || size > MaxReadBytes) {
        return JNI_FALSE;
    }

    std::vector<u8> buffer(static_cast<std::size_t>(size));
    env->GetByteArrayRegion(jdata, 0, size, reinterpret_cast<jbyte*>(buffer.data()));

    auto& session = EmulationSession::GetInstance();
    const auto lock = session.AcquireSessionLock();
    auto& system = session.System();
    return WriteGuest(system, static_cast<VAddr>(jaddress), buffer.data(), buffer.size())
               ? JNI_TRUE
               : JNI_FALSE;
}

jboolean Java_org_citron_citron_1emu_NativeLibrary_memFreezeSet(JNIEnv* env, jobject jobj,
                                                                jlong jaddress, jstring jtype,
                                                                jlong jvalue, jstring jdescription,
                                                                jboolean jenabled) {
    auto& session = EmulationSession::GetInstance();
    const auto lock = session.AcquireSessionLock();
    auto& system = session.System();

    auto* freezer = system.GetFreezer();
    if (freezer == nullptr) {
        return JNI_FALSE;
    }

    Tools::ScanType type{};
    if (!Tools::MemoryScanner::ParseScanType(Common::Android::GetJString(env, jtype), type)) {
        return JNI_FALSE;
    }
    if (!Tools::MemoryScanner::IsFixedWidth(type)) {
        return JNI_FALSE;
    }

    const auto address = static_cast<VAddr>(jaddress);
    const auto value = static_cast<u64>(jvalue);
    const auto description = Common::Android::GetJString(env, jdescription);

    freezer->FreezeAt(address, type, value, description);

    // Anchor the entry to the region that contains it, so a saved table can be relocated onto
    // a different address space layout next boot.
    VAddr base = 0;
    const auto region = RegionContaining(system, address, base);
    freezer->SetEntryRegion(address, region, system.GetFreezeRegionBases());

    freezer->SetEntryEnabled(address, jenabled != 0);
    freezer->SetActive(true);
    return JNI_TRUE;
}

void Java_org_citron_citron_1emu_NativeLibrary_memFreezeRemove(JNIEnv* env, jobject jobj,
                                                               jlong jaddress) {
    auto& system = EmulationSession::GetInstance().System();
    auto* freezer = system.GetFreezer();
    if (freezer == nullptr) {
        return;
    }

    // The frontend always speaks in absolute addresses, but the entry may store a region
    // offset, so the lookup has to go through the entry list.
    const auto requested = static_cast<VAddr>(jaddress);
    for (const auto& entry : freezer->GetEntries()) {
        if (AbsoluteAddressOf(system, entry) == requested) {
            freezer->Unfreeze(entry.address);
            return;
        }
    }
}

void Java_org_citron_citron_1emu_NativeLibrary_memFreezeClear(JNIEnv* env, jobject jobj) {
    auto& system = EmulationSession::GetInstance().System();
    if (auto* freezer = system.GetFreezer(); freezer != nullptr) {
        freezer->Clear();
    }
}

jstring Java_org_citron_citron_1emu_NativeLibrary_memFreezeList(JNIEnv* env, jobject jobj) {
    auto& system = EmulationSession::GetInstance().System();
    return Common::Android::ToJString(env, DumpFreezeEntries(system));
}

jboolean Java_org_citron_citron_1emu_NativeLibrary_memTableSave(JNIEnv* env, jobject jobj,
                                                                jstring jprogramId) {
    auto& session = EmulationSession::GetInstance();
    const auto lock = session.AcquireSessionLock();
    auto& system = session.System();

    auto* freezer = system.GetFreezer();
    if (freezer == nullptr) {
        return JNI_FALSE;
    }

    const auto program_id = EmulationSession::GetProgramId(env, jprogramId);
    if (program_id == 0) {
        return JNI_FALSE;
    }

    // Entries are already anchored when they were created, and the anchor points at the base of
    // this session's region, so saving does not need to (and must not) rebase them again.
    return freezer->SaveToDisk(program_id) ? JNI_TRUE : JNI_FALSE;
}

jboolean Java_org_citron_citron_1emu_NativeLibrary_memTableLoad(JNIEnv* env, jobject jobj,
                                                                jstring jprogramId) {
    auto& session = EmulationSession::GetInstance();
    const auto lock = session.AcquireSessionLock();
    auto& system = session.System();

    auto* freezer = system.GetFreezer();
    if (freezer == nullptr) {
        return JNI_FALSE;
    }

    const auto program_id = EmulationSession::GetProgramId(env, jprogramId);
    if (program_id == 0) {
        return JNI_FALSE;
    }

    if (!freezer->LoadFromDisk(program_id, system.GetFreezeRegionBases())) {
        return JNI_FALSE;
    }
    if (!freezer->GetEntries().empty()) {
        freezer->SetActive(true);
    }
    return JNI_TRUE;
}

} // extern "C"
