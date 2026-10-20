// SPDX-FileCopyrightText: Copyright 2025 Citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <functional>
#include <string>
#include <vector>

#include "common/common_types.h"

namespace Tools {

/// Value width/interpretation used when scanning or comparing guest memory.
/// The numeric order is part of the frontend contract: it is passed across the JNI
/// boundary as an integer ordinal, so entries must only ever be appended.
enum class ScanType : u8 {
    U8 = 0,
    U16 = 1,
    U32 = 2,
    U64 = 3,
    S8 = 4,
    S16 = 5,
    S32 = 6,
    S64 = 7,
    F32 = 8,
    F64 = 9,
    String = 10,
    Aob = 11,
};

/// Comparison applied when refining an existing candidate list against current memory.
enum class RefineType : u8 {
    Exact = 0,
    Greater = 1,
    Less = 2,
    Changed = 3,
    Unchanged = 4,
    Increased = 5,
    Decreased = 6,
};

/// Returns false from the progress callback to request cancellation.
using ScanProgressFn = std::function<bool(u64 done, u64 total)>;

/// Reads guest memory. Returns false when the range is not readable;
/// the scanner then treats the block as absent rather than as zeroes.
using ScanReadFn = std::function<bool(VAddr address, void* data, std::size_t size)>;

struct ScanParams {
    VAddr begin{};
    u64 size{};

    ScanType type{ScanType::U32};

    /// Target value for the integral types and for RefineType::Greater/Less.
    /// Signed types interpret this field as its two's complement bit pattern.
    u64 value{};

    /// Absolute tolerance used by F32/F64 matching.
    f64 epsilon{0.001};

    /// Raw bytes for ScanType::String and ScanType::Aob.
    std::vector<u8> pattern;

    /// One flag per pattern byte for ScanType::Aob. true means "any byte matches".
    /// Ignored for every other type.
    std::vector<bool> aob_mask;
};

/// Stateless guest memory scanner.
///
/// Memory access is injected through ScanReadFn so the algorithm can be exercised
/// against a plain host buffer without a running emulated process.
class MemoryScanner {
public:
    /// Upper bound on retained candidates, to keep the public result list bounded.
    /// Must be large enough that a 1-byte scan over a ~1 GB code region does not truncate
    /// before the real target is reached (1/256 hit rate would otherwise clip at a few
    /// hundred thousand and make "next scan" return zero).
    static constexpr std::size_t MAX_RESULTS = 5000000;

    /// Number of bytes read per step. Must stay a multiple of 8 so that every
    /// scan width can be stepped without straddling a chunk boundary.
    static constexpr std::size_t CHUNK_SIZE = 0x100000;

    /// Width in bytes of a single element for the given type.
    /// Returns 0 for String/Aob, which have no fixed width.
    [[nodiscard]] static std::size_t WidthOf(ScanType type);

    /// True for the fixed-width value types (everything except String/Aob).
    [[nodiscard]] static bool IsFixedWidth(ScanType type);

    /// First scan: returns every address in [begin, begin+size) whose current value
    /// satisfies the condition described by params.
    [[nodiscard]] static std::vector<VAddr> Scan(const ScanReadFn& read, const ScanParams& params,
                                                 const ScanProgressFn& progress = {});

    /// Second scan: filters candidates in place against current memory.
    /// For Changed/Unchanged/Increased/Decreased the caller must supply the values that were
    /// observed when the candidates were produced, via previous_values (same order as
    /// candidates). Those four comparisons return an empty result if previous_values is
    /// missing or mis-sized, since the comparison is undefined without it.
    [[nodiscard]] static std::vector<VAddr> Refine(const ScanReadFn& read,
                                                    std::vector<VAddr> candidates,
                                                    const std::vector<u64>& previous_values,
                                                    ScanType type, RefineType compare,
                                                    u64 value, f64 epsilon = 0.001);

    /// Reads the current value of a single element. Returns false if unreadable.
    [[nodiscard]] static bool ReadValue(const ScanReadFn& read, ScanType type, VAddr address,
                                        u64& out);

    /// Human readable name, used for logging and for the persisted address table.
    [[nodiscard]] static std::string_view NameOf(ScanType type);
    [[nodiscard]] static std::string_view NameOf(RefineType type);

    /// Parses the names produced by NameOf. Returns false on an unknown name.
    [[nodiscard]] static bool ParseScanType(std::string_view name, ScanType& out);
    [[nodiscard]] static bool ParseRefineType(std::string_view name, RefineType& out);
};

} // namespace Tools
