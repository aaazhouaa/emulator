// SPDX-FileCopyrightText: Copyright 2025 Citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

package org.citron.citron_emu.features.memory.ui

import android.app.Dialog
import android.os.Bundle
import android.view.Gravity
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import android.view.WindowManager
import android.widget.ArrayAdapter
import android.widget.Toast
import androidx.appcompat.app.AlertDialog
import androidx.core.widget.doAfterTextChanged
import androidx.fragment.app.DialogFragment
import androidx.fragment.app.activityViewModels
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.lifecycleScope
import androidx.lifecycle.repeatOnLifecycle
import androidx.recyclerview.widget.LinearLayoutManager
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import com.google.android.material.tabs.TabLayout
import kotlinx.coroutines.launch
import org.citron.citron_emu.R
import org.citron.citron_emu.databinding.DialogMemValueBinding
import org.citron.citron_emu.databinding.FragmentMemoryEditorBinding
import org.citron.citron_emu.features.memory.MemoryEditorViewModel
import org.citron.citron_emu.features.memory.adapter.FreezeEntryAdapter
import org.citron.citron_emu.features.memory.adapter.ScanResultAdapter
import org.citron.citron_emu.features.memory.model.FreezeEntry
import org.citron.citron_emu.features.memory.model.MemCompare
import org.citron.citron_emu.features.memory.model.MemRegion
import org.citron.citron_emu.features.memory.model.MemValueType
import org.citron.citron_emu.utils.ViewUtils.setVisible

/**
 * Side-panel memory editor: value scanning, result refinement, and a frozen address table.
 *
 * Emulation is paused by the caller while this is open, but the freezer keeps enforcing
 * values because it runs on the core timing thread, not the UI thread.
 */
class MemoryEditorFragment : DialogFragment() {

    private var _binding: FragmentMemoryEditorBinding? = null
    private val binding get() = _binding!!

    private val viewModel: MemoryEditorViewModel by activityViewModels()

    private lateinit var scanAdapter: ScanResultAdapter
    private lateinit var freezeAdapter: FreezeEntryAdapter

    private var scanRegion: MemRegion? = null
    private var selectedTypes: List<MemValueType> = MemValueType.scanTypes

    /** Whether the single value field is asking for the next-scan value. */
    private var refineModeActive = false

    private var gameProgramId: String = ""

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val args = requireArguments()
        gameProgramId = args.getString(ARG_PROGRAM_ID).orEmpty()
        // Left-hand side panel matching the in-game drawer.
        setStyle(STYLE_NO_TITLE, R.style.ThemeOverlay_Citron_SidePanel)
    }

    @Suppress("DEPRECATION")
    override fun onCreateDialog(savedInstanceState: Bundle?): Dialog {
        return Dialog(requireContext(), theme).apply {
            window?.setSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_ADJUST_RESIZE)
        }
    }

    override fun onStart() {
        super.onStart()
        val window = dialog?.window ?: return
        val metrics = resources.displayMetrics
        val params = window.attributes
        params.width = (metrics.widthPixels * WINDOW_WIDTH_RATIO).toInt()
        params.height = WindowManager.LayoutParams.MATCH_PARENT
        params.gravity = Gravity.START or Gravity.TOP
        window.attributes = params
        window.clearFlags(WindowManager.LayoutParams.FLAG_DIM_BEHIND)
        // Let the panel draw under the status bar so the content starts at the very top.
        window.decorView.systemUiVisibility =
            View.SYSTEM_UI_FLAG_LAYOUT_STABLE or View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
    }

    override fun onCreateView(
        inflater: LayoutInflater,
        container: ViewGroup?,
        savedInstanceState: Bundle?
    ): View {
        _binding = FragmentMemoryEditorBinding.inflate(inflater, container, false)
        return binding.root
    }

    override fun onViewCreated(view: View, savedInstanceState: Bundle?) {
        super.onViewCreated(view, savedInstanceState)

        scanAdapter = ScanResultAdapter(
            scope = viewLifecycleOwner.lifecycleScope,
            pageLoader = viewModel::loadResultPage,
            onAdd = { address -> promptAddToTable(address) }
        )
        binding.memoryEditorScanPanel.memScanResults.apply {
            layoutManager = LinearLayoutManager(requireContext())
            adapter = scanAdapter
        }

        freezeAdapter = FreezeEntryAdapter(
            onToggle = viewModel::toggleFreeze,
            onEdit = ::promptEditValue,
            onDelete = { entry -> viewModel.removeFreeze(entry.address) }
        )
        binding.memoryEditorTablePanel.memTableEntries.apply {
            layoutManager = LinearLayoutManager(requireContext())
            adapter = freezeAdapter
        }

        setupTabs()
        setupScanControls()
        setupTableControls()

        viewModel.loadRegions()
        viewModel.refreshFreezeEntries()
        observeState()
    }

    private fun setupTabs() {
        binding.memoryEditorTabs.addOnTabSelectedListener(object :
            TabLayout.OnTabSelectedListener {
            override fun onTabSelected(tab: TabLayout.Tab) {
                val showScan = tab.position == 0
                binding.memoryEditorScanPanel.root.setVisible(visible = showScan, gone = true)
                binding.memoryEditorTablePanel.root.setVisible(visible = !showScan, gone = true)
                if (!showScan) {
                    viewModel.refreshFreezeEntries()
                }
            }

            override fun onTabUnselected(tab: TabLayout.Tab) = Unit
            override fun onTabReselected(tab: TabLayout.Tab) = Unit
        })
    }

    private fun setupScanControls() {
        val panel = binding.memoryEditorScanPanel

        panel.memScanType.setAdapter(
            ArrayAdapter(
                requireContext(),
                android.R.layout.simple_list_item_1,
                selectedTypes.map { describeType(it) }
            )
        )
        val savedType = selectedTypes.firstOrNull { it.id == viewModel.selectedTypeId }
            ?: selectedTypes.first()
        panel.memScanType.setText(describeType(savedType), false)

        // Re-render the hint as the type changes and persist the pick immediately.
        panel.memScanType.doAfterTextChanged {
            updateValueHint()
            viewModel.selectedTypeId = currentType().id
        }

        panel.memScanFirst.setOnClickListener { startFirstScan() }
        panel.memScanNext.setOnClickListener { refineScan() }
        panel.memScanReset.setOnClickListener { viewModel.clearResults() }
    }

    private fun updateValueHint() {
        val panel = binding.memoryEditorScanPanel
        val type = currentType()
        panel.memScanValueLayout.hint = when {
            refineModeActive -> requireContext().getString(R.string.mem_scan_new_value)
            type.isPattern -> requireContext().getString(R.string.mem_scan_value_pattern)
            type.isFloat -> requireContext().getString(R.string.mem_scan_value_float)
            else -> requireContext().getString(R.string.mem_scan_value)
        }
    }

    private fun setRefineMode(active: Boolean) {
        refineModeActive = active
        val panel = binding.memoryEditorScanPanel
        panel.memScanFirst.setVisible(visible = !active, gone = true)
        panel.memScanNext.setVisible(visible = active, gone = true)
        panel.memScanReset.setVisible(visible = active, gone = true)
        updateValueHint()
    }

    private fun setupTableControls() {
        val panel = binding.memoryEditorTablePanel
        panel.memTableSave.setOnClickListener { viewModel.saveTable(gameProgramId) }
        panel.memTableReload.setOnClickListener { viewModel.loadTable(gameProgramId) }
        panel.memTableClear.setOnClickListener { confirmClearTable() }
    }

    private fun currentRegion(): MemRegion? = scanRegion

    private fun currentType(): MemValueType {
        val label = binding.memoryEditorScanPanel.memScanType.text.toString()
        return selectedTypes.firstOrNull { describeType(it) == label } ?: MemValueType.U32
    }

    private fun startFirstScan() {
        val region = currentRegion()
        if (region == null) {
            viewModel.notify(getString(R.string.mem_cannot_start_scan))
            return
        }

        val type = currentType()
        val text = binding.memoryEditorScanPanel.memScanValue.text?.toString().orEmpty()
        val value = if (type.isPattern) 0L else viewModel.parseInput(type, text) ?: run {
            binding.memoryEditorScanPanel.memScanValueLayout.error =
                getString(R.string.mem_invalid_value)
            return
        }
        binding.memoryEditorScanPanel.memScanValueLayout.error = null

        val pattern = if (type.isPattern) text.replace(" ", "") else ""
        if (type.isPattern && (pattern.length % 2 != 0 || pattern.isEmpty())) {
            binding.memoryEditorScanPanel.memScanValueLayout.error =
                getString(R.string.mem_invalid_value)
            return
        }

        viewModel.startScan(region, type, value, pattern, FLOAT_EPSILON)
    }

    private fun refineScan() {
        val type = currentType()

        val text = binding.memoryEditorScanPanel.memScanValue.text?.toString().orEmpty()
        val value = viewModel.parseInput(type, text) ?: run {
            binding.memoryEditorScanPanel.memScanValueLayout.error =
                getString(R.string.mem_invalid_value)
            return
        }
        binding.memoryEditorScanPanel.memScanValueLayout.error = null
        viewModel.refine(type, MemCompare.EXACT, value, FLOAT_EPSILON)
    }

    private fun promptAddToTable(address: Long) {
        val type = currentType()
        if (type.isPattern) {
            viewModel.notify(getString(R.string.mem_freeze_pattern_unsupported))
            return
        }
        // Seed the value with whatever is in memory right now; the user can change it next.
        val current = viewModel.readValue(address, type)
        showValueDialog(
            title = getString(R.string.mem_freeze_add_title),
            type = type,
            initialValue = current?.let { formatValue(type, it) } ?: "",
            initialDescription = "",
            showDescription = true
        ) { value, description ->
            viewModel.freeze(address, type, value, description)
            viewModel.notify(getString(R.string.mem_freeze_added))
        }
    }

    private fun promptEditValue(entry: FreezeEntry) {
        showValueDialog(
            title = entry.description.ifEmpty { getString(R.string.mem_freeze_untitled) },
            type = entry.type,
            initialValue = formatValue(entry.type, entry.value),
            initialDescription = entry.description,
            // Renaming is part of editing an existing entry, so keep the description field.
            showDescription = true
        ) { value, description ->
            viewModel.updateFreezeValue(entry, value, description)
        }
    }

    private fun showValueDialog(
        title: String,
        type: MemValueType,
        initialValue: String,
        initialDescription: String,
        showDescription: Boolean,
        onConfirm: (value: Long, description: String) -> Unit
    ) {
        val dialogBinding = DialogMemValueBinding.inflate(layoutInflater)
        dialogBinding.memValueLayout.hint = getString(R.string.mem_freeze_value_title)
        dialogBinding.memValue.setText(initialValue)
        dialogBinding.memValueLayout.error = null

        dialogBinding.memDescriptionLayout.hint = getString(R.string.mem_freeze_description)
        dialogBinding.memDescription.setText(initialDescription)
        dialogBinding.memDescriptionLayout.setVisible(visible = showDescription, gone = true)

        val dialog = MaterialAlertDialogBuilder(requireContext())
            .setTitle(title)
            .setView(dialogBinding.root)
            .setNegativeButton(android.R.string.cancel, null)
            .setPositiveButton(R.string.mem_confirm, null)
            .create()

        dialog.setOnShowListener {
            dialog.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener {
                val text = dialogBinding.memValue.text?.toString().orEmpty()
                val value = viewModel.parseInput(type, text)
                if (value == null) {
                    dialogBinding.memValueLayout.error =
                        getString(R.string.mem_invalid_value)
                    return@setOnClickListener
                }
                val description = if (showDescription) {
                    dialogBinding.memDescription.text?.toString()?.trim().orEmpty()
                } else {
                    ""
                }
                dialog.dismiss()
                onConfirm(value, description)
            }
        }
        dialog.show()
    }

    private fun confirmClearTable() {
        MaterialAlertDialogBuilder(requireContext())
            .setTitle(R.string.mem_table_clear)
            .setMessage(R.string.mem_freeze_clear_confirm)
            .setNegativeButton(android.R.string.cancel, null)
            .setPositiveButton(R.string.mem_table_clear) { _, _ -> viewModel.clearFreezes() }
            .show()
    }

    private fun observeState() {
        viewLifecycleOwner.lifecycleScope.launch {
            viewLifecycleOwner.repeatOnLifecycle(Lifecycle.State.STARTED) {
                launch { viewModel.regions.collect { onRegionsChanged(it) } }
                launch { viewModel.status.collect { renderStatus(it) } }
                launch {
                    viewModel.resultWindow.collect { window ->
                        scanAdapter.submitCount(window.count)
                    }
                }
                launch { viewModel.freezeEntries.collect { onFreezeEntriesChanged(it) } }
                launch {
                    viewModel.message.collect { message ->
                        if (!message.isNullOrEmpty()) {
                            Toast.makeText(requireContext(), message, Toast.LENGTH_SHORT).show()
                            viewModel.consumeMessage()
                        }
                    }
                }
            }
        }
    }

    private fun onRegionsChanged(regions: List<MemRegion>) {
        // Native now reports only the main code+data region; just record it for scanning.
        scanRegion = regions.firstOrNull()
    }

    private fun renderStatus(status: MemoryEditorViewModel.ScanStatus) {
        val panel = binding.memoryEditorScanPanel
        val running = status is MemoryEditorViewModel.ScanStatus.Running
        panel.memScanProgress.setVisible(running)

        // Show First scan only when there are no candidates yet; otherwise show Next scan + Reset.
        val hasResults = status is MemoryEditorViewModel.ScanStatus.Done ||
            status is MemoryEditorViewModel.ScanStatus.Cancelled
        setRefineMode(hasResults)

        panel.memScanStatus.text = when (status) {
            is MemoryEditorViewModel.ScanStatus.Idle ->
                getString(R.string.mem_scan_idle)
            is MemoryEditorViewModel.ScanStatus.Running -> {
                val percent = if (status.total > 0) {
                    ((status.done * 100) / status.total).toInt().coerceIn(0, 100)
                } else {
                    0
                }
                panel.memScanProgress.setProgressCompat(percent, true)
                getString(R.string.mem_scan_running, percent)
            }

            is MemoryEditorViewModel.ScanStatus.Done -> if (status.count == 0) {
                getString(R.string.mem_scan_no_results)
            } else {
                getString(R.string.mem_scan_done, status.count)
            }

            is MemoryEditorViewModel.ScanStatus.Cancelled -> getString(R.string.mem_scan_cancelled, 0)
            is MemoryEditorViewModel.ScanStatus.Failed -> status.message
        }
    }

    private fun onFreezeEntriesChanged(entries: List<FreezeEntry>) {
        freezeAdapter.submit(entries)
        binding.memoryEditorTablePanel.memTableStatus.text = when {
            entries.isEmpty() -> getString(R.string.mem_freeze_none)
            // A table restored at boot stays inactive on purpose, so say so instead of letting
            // the ticked boxes imply the values are already being enforced.
            entries.none { it.enabled } -> getString(R.string.mem_freeze_all_disabled, entries.size)
            else -> getString(R.string.mem_freeze_count, entries.size)
        }
    }

    private fun describeType(type: MemValueType): String = getString(
        when (type) {
            MemValueType.U32 -> R.string.mem_type_u32
            MemValueType.F32 -> R.string.mem_type_f32
            MemValueType.U16 -> R.string.mem_type_u16
            MemValueType.U8 -> R.string.mem_type_u8
            MemValueType.U64 -> R.string.mem_type_u64
            MemValueType.S32 -> R.string.mem_type_s32
            MemValueType.F64 -> R.string.mem_type_f64
            MemValueType.S16 -> R.string.mem_type_s16
            MemValueType.S8 -> R.string.mem_type_s8
            MemValueType.S64 -> R.string.mem_type_s64
            MemValueType.STRING -> R.string.mem_type_str
            MemValueType.AOB -> R.string.mem_type_aob
        }
    )

    private fun formatValue(type: MemValueType, bits: Long): String =
        FreezeEntry(0, type, bits, null, type.width, true, "").formatValue(bits)

    override fun onDestroyView() {
        super.onDestroyView()
        _binding = null
    }

    override fun onDestroy() {
        // Flush as a backstop: every mutation already persists, but a table left untouched while
        // the game was paused would otherwise rely on that alone.
        viewModel.persistTable(gameProgramId)
        // Tell the host to resume emulation; a dismissed dialog must not leave the game paused.
        parentFragmentManager.setFragmentResult(REQUEST_KEY, Bundle.EMPTY)
        super.onDestroy()
    }

    companion object {
        const val TAG = "MemoryEditorFragment"
        const val REQUEST_KEY = "MemoryEditorFragment.result"
        private const val ARG_PROGRAM_ID = "programId"
        private const val FLOAT_EPSILON = 0.001
        private const val WINDOW_WIDTH_RATIO = 0.4f

        fun newInstance(programId: String): MemoryEditorFragment {
            val fragment = MemoryEditorFragment()
            fragment.arguments = Bundle().apply { putString(ARG_PROGRAM_ID, programId) }
            return fragment
        }
    }
}
