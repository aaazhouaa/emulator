// SPDX-FileCopyrightText: Copyright 2025 Citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

package org.citron.citron_emu.features.memory.model

/**
 * Value interpretations understood by the native memory scanner.
 *
 * [id] is the name the native side parses, and must match `Tools::MemoryScanner::NameOf`.
 */
enum class MemValueType(val id: String, val width: Int) {
    U8("u8", 1),
    U16("u16", 2),
    U32("u32", 4),
    U64("u64", 8),
    S8("s8", 1),
    S16("s16", 2),
    S32("s32", 4),
    S64("s64", 8),
    F32("f32", 4),
    F64("f64", 8),
    STRING("str", 0),
    AOB("aob", 0);

    val isPattern: Boolean get() = width == 0
    val isFloat: Boolean get() = this == F32 || this == F64

    companion object {
        /** Types offered for a first scan. */
        val scanTypes: List<MemValueType> = entries

        /** Types a frozen entry can use; pattern types cannot be frozen. */
        val freezeTypes: List<MemValueType> = entries.filter { !it.isPattern }

        fun fromId(id: String): MemValueType = entries.firstOrNull { it.id == id } ?: U32
    }
}

/**
 * Comparison used when filtering existing scan results.
 *
 * [id] must match `Tools::MemoryScanner::NameOf(RefineType)`.
 */
enum class MemCompare(val id: String) {
    EXACT("exact"),
    GREATER("greater"),
    LESS("less"),
    CHANGED("changed"),
    UNCHANGED("unchanged"),
    INCREASED("increased"),
    DECREASED("decreased");

    /** True for the comparisons that need the values observed at the previous scan. */
    val needsHistory: Boolean
        get() = this == CHANGED || this == UNCHANGED || this == INCREASED || this == DECREASED
}
