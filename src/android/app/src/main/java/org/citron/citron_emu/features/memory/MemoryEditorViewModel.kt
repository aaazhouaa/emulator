// SPDX-FileCopyrightText: Copyright 2025 Citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

package org.citron.citron_emu.features.memory

import android.app.Application
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.citron.citron_emu.NativeLibrary
import org.citron.citron_emu.R
import org.citron.citron_emu.features.memory.model.FreezeEntry
import org.citron.citron_emu.features.memory.model.MemCompare
import org.citron.citron_emu.features.memory.model.MemRegion
import org.citron.citron_emu.features.memory.model.MemValueType
import org.json.JSONObject

/**
 * Drives the memory editor: region discovery, scanning, refinement, and the frozen address table.
 *
 * All native calls are made on [Dispatchers.IO]. The scan itself runs on a native worker thread,
 * so this class only polls for progress and never blocks waiting for it.
 */
class MemoryEditorViewModel(application: Application) : AndroidViewModel(application) {

    private val context = application.applicationContext

    sealed interface ScanStatus {
        data object Idle : ScanStatus
        data class Running(val done: Long, val total: Long) : ScanStatus
        data class Done(val count: Int) : ScanStatus
        data object Cancelled : ScanStatus
        data class Failed(val message: String) : ScanStatus
    }

    private val _regions = MutableStateFlow<List<MemRegion>>(emptyList())
    val regions = _regions.asStateFlow()

    private val _status = MutableStateFlow<ScanStatus>(ScanStatus.Idle)
    val status = _status.asStateFlow()

    /** Results of the current scan, loaded lazily in pages by the list adapter. */
    private val _results = MutableStateFlow<List<Long>>(emptyList())
    val results = _results.asStateFlow()

    private val _freezeEntries = MutableStateFlow<List<FreezeEntry>>(emptyList())
    val freezeEntries = _freezeEntries.asStateFlow()

    private val _message = MutableStateFlow<String?>(null)
    val message = _message.asStateFlow()

    private var pollJob: Job? = null

    /** Last value type picked by the user, so reopening the editor keeps it. */
    var selectedTypeId: String? = null

    fun consumeMessage() {
        _message.value = null
    }

    fun notify(message: String) {
        _message.value = message
    }

    /** Reads the live value at an address so the add-to-table dialog can be pre-filled. */
    fun readValue(address: Long, type: MemValueType): Long? {
        if (type.isPattern) {
            return null
        }
        val bytes = NativeLibrary.memReadBytes(address, type.width) ?: return null
        if (bytes.size < type.width) {
            return null
        }
        var bits = 0L
        for (i in bytes.indices) {
            bits = bits or ((bytes[i].toLong() and 0xFF) shl (8 * i))
        }
        return bits
    }

    fun loadRegions() {
        viewModelScope.launch {
            val parsed = withContext(Dispatchers.IO) {
                MemRegion.fromJson(NativeLibrary.memGetRegions())
            }
            _regions.value = parsed
        }
    }

    fun refreshFreezeEntries() {
        viewModelScope.launch {
            val entries = withContext(Dispatchers.IO) {
                FreezeEntry.fromJson(NativeLibrary.memFreezeList())
            }
            _freezeEntries.value = entries
        }
    }

    /**
     * Starts a first scan.
     *
     * @param value Raw bit pattern of the target value for numeric types.
     * @param pattern Hex byte pattern for [MemValueType.STRING] and [MemValueType.AOB], where
     * `??` marks a wildcard byte.
     */
    fun startScan(
        region: MemRegion,
        type: MemValueType,
        value: Long,
        pattern: String,
        epsilon: Double
    ) {
        if (_status.value is ScanStatus.Running) {
            return
        }
        selectedTypeId = type.id

        val params = JSONObject().apply {
            put("begin", "%016X".format(region.base))
            put("size", "%016X".format(region.size))
            put("type", type.id)
            put("value", "%016X".format(value))
            put("epsilon", epsilon)
            if (type.isPattern) {
                put("pattern", pattern)
            }
        }.toString()

        _message.value = "首次扫描 region=${region.name} base=0x${region.base.toString(16)} size=0x${region.size.toString(16)} type=${type.id}"

        viewModelScope.launch {
            val result = withContext(Dispatchers.IO) { NativeLibrary.memScanStart(params) }
            _message.value = "首次扫描 start 返回码=$result"
            if (result != NativeLibrary.MemScanResult.SUCCESS) {
                _status.value = ScanStatus.Failed(scanErrorMessage(result))
                return@launch
            }
            _results.value = emptyList()
            _status.value = ScanStatus.Running(0, region.size)
            beginPolling()
        }
    }

    fun refine(type: MemValueType, compare: MemCompare, value: Long, epsilon: Double) {
        val params = JSONObject().apply {
            put("type", type.id)
            put("compare", compare.id)
            put("value", "%016X".format(value))
            put("epsilon", epsilon)
        }.toString()

        android.util.Log.d("MemoryEditor", "refine params=$params")
        viewModelScope.launch {
            val countBefore = withContext(Dispatchers.IO) { NativeLibrary.memScanCount() }
            val result = withContext(Dispatchers.IO) { NativeLibrary.memScanRefine(params) }
            val countAfter = withContext(Dispatchers.IO) { NativeLibrary.memScanCount() }
            _message.value = "refine 码=$result 前=$countBefore 后=$countAfter 值=0x${value.toString(16)}"
            if (result != NativeLibrary.MemScanResult.SUCCESS) {
                return@launch
            }
            reloadResults()
        }
    }

    fun cancelScan() {
        viewModelScope.launch(Dispatchers.IO) { NativeLibrary.memScanCancel() }
    }

    fun clearResults() {
        pollJob?.cancel()
        pollJob = null
        viewModelScope.launch {
            withContext(Dispatchers.IO) { NativeLibrary.memScanClear() }
            _results.value = emptyList()
            _status.value = ScanStatus.Idle
        }
    }

    private fun beginPolling() {
        pollJob?.cancel()
        pollJob = viewModelScope.launch {
            while (true) {
                val poll = withContext(Dispatchers.IO) { NativeLibrary.memScanPoll() }
                if (poll.size < 3) {
                    _status.value = ScanStatus.Failed(
                        context.getString(R.string.mem_scan_failed_stopped)
                    )
                    return@launch
                }
                val state = poll[0]
                when (state) {
                    NativeLibrary.MemScanState.RUNNING -> {
                        _status.value = ScanStatus.Running(poll[1], poll[2])
                        delay(POLL_INTERVAL_MS)
                    }

                    NativeLibrary.MemScanState.CANCELLED -> {
                        _status.value = ScanStatus.Cancelled
                        reloadResults()
                        return@launch
                    }

                    else -> {
                        reloadResults()
                        return@launch
                    }
                }
            }
        }
    }

    private suspend fun reloadResults() {
        val count = withContext(Dispatchers.IO) { NativeLibrary.memScanCount() }
        val loaded = mutableListOf<Long>()
        var offset = 0
        while (offset < count) {
            val page = withContext(Dispatchers.IO) {
                NativeLibrary.memScanResults(offset, RESULT_PAGE_SIZE)
            }
            if (page.isEmpty()) {
                // The native list shrank (cleared or re-scanned) while paging.
                break
            }
            page.forEach { loaded.add(it) }
            offset += page.size
        }
        _results.value = loaded
        _status.value = ScanStatus.Done(loaded.size)

        // Tell the user when the scan hit the cap: the candidate list is incomplete and
        // refining it will likely end up empty, so the honest next step is a narrower scan.
        val truncated = withContext(Dispatchers.IO) { NativeLibrary.memScanIsTruncated() }
        if (truncated) {
            _message.value = context.getString(R.string.mem_scan_truncated)
        }
    }

    fun freeze(address: Long, type: MemValueType, value: Long, description: String) {
        viewModelScope.launch {
            withContext(Dispatchers.IO) {
                // Add the entry without enabling the freeze: it writes once, then stays
                // parked until the user toggles the checkbox. Passing true would start the
                // per-frame freezer and make it behave like an always-on freeze.
                NativeLibrary.memFreezeSet(address, type.id, value, description, false)
                NativeLibrary.memFreezeList()
            }.let { FreezeEntry.fromJson(it) }
                .also { _freezeEntries.value = it }
        }
    }

    fun removeFreeze(address: Long) {
        viewModelScope.launch {
            val entries = withContext(Dispatchers.IO) {
                NativeLibrary.memFreezeRemove(address)
                FreezeEntry.fromJson(NativeLibrary.memFreezeList())
            }
            _freezeEntries.value = entries
        }
    }

    fun toggleFreeze(entry: FreezeEntry, enabled: Boolean) {
        viewModelScope.launch {
            val entries = withContext(Dispatchers.IO) {
                NativeLibrary.memFreezeSet(
                    entry.address, entry.type.id, entry.value, entry.description, enabled
                )
                FreezeEntry.fromJson(NativeLibrary.memFreezeList())
            }
            _freezeEntries.value = entries
        }
    }

    fun updateFreezeValue(entry: FreezeEntry, value: Long, description: String) {
        viewModelScope.launch {
            val entries = withContext(Dispatchers.IO) {
                NativeLibrary.memFreezeSet(entry.address, entry.type.id, value, description, entry.enabled)
                FreezeEntry.fromJson(NativeLibrary.memFreezeList())
            }
            _freezeEntries.value = entries
        }
    }

    fun clearFreezes() {
        viewModelScope.launch {
            withContext(Dispatchers.IO) {
                NativeLibrary.memFreezeClear()
                FreezeEntry.fromJson(NativeLibrary.memFreezeList())
            }.also { _freezeEntries.value = it }
        }
    }

    fun saveTable(programId: String) {
        viewModelScope.launch {
            val ok = withContext(Dispatchers.IO) { NativeLibrary.memTableSave(programId) }
            _message.value = context.getString(
                if (ok) R.string.mem_table_saved else R.string.mem_table_save_failed
            )
        }
    }

    fun loadTable(programId: String) {
        viewModelScope.launch {
            val ok = withContext(Dispatchers.IO) { NativeLibrary.memTableLoad(programId) }
            _message.value = context.getString(
                if (ok) R.string.mem_table_loaded else R.string.mem_table_none
            )
            refreshFreezeEntries()
        }
    }

    /** Interprets the text typed by the user as the given type and returns its bit pattern. */
    fun parseInput(type: MemValueType, text: String): Long? {
        val trimmed = text.trim()
        if (trimmed.isEmpty()) {
            return null
        }
        return runCatching {
            when (type) {
                MemValueType.U8 -> trimmed.toUByte().toLong() and 0xFF
                MemValueType.U16 -> trimmed.toUShort().toLong() and 0xFFFF
                MemValueType.U32 -> trimmed.toUInt().toLong() and 0xFFFFFFFFL
                MemValueType.U64 -> trimmed.toULong().toLong()
                MemValueType.S8 -> trimmed.toByte().toLong() and 0xFF
                MemValueType.S16 -> trimmed.toShort().toLong() and 0xFFFF
                MemValueType.S32 -> trimmed.toInt().toLong() and 0xFFFFFFFFL
                MemValueType.S64 -> trimmed.toLong()
                MemValueType.F32 -> trimmed.toFloat().toRawBits().toLong() and 0xFFFFFFFFL
                MemValueType.F64 -> trimmed.toDouble().toRawBits()
                MemValueType.STRING, MemValueType.AOB -> trimmed.toLong(16)
            }
        }.getOrNull()
    }

    private fun scanErrorMessage(result: Int): String = context.getString(
        when (result) {
            NativeLibrary.MemScanResult.NO_PROCESS -> R.string.mem_scan_failed_no_process
            NativeLibrary.MemScanResult.INVALID_ARGUMENT -> R.string.mem_scan_failed_invalid
            NativeLibrary.MemScanResult.ALREADY_RUNNING -> R.string.mem_scan_failed_running
            else -> R.string.mem_scan_failed_generic
        }
    )

    private fun refineErrorMessage(result: Int): String = context.getString(
        when (result) {
            NativeLibrary.MemScanResult.NOTHING_TO_REFINE -> R.string.mem_refine_failed_first
            NativeLibrary.MemScanResult.ALREADY_RUNNING -> R.string.mem_scan_failed_running
            NativeLibrary.MemScanResult.NO_PROCESS -> R.string.mem_scan_failed_no_process
            NativeLibrary.MemScanResult.INVALID_ARGUMENT -> R.string.mem_refine_failed_type
            else -> R.string.mem_refine_failed_generic
        }
    )

    override fun onCleared() {
        pollJob?.cancel()
        // Only flips a native flag; the scan result is unreachable once this view model is gone,
        // so the worker is asked to stop rather than waited on from the main thread.
        NativeLibrary.memScanCancel()
        super.onCleared()
    }

    private companion object {
        const val POLL_INTERVAL_MS = 120L
        const val RESULT_PAGE_SIZE = 20000
    }
}
