// SPDX-FileCopyrightText: Copyright 2025 Citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/tools/memory_scanner.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <string_view>

namespace Tools {
namespace {

/// Reads one element of the given width out of a raw little-endian byte block
/// and returns it as an unsigned bit pattern.
template <typename T>
u64 LoadBits(const u8* data) {
    T value{};
    std::memcpy(&value, data, sizeof(T));
    if constexpr (sizeof(T) == 1) {
        return static_cast<u64>(value);
    } else if constexpr (std::endian::native == std::endian::little) {
        // Guest memory is little-endian, which is already the host representation.
        return static_cast<u64>(value);
    } else {
        return static_cast<u64>(std::byteswap(value));
    }
}

constexpr std::array<ScanType, 10> FIXED_WIDTH_TYPES{
    ScanType::U8,  ScanType::U16, ScanType::U32, ScanType::U64, ScanType::S8,
    ScanType::S16, ScanType::S32, ScanType::S64, ScanType::F32, ScanType::F64,
};

/// Decodes an element as a signed integer for the signed comparison modes.
s64 ToSigned(u64 bits, std::size_t width) {
    switch (width) {
    case 1:
        return static_cast<s64>(static_cast<s8>(static_cast<u8>(bits)));
    case 2:
        return static_cast<s64>(static_cast<s16>(static_cast<u16>(bits)));
    case 4:
        return static_cast<s64>(static_cast<s32>(static_cast<u32>(bits)));
    case 8:
        return static_cast<s64>(bits);
    default:
        return static_cast<s64>(bits);
    }
}

/// Decodes an element as a floating point value. Only meaningful for F32/F64.
f64 ToFloat(u64 bits, std::size_t width) {
    if (width == 4) {
        f32 value{};
        const auto raw = static_cast<u32>(bits);
        std::memcpy(&value, &raw, sizeof(value));
        return static_cast<f64>(value);
    }
    f64 value{};
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

bool IsFloatType(ScanType type) {
    return type == ScanType::F32 || type == ScanType::F64;
}

bool IsSignedType(ScanType type) {
    return type == ScanType::S8 || type == ScanType::S16 || type == ScanType::S32 ||
           type == ScanType::S64;
}

/// Compares two values of the same element type according to the requested relation.
bool Satisfies(ScanType type, RefineType compare, u64 candidate_bits, u64 target_bits, f64 epsilon) {
    const auto width = MemoryScanner::WidthOf(type);

    if (IsFloatType(type)) {
        const auto candidate = ToFloat(candidate_bits, width);
        const auto target = ToFloat(target_bits, width);
        if (!std::isfinite(candidate) || !std::isfinite(target)) {
            return compare == RefineType::Unchanged && candidate == target;
        }
        switch (compare) {
        case RefineType::Exact:
            return std::fabs(candidate - target) <= epsilon;
        case RefineType::Greater:
            return candidate > target + epsilon;
        case RefineType::Less:
            return candidate < target - epsilon;
        case RefineType::Increased:
            return candidate > target + epsilon;
        case RefineType::Decreased:
            return candidate < target - epsilon;
        case RefineType::Changed:
            return std::fabs(candidate - target) > epsilon;
        case RefineType::Unchanged:
            return std::fabs(candidate - target) <= epsilon;
        }
        return false;
    }

    switch (compare) {
    case RefineType::Exact:
    case RefineType::Unchanged:
        return candidate_bits == target_bits;
    case RefineType::Changed:
        return candidate_bits != target_bits;
    case RefineType::Greater:
        if (IsSignedType(type)) {
            return ToSigned(candidate_bits, width) > ToSigned(target_bits, width);
        }
        return candidate_bits > target_bits;
    case RefineType::Less:
        if (IsSignedType(type)) {
            return ToSigned(candidate_bits, width) < ToSigned(target_bits, width);
        }
        return candidate_bits < target_bits;
    case RefineType::Increased:
        if (IsSignedType(type)) {
            return ToSigned(candidate_bits, width) > ToSigned(target_bits, width);
        }
        return candidate_bits > target_bits;
    case RefineType::Decreased:
        if (IsSignedType(type)) {
            return ToSigned(candidate_bits, width) < ToSigned(target_bits, width);
        }
        return candidate_bits < target_bits;
    }
    return false;
}

u64 LoadElement(const u8* data, std::size_t width) {
    switch (width) {
    case 1:
        return LoadBits<u8>(data);
    case 2:
        return LoadBits<u16>(data);
    case 4:
        return LoadBits<u32>(data);
    case 8:
        return LoadBits<u64>(data);
    default:
        return 0;
    }
}

/// Byte pattern matching shared by String and Aob scans.
/// mask may be empty (String), in which case every byte must match exactly.
bool PatternMatches(const u8* data, const std::vector<u8>& pattern, const std::vector<bool>& mask) {
    const auto size = pattern.size();
    for (std::size_t i = 0; i < size; ++i) {
        if (i < mask.size() && mask[i]) {
            continue;
        }
        if (data[i] != pattern[i]) {
            return false;
        }
    }
    return true;
}

} // Anonymous namespace

std::size_t MemoryScanner::WidthOf(ScanType type) {
    switch (type) {
    case ScanType::U8:
    case ScanType::S8:
        return 1;
    case ScanType::U16:
    case ScanType::S16:
        return 2;
    case ScanType::U32:
    case ScanType::S32:
    case ScanType::F32:
        return 4;
    case ScanType::U64:
    case ScanType::S64:
    case ScanType::F64:
        return 8;
    case ScanType::String:
    case ScanType::Aob:
        return 0;
    }
    return 0;
}

bool MemoryScanner::IsFixedWidth(ScanType type) {
    return WidthOf(type) != 0;
}

std::vector<VAddr> MemoryScanner::Scan(const ScanReadFn& read, const ScanParams& params,
                                       const ScanProgressFn& progress) {
    std::vector<VAddr> results;
    if (!read || params.size == 0) {
        return results;
    }

    const bool pattern_scan =
        params.type == ScanType::String || params.type == ScanType::Aob;
    const std::size_t width = pattern_scan ? params.pattern.size() : WidthOf(params.type);
    if (width == 0 || width > CHUNK_SIZE) {
        return results;
    }
    if (!pattern_scan && !IsFixedWidth(params.type)) {
        return results;
    }

    const VAddr end = params.begin + params.size;
    const u64 total = params.size;
    const bool can_cancel = static_cast<bool>(progress);

    std::vector<u8> block(CHUNK_SIZE + width);

    for (VAddr cursor = params.begin; cursor < end;) {
        if (can_cancel && !progress(cursor - params.begin, total)) {
            break;
        }
        if (results.size() >= MAX_RESULTS) {
            break;
        }

        const u64 remaining = end - cursor;
        const u64 usable = std::min<u64>(remaining, CHUNK_SIZE);
        // Over-read by one element so a value straddling the chunk boundary is not missed.
        // Only safe when more chunk remains behind: at the tail of the region the over-read
        // would leave the mapped range and fail the whole block.
        const std::size_t want = static_cast<std::size_t>(
            remaining > CHUNK_SIZE ? usable + width : usable);

        if (!read(cursor, block.data(), want)) {
            // Skip the whole chunk on unreadable memory; advance by exactly one chunk so
            // progress always moves forward and the loop terminates.
            cursor += usable;
            continue;
        }

        if (pattern_scan) {
            for (u64 offset = 0; offset + width <= usable; ++offset) {
                if (PatternMatches(block.data() + offset, params.pattern, params.aob_mask)) {
                    results.push_back(cursor + offset);
                    if (results.size() >= MAX_RESULTS) {
                        break;
                    }
                }
            }
        } else {
            for (u64 offset = 0; offset + width <= usable; offset += width) {
                const auto bits = LoadElement(block.data() + offset, width);
                if (Satisfies(params.type, RefineType::Exact, bits, params.value,
                              params.epsilon)) {
                    results.push_back(cursor + offset);
                    if (results.size() >= MAX_RESULTS) {
                        break;
                    }
                }
            }
        }

        cursor += usable;
    }

    return results;
}

std::vector<VAddr> MemoryScanner::Refine(const ScanReadFn& read, std::vector<VAddr> candidates,
                                         const std::vector<u64>& previous_values, ScanType type,
                                         RefineType compare, u64 value, f64 epsilon) {
    std::vector<VAddr> results;
    if (!read || candidates.empty()) {
        return results;
    }

    const std::size_t width = WidthOf(type);
    if (width == 0) {
        // String/Aob refinement is not defined: a pattern has no single value to compare.
        return results;
    }

    const bool needs_history = compare == RefineType::Changed || compare == RefineType::Unchanged ||
                               compare == RefineType::Increased || compare == RefineType::Decreased;
    if (needs_history && previous_values.size() != candidates.size()) {
        return results;
    }

    results.reserve(candidates.size());

    std::array<u8, 8> buffer{};
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        const auto address = candidates[i];
        if (!read(address, buffer.data(), width)) {
            continue;
        }
        const auto bits = LoadElement(buffer.data(), width);
        const auto reference = needs_history ? previous_values[i] : value;
        if (Satisfies(type, compare, bits, reference, epsilon)) {
            results.push_back(address);
        }
    }

    return results;
}

bool MemoryScanner::ReadValue(const ScanReadFn& read, ScanType type, VAddr address, u64& out) {
    const std::size_t width = WidthOf(type);
    if (!read || width == 0) {
        return false;
    }

    std::array<u8, 8> buffer{};
    if (!read(address, buffer.data(), width)) {
        return false;
    }
    out = LoadElement(buffer.data(), width);
    return true;
}

std::string_view MemoryScanner::NameOf(ScanType type) {
    switch (type) {
    case ScanType::U8:
        return "u8";
    case ScanType::U16:
        return "u16";
    case ScanType::U32:
        return "u32";
    case ScanType::U64:
        return "u64";
    case ScanType::S8:
        return "s8";
    case ScanType::S16:
        return "s16";
    case ScanType::S32:
        return "s32";
    case ScanType::S64:
        return "s64";
    case ScanType::F32:
        return "f32";
    case ScanType::F64:
        return "f64";
    case ScanType::String:
        return "str";
    case ScanType::Aob:
        return "aob";
    }
    return "u32";
}

std::string_view MemoryScanner::NameOf(RefineType type) {
    switch (type) {
    case RefineType::Exact:
        return "exact";
    case RefineType::Greater:
        return "greater";
    case RefineType::Less:
        return "less";
    case RefineType::Changed:
        return "changed";
    case RefineType::Unchanged:
        return "unchanged";
    case RefineType::Increased:
        return "increased";
    case RefineType::Decreased:
        return "decreased";
    }
    return "exact";
}

bool MemoryScanner::ParseScanType(std::string_view name, ScanType& out) {
    for (const auto candidate : FIXED_WIDTH_TYPES) {
        if (NameOf(candidate) == name) {
            out = candidate;
            return true;
        }
    }
    if (name == NameOf(ScanType::String)) {
        out = ScanType::String;
        return true;
    }
    if (name == NameOf(ScanType::Aob)) {
        out = ScanType::Aob;
        return true;
    }
    return false;
}

bool MemoryScanner::ParseRefineType(std::string_view name, RefineType& out) {
    constexpr std::array<RefineType, 7> all{
        RefineType::Exact,  RefineType::Greater,   RefineType::Less,     RefineType::Changed,
        RefineType::Unchanged, RefineType::Increased, RefineType::Decreased,
    };
    for (const auto candidate : all) {
        if (NameOf(candidate) == name) {
            out = candidate;
            return true;
        }
    }
    return false;
}

} // namespace Tools
