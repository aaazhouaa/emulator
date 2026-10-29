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

    /**
     * Size of the current scan result set, plus a serial that changes on every refresh.
     *
     * The addresses themselves are never held here: a first scan can yield millions of them, so
     * the list adapter pulls pages straight from native and keeps only a small window resident.
     *
     * [serial] exists because a `StateFlow` conflates equal values: refining can leave the count
     * unchanged while replacing every address behind it, and the adapter would then keep showing
     * the stale pages. Bumping the serial guarantees the refresh is delivered.
     */
    data class ResultWindow(val count: Int = 0, val serial: Long = 0)

    private val _resultWindow = MutableStateFlow(ResultWindow())
    val resultWindow = _resultWindow.asStateFlow()
    private var resultSerial = 0L

    /** Publishes a new result set size, forcing delivery even when [count] is unchanged. */
    private fun publishResultWindow(count: Int) {
        _resultWindow.value = ResultWindow(count, ++resultSerial)
    }

    private val _freezeEntries = MutableStateFlow<List<FreezeEntry>>(emptyList())
    val freezeEntries = _freezeEntries.asStateFlow()

    private var pollJob: Job? = null

    /** Last value type picked by the user, so reopening the editor keeps it. */
    var selectedTypeId: String? = null

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

        viewModelScope.launch {
            val result = withContext(Dispatchers.IO) { NativeLibrary.memScanStart(params) }
            if (result != NativeLibrary.MemScanResult.SUCCESS) {
                _status.value = ScanStatus.Failed(scanErrorMessage(result))
                return@launch
            }
            publishResultWindow(0)
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

        viewModelScope.launch {
            val result = withContext(Dispatchers.IO) { NativeLibrary.memScanRefine(params) }
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
            publishResultWindow(0)
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
        publishResultWindow(count)
        _status.value = ScanStatus.Done(count)
    }

    /**
     * Copies one page of the current scan results out of native.
     *
     * Called from the adapter's background loader, never from a binding pass: the native scan
     * session holds its mutex for the whole of a refinement, which walks every candidate.
     */
    fun loadResultPage(offset: Int, count: Int): List<Long> =
        NativeLibrary.memScanResults(offset, count).toList()

    fun freeze(address: Long, type: MemValueType, value: Long, description: String) {
        viewModelScope.launch {
            val result = withContext(Dispatchers.IO) {
                // Add the entry without enabling the freeze: it writes once, then stays
                // parked until the user toggles the checkbox. Passing true would start the
                // per-frame freezer and make it behave like an always-on freeze.
                NativeLibrary.memFreezeSet(address, type.id, value, description, false)
                // Drop the row from the scan results: it now lives in the address table, and
                // leaving it behind let the user add the same address over and over.
                val remaining = NativeLibrary.memScanEraseResult(address)
                remaining to FreezeEntry.fromJson(NativeLibrary.memFreezeList())
            }
            val (remaining, entries) = result
            _freezeEntries.value = entries
            // Only touch the scan side when a scan is actually on screen; adding an address while
            // the list is idle must not replace the "enter a value" prompt with a result count.
            val scanned = _status.value.let {
                it is ScanStatus.Done || it is ScanStatus.Cancelled
            }
            if (scanned) {
                publishResultWindow(remaining)
                _status.value = ScanStatus.Done(remaining)
            }
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
                NativeLibrary.memFreezeSet(
                    entry.address, entry.type.id, value, description, entry.enabled
                )
                FreezeEntry.fromJson(NativeLibrary.memFreezeList())
            }
            _freezeEntries.value = entries
        }
    }

    fun clearFreezes() {
        viewModelScope.launch {
            val entries = withContext(Dispatchers.IO) {
                NativeLibrary.memFreezeClear()
                FreezeEntry.fromJson(NativeLibrary.memFreezeList())
            }
            _freezeEntries.value = entries
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
    }
}
