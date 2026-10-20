// SPDX-FileCopyrightText: Copyright 2025 Citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstring>
#include <vector>

#include "core/tools/memory_scanner.h"

using namespace Tools;

namespace {

/// Backing store for a fake guest address space: address zero maps to buffer[0].
constexpr VAddr BASE = 0x1000;

struct FakeMemory {
    std::vector<u8> bytes;

    ScanReadFn Reader() const {
        return [this](VAddr address, void* data, std::size_t size) {
            if (address < BASE) {
                return false;
            }
            const auto offset = static_cast<std::size_t>(address - BASE);
            if (offset + size > bytes.size()) {
                return false;
            }
            std::memcpy(data, bytes.data() + offset, size);
            return true;
        };
    }

    void Write(VAddr address, u64 bits, std::size_t width) {
        const auto offset = static_cast<std::size_t>(address - BASE);
        for (std::size_t i = 0; i < width; ++i) {
            bytes[offset + i] = static_cast<u8>((bits >> (8 * i)) & 0xFF);
        }
    }
};

} // Anonymous namespace

TEST_CASE("MemoryScanner::WidthOf", "[core][memory_scanner]") {
    REQUIRE(MemoryScanner::WidthOf(ScanType::U8) == 1);
    REQUIRE(MemoryScanner::WidthOf(ScanType::U16) == 2);
    REQUIRE(MemoryScanner::WidthOf(ScanType::U32) == 4);
    REQUIRE(MemoryScanner::WidthOf(ScanType::U64) == 8);
    REQUIRE(MemoryScanner::WidthOf(ScanType::F32) == 4);
    REQUIRE(MemoryScanner::WidthOf(ScanType::F64) == 8);
    // Pattern scans have no fixed element width.
    REQUIRE(MemoryScanner::WidthOf(ScanType::String) == 0);
    REQUIRE(MemoryScanner::WidthOf(ScanType::Aob) == 0);
    REQUIRE_FALSE(MemoryScanner::IsFixedWidth(ScanType::Aob));
    REQUIRE(MemoryScanner::IsFixedWidth(ScanType::U32));
}

TEST_CASE("MemoryScanner::Scan integral widths", "[core][memory_scanner]") {
    FakeMemory memory;
    memory.bytes.assign(64, 0);

    // u32 1234 at offset 0, u32 1234 at offset 16, non-matching value in between.
    memory.Write(BASE + 0, 1234, 4);
    memory.Write(BASE + 16, 1234, 4);
    memory.Write(BASE + 32, 4321, 4);

    ScanParams params;
    params.begin = BASE;
    params.size = memory.bytes.size();
    params.type = ScanType::U32;
    params.value = 1234;

    const auto results = MemoryScanner::Scan(memory.Reader(), params);
    REQUIRE(results.size() == 2);
    REQUIRE(results[0] == BASE + 0);
    REQUIRE(results[1] == BASE + 16);
}

TEST_CASE("MemoryScanner::Scan respects element alignment", "[core][memory_scanner]") {
    FakeMemory memory;
    memory.bytes.assign(64, 0);

    // Place the u32 pattern at a byte offset that is not a multiple of the element width.
    memory.Write(BASE + 2, 0x11223344, 4);

    ScanParams params;
    params.begin = BASE;
    params.size = memory.bytes.size();
    params.type = ScanType::U32;
    params.value = 0x11223344;

    // A u32 scan steps by 4 bytes, so it must not report the unaligned occurrence.
    REQUIRE(MemoryScanner::Scan(memory.Reader(), params).empty());
}

TEST_CASE("MemoryScanner::Scan signed values", "[core][memory_scanner]") {
    FakeMemory memory;
    memory.bytes.assign(32, 0);

    memory.Write(BASE + 0, static_cast<u64>(static_cast<s32>(-5)) & 0xFFFFFFFF, 4);
    memory.Write(BASE + 4, 5, 4);

    ScanParams params;
    params.begin = BASE;
    params.size = memory.bytes.size();
    params.type = ScanType::S32;
    params.value = static_cast<u64>(static_cast<s32>(-5)) & 0xFFFFFFFF;

    const auto results = MemoryScanner::Scan(memory.Reader(), params);
    REQUIRE(results.size() == 1);
    REQUIRE(results[0] == BASE + 0);
}

TEST_CASE("MemoryScanner::Scan float epsilon", "[core][memory_scanner]") {
    FakeMemory memory;
    memory.bytes.assign(32, 0);

    f32 one_point_zero = 1.0f;
    f32 one_point_nine = 1.9f;
    u32 bits_one{};
    u32 bits_nine{};
    std::memcpy(&bits_one, &one_point_zero, 4);
    std::memcpy(&bits_nine, &one_point_nine, 4);
    memory.Write(BASE + 0, bits_one, 4);
    memory.Write(BASE + 4, bits_nine, 4);

    u32 target_bits{};
    std::memcpy(&target_bits, &one_point_zero, 4);

    ScanParams params;
    params.begin = BASE;
    params.size = memory.bytes.size();
    params.type = ScanType::F32;
    params.value = target_bits;
    // Wide enough to also match the 1.9 sample, and the zeroed tail is within tolerance too.
    params.epsilon = 2.0;

    // The exact match and the 1.9 sample must both be reported, in address order.
    const auto wide = MemoryScanner::Scan(memory.Reader(), params);
    REQUIRE(wide.size() >= 2);
    REQUIRE(wide[0] == BASE + 0);
    REQUIRE(wide[1] == BASE + 4);

    params.epsilon = 0.001;
    const auto narrow = MemoryScanner::Scan(memory.Reader(), params);
    REQUIRE(narrow.size() == 1);
    REQUIRE(narrow[0] == BASE + 0);
}

TEST_CASE("MemoryScanner::Scan string and aob", "[core][memory_scanner]") {
    FakeMemory memory;
    memory.bytes.assign(32, 0xAA);
    const std::array<u8, 4> needle{'T', 'E', 'S', 'T'};
    std::memcpy(memory.bytes.data() + 8, needle.data(), needle.size());

    ScanParams params;
    params.begin = BASE;
    params.size = memory.bytes.size();
    params.type = ScanType::String;
    params.pattern.assign(needle.begin(), needle.end());

    const auto string_hits = MemoryScanner::Scan(memory.Reader(), params);
    REQUIRE(string_hits.size() == 1);
    REQUIRE(string_hits[0] == BASE + 8);

    SECTION("wildcard bytes match anything") {
        ScanParams aob;
        aob.begin = BASE;
        aob.size = memory.bytes.size();
        aob.type = ScanType::Aob;
        // Match the 'TEST' needle but allow any 3rd byte.
        aob.pattern = {'T', 'E', 0x00, 'T'};
        aob.aob_mask = {false, false, true, false};

        const auto aob_hits = MemoryScanner::Scan(memory.Reader(), aob);
        REQUIRE(aob_hits.size() == 1);
        REQUIRE(aob_hits[0] == BASE + 8);
    }

    SECTION("masked-out bytes are not compared") {
        ScanParams aob;
        aob.begin = BASE;
        aob.size = memory.bytes.size();
        aob.type = ScanType::Aob;
        aob.pattern = {0x00, 0x00, 'S', 'T'};
        aob.aob_mask = {true, true, false, false};

        const auto aob_hits = MemoryScanner::Scan(memory.Reader(), aob);
        REQUIRE(aob_hits.size() == 1);
        REQUIRE(aob_hits[0] == BASE + 8);
    }
}

TEST_CASE("MemoryScanner::Scan empty and invalid params", "[core][memory_scanner]") {
    FakeMemory memory;
    memory.bytes.assign(16, 0);
    const auto read = memory.Reader();

    ScanParams params;
    params.begin = BASE;
    params.size = 0;
    params.type = ScanType::U32;
    REQUIRE(MemoryScanner::Scan(read, params).empty());

    // Pattern scan with an empty needle must not report a match at every byte.
    params.size = memory.bytes.size();
    params.type = ScanType::String;
    params.pattern.clear();
    REQUIRE(MemoryScanner::Scan(read, params).empty());

    // A null reader is rejected rather than dereferenced.
    REQUIRE(MemoryScanner::Scan({}, params).empty());
}

TEST_CASE("MemoryScanner::Scan respects max results", "[core][memory_scanner]") {
    FakeMemory memory;
    // Every aligned u32 equals the target; MAX_RESULTS must cap the output.
    const std::size_t count = MemoryScanner::MAX_RESULTS * 2;
    memory.bytes.assign(count * 4, 0);
    for (std::size_t i = 0; i < count; ++i) {
        memory.Write(BASE + i * 4, 7, 4);
    }

    ScanParams params;
    params.begin = BASE;
    params.size = memory.bytes.size();
    params.type = ScanType::U32;
    params.value = 7;

    REQUIRE(MemoryScanner::Scan(memory.Reader(), params).size() == MemoryScanner::MAX_RESULTS);
}

TEST_CASE("MemoryScanner::Scan cancellable progress", "[core][memory_scanner]") {
    FakeMemory memory;
    memory.bytes.assign(16, 0);
    memory.Write(BASE + 0, 99, 4);

    ScanParams params;
    params.begin = BASE;
    params.size = memory.bytes.size();
    params.type = ScanType::U32;
    params.value = 99;

    bool called = false;
    const auto cancel = [&called](u64, u64) {
        called = true;
        return false;
    };

    REQUIRE(MemoryScanner::Scan(memory.Reader(), params, cancel).empty());
    REQUIRE(called);
}

TEST_CASE("MemoryScanner::Refine comparisons", "[core][memory_scanner]") {
    FakeMemory memory;
    memory.bytes.assign(48, 0);

    // Candidates at 0, 4, 8 with values 10, 20, 30.
    memory.Write(BASE + 0, 10, 4);
    memory.Write(BASE + 4, 20, 4);
    memory.Write(BASE + 8, 30, 4);

    const std::vector<VAddr> candidates{BASE + 0, BASE + 4, BASE + 8};
    const std::vector<u64> previous{10, 20, 30};

    SECTION("exact") {
        const auto out = MemoryScanner::Refine(memory.Reader(), candidates, {}, ScanType::U32,
                                               RefineType::Exact, 20);
        REQUIRE(out.size() == 1);
        REQUIRE(out[0] == BASE + 4);
    }

    SECTION("greater / less are unsigned for unsigned types") {
        REQUIRE(MemoryScanner::Refine(memory.Reader(), candidates, {}, ScanType::U32,
                                      RefineType::Greater, 10)
                    .size() == 2);
        REQUIRE(MemoryScanner::Refine(memory.Reader(), candidates, {}, ScanType::U32,
                                      RefineType::Less, 30)
                    .size() == 2);
    }

    SECTION("changed / unchanged need history") {
        // Mutate the middle slot so it differs from the recorded value.
        memory.Write(BASE + 4, 21, 4);

        const auto changed = MemoryScanner::Refine(memory.Reader(), candidates, previous,
                                                   ScanType::U32, RefineType::Changed, 0);
        REQUIRE(changed.size() == 1);
        REQUIRE(changed[0] == BASE + 4);

        const auto unchanged = MemoryScanner::Refine(memory.Reader(), candidates, previous,
                                                     ScanType::U32, RefineType::Unchanged, 0);
        REQUIRE(unchanged.size() == 2);
    }

    SECTION("increased / decreased need history") {
        memory.Write(BASE + 4, 21, 4);
        memory.Write(BASE + 8, 29, 4);

        const auto increased = MemoryScanner::Refine(memory.Reader(), candidates, previous,
                                                     ScanType::U32, RefineType::Increased, 0);
        REQUIRE(increased.size() == 1);
        REQUIRE(increased[0] == BASE + 4);

        const auto decreased = MemoryScanner::Refine(memory.Reader(), candidates, previous,
                                                     ScanType::U32, RefineType::Decreased, 0);
        REQUIRE(decreased.size() == 1);
        REQUIRE(decreased[0] == BASE + 8);
    }

    SECTION("missing history yields an empty result instead of a wrong one") {
        REQUIRE(MemoryScanner::Refine(memory.Reader(), candidates, {}, ScanType::U32,
                                      RefineType::Changed, 0)
                    .empty());
        REQUIRE(MemoryScanner::Refine(memory.Reader(), candidates, {1, 2}, ScanType::U32,
                                      RefineType::Increased, 0)
                    .empty());
    }

    SECTION("pattern types cannot be refined") {
        REQUIRE(MemoryScanner::Refine(memory.Reader(), candidates, {}, ScanType::String,
                                      RefineType::Exact, 0)
                    .empty());
    }

    SECTION("empty candidate list") {
        REQUIRE(MemoryScanner::Refine(memory.Reader(), {}, {}, ScanType::U32, RefineType::Exact, 0)
                    .empty());
    }
}

TEST_CASE("MemoryScanner::Refine skips unreadable candidates", "[core][memory_scanner]") {
    FakeMemory memory;
    memory.bytes.assign(8, 0);
    memory.Write(BASE + 0, 42, 4);

    // Second candidate lies outside the backing store.
    const std::vector<VAddr> candidates{BASE + 0, BASE + 0x10000};
    const auto out = MemoryScanner::Refine(memory.Reader(), candidates, {}, ScanType::U32,
                                           RefineType::Exact, 42);
    REQUIRE(out.size() == 1);
    REQUIRE(out[0] == BASE + 0);
}

TEST_CASE("MemoryScanner::ReadValue", "[core][memory_scanner]") {
    FakeMemory memory;
    memory.bytes.assign(16, 0);
    memory.Write(BASE + 4, 0xABCDEF01u, 4);

    u64 value{};
    REQUIRE(MemoryScanner::ReadValue(memory.Reader(), ScanType::U32, BASE + 4, value));
    REQUIRE(value == 0xABCDEF01u);

    // Pattern types have no scalar value.
    REQUIRE_FALSE(MemoryScanner::ReadValue(memory.Reader(), ScanType::String, BASE + 4, value));
    // Out of range.
    REQUIRE_FALSE(MemoryScanner::ReadValue(memory.Reader(), ScanType::U32, BASE + 0x1000, value));
    // Null reader.
    REQUIRE_FALSE(MemoryScanner::ReadValue({}, ScanType::U32, BASE, value));
}

TEST_CASE("MemoryScanner type name round trip", "[core][memory_scanner]") {
    for (const auto type : {ScanType::U8, ScanType::U16, ScanType::U32, ScanType::U64,
                            ScanType::S8, ScanType::S16, ScanType::S32, ScanType::S64,
                            ScanType::F32, ScanType::F64, ScanType::String, ScanType::Aob}) {
        ScanType parsed{};
        REQUIRE(MemoryScanner::ParseScanType(MemoryScanner::NameOf(type), parsed));
        REQUIRE(parsed == type);
    }
    ScanType unknown{};
    REQUIRE_FALSE(MemoryScanner::ParseScanType("nonsense", unknown));

    for (const auto compare :
         {RefineType::Exact, RefineType::Greater, RefineType::Less, RefineType::Changed,
          RefineType::Unchanged, RefineType::Increased, RefineType::Decreased}) {
        RefineType parsed{};
        REQUIRE(MemoryScanner::ParseRefineType(MemoryScanner::NameOf(compare), parsed));
        REQUIRE(parsed == compare);
    }
    RefineType unknown_compare{};
    REQUIRE_FALSE(MemoryScanner::ParseRefineType("nonsense", unknown_compare));
}

TEST_CASE("MemoryScanner::Scan handles unreadable chunks", "[core][memory_scanner]") {
    // A reader that reports the first chunk as unmapped and everything after as zeroed.
    FakeMemory memory;
    memory.bytes.assign(64, 0);
    memory.Write(BASE + 0, 5, 4);

    std::size_t calls = 0;
    const auto flaky = [&memory, &calls](VAddr address, void* data, std::size_t size) {
        ++calls;
        if (calls == 1) {
            return false;
        }
        return memory.Reader()(address, data, size);
    };

    ScanParams params;
    params.begin = BASE;
    params.size = memory.bytes.size();
    params.type = ScanType::U32;
    params.value = 5;

    // The failed chunk is skipped without aborting the scan or looping forever.
    REQUIRE(MemoryScanner::Scan(flaky, params).empty());
    REQUIRE(calls >= 1);
}
