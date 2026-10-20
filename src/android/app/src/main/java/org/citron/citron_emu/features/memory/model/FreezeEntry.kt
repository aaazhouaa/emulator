// SPDX-FileCopyrightText: Copyright 2025 Citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

package org.citron.citron_emu.features.memory.model

/** One row of the frozen address table, mirroring `Tools::Freezer::Entry`. */
data class FreezeEntry(
    val address: Long,
    val type: MemValueType,
    /** The value the freezer keeps writing, as a raw bit pattern. */
    val value: Long,
    /** The live value in guest memory, or null when the address is no longer mapped. */
    val current: Long?,
    val width: Int,
    val enabled: Boolean,
    val description: String
) {
    val addressText: String get() = "0x%016X".format(address)

    fun typeText(): String = type.id

    /** Formats a raw bit pattern according to the entry's type. */
    fun formatValue(bits: Long?): String {
        if (bits == null) {
            return "—"
        }
        return when (type) {
            MemValueType.U8 -> (bits and 0xFF).toString()
            MemValueType.U16 -> (bits and 0xFFFF).toString()
            MemValueType.U32 -> (bits and 0xFFFFFFFFL).toString()
            MemValueType.U64 -> bits.toULong().toString()
            MemValueType.S8 -> (bits and 0xFF).toByte().toString()
            MemValueType.S16 -> (bits and 0xFFFF).toShort().toString()
            MemValueType.S32 -> (bits and 0xFFFFFFFFL).toInt().toString()
            MemValueType.S64 -> bits.toString()
            MemValueType.F32 -> Float.fromBits((bits and 0xFFFFFFFFL).toInt()).toString()
            MemValueType.F64 -> Double.fromBits(bits).toString()
            MemValueType.STRING, MemValueType.AOB -> bits.toString()
        }
    }

    val valueText: String get() = formatValue(value)
    val currentText: String get() = formatValue(current)

    companion object {
        fun fromJson(json: String): List<FreezeEntry> {
            val entries = mutableListOf<FreezeEntry>()
            val array = runCatching { org.json.JSONArray(json) }.getOrNull() ?: return entries
            for (i in 0 until array.length()) {
                val item = array.optJSONObject(i) ?: continue
                val address = item.optString("address", "0").toLongOrNull(16) ?: continue
                val type = MemValueType.fromId(item.optString("type", "u32"))
                val value = item.optString("value", "0").toLongOrNull(16) ?: 0L
                val currentText = item.optString("current", "")
                val current = if (currentText.isEmpty()) {
                    null
                } else {
                    currentText.toLongOrNull(16)
                }
                entries.add(
                    FreezeEntry(
                        address = address,
                        type = type,
                        value = value,
                        current = current,
                        width = item.optInt("width", type.width),
                        enabled = item.optBoolean("enabled", true),
                        description = item.optString("description", "")
                    )
                )
            }
            return entries
        }
    }
}
