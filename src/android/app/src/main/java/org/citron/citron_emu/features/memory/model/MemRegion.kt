// SPDX-FileCopyrightText: Copyright 2025 Citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

package org.citron.citron_emu.features.memory.model

import org.json.JSONArray
import org.json.JSONObject

/**
 * A guest memory region reported by the native side.
 *
 * Base and size arrive as hex strings because they are 64-bit values that Kotlin's JSON
 * would otherwise round-trip through a Double.
 */
data class MemRegion(
    val name: String,
    val base: Long,
    val size: Long
) {
    val end: Long get() = base + size

    companion object {
        fun fromJson(json: String): List<MemRegion> {
            val regions = mutableListOf<MemRegion>()
            val array = runCatching { JSONArray(json) }.getOrNull() ?: return regions
            for (i in 0 until array.length()) {
                val item = array.optJSONObject(i) ?: continue
                val name = item.optString("name", "")
                val base = item.optString("base", "0").toLongOrNull(16) ?: continue
                val size = item.optString("size", "0").toLongOrNull(16) ?: 0L
                if (name.isEmpty() || size <= 0) {
                    continue
                }
                regions.add(MemRegion(name, base, size))
            }
            return regions
        }
    }
}
