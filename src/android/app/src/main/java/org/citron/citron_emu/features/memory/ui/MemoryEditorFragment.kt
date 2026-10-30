// SPDX-FileCopyrightText: Copyright 2025 Citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

package org.citron.citron_emu.features.memory.ui

import android.app.Dialog
import android.content.Context
import android.os.Bundle
import android.text.InputType
import android.view.Gravity
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import android.view.WindowManager
import android.view.inputmethod.InputMethodManager
import android.widget.ArrayAdapter
import android.widget.EditText
import android.widget.Filter
import android.widget.TextView
import androidx.appcompat.app.AlertDialog
import androidx.core.view.updateLayoutParams
import androidx.core.widget.doAfterTextChanged
import androidx.fragment.app.DialogFragment
import androidx.fragment.app.activityViewModels
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.lifecycleScope
import androidx.lifecycle.repeatOnLifecycle
import androidx.recyclerview.widget.LinearLayoutManager
import androidx.recyclerview.widget.RecyclerView
import androidx.viewpager2.widget.ViewPager2
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import kotlinx.coroutines.launch
import org.citron.citron_emu.R
import org.citron.citron_emu.databinding.DialogMemTypeHelpBinding
import org.citron.citron_emu.databinding.DialogMemValueBinding
import org.citron.citron_emu.databinding.FragmentMemoryEditorBinding
import org.citron.citron_emu.databinding.ViewMemScanBinding
import org.citron.citron_emu.databinding.ViewMemTableBinding
import org.citron.citron_emu.features.memory.MemoryEditorViewModel
import org.citron.citron_emu.features.memory.adapter.FreezeEntryAdapter
import org.citron.citron_emu.features.memory.adapter.ScanResultAdapter
import org.citron.citron_emu.features.memory.model.FreezeEntry
import org.citron.citron_emu.features.memory.model.MemCompare
import org.citron.citron_emu.features.memory.model.MemRegion
import org.citron.citron_emu.features.memory.model.MemValueType
import org.citron.citron_emu.utils.ViewUtils.setVisible

private data class MemTypeItem(
    val type: MemValueType,
    val title: String,
    val description: String
) {
    override fun toString(): String = title
}

private class MemTypeAdapter(
    context: Context,
    private val items: List<MemTypeItem>
) : ArrayAdapter<MemTypeItem>(context, 0, items) {

    private val noFilter = object : Filter() {
        override fun performFiltering(constraint: CharSequence?): FilterResults {
            return FilterResults().apply {
                values = items
                count = items.size
            }
        }

        @Suppress("UNCHECKED_CAST")
        override fun publishResults(constraint: CharSequence?, results: FilterResults?) {
            notifyDataSetChanged()
        }

        override fun convertResultToString(resultValue: Any?): CharSequence {
            return (resultValue as? MemTypeItem)?.title ?: super.convertResultToString(resultValue)
        }
    }

    override fun getFilter(): Filter = noFilter

    override fun getView(position: Int, convertView: View?, parent: ViewGroup): View {
        val view = convertView ?: LayoutInflater.from(context)
            .inflate(R.layout.list_item_mem_scan_type, parent, false)
        val item = getItem(position) ?: return view
        view.findViewById<TextView>(R.id.type_title).text = item.title
        view.findViewById<TextView>(R.id.type_desc).text = item.description
        return view
    }
}

private class MemoryPagerAdapter(
    private val scanView: View,
    private val tableView: View
) : RecyclerView.Adapter<MemoryPagerAdapter.PageViewHolder>() {

    class PageViewHolder(itemView: View) : RecyclerView.ViewHolder(itemView)

    override fun getItemCount(): Int = 2
    override fun getItemViewType(position: Int): Int = position

    override fun onCreateViewHolder(parent: ViewGroup, viewType: Int): PageViewHolder {
        val view = if (viewType == 0) scanView else tableView
        (view.parent as? ViewGroup)?.removeView(view)
        view.layoutParams = ViewGroup.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT,
            ViewGroup.LayoutParams.MATCH_PARENT
        )
        return PageViewHolder(view)
    }

    override fun onBindViewHolder(holder: PageViewHolder, position: Int) = Unit
}

/**
 * Side-panel memory editor: value scanning, result refinement, and a frozen address table.
 *
 * Emulation is paused by the caller while this is open, but the freezer keeps enforcing
 * values because it runs on the core timing thread, not the UI thread.
 */
class MemoryEditorFragment : DialogFragment() {

    private var _binding: FragmentMemoryEditorBinding? = null
    private val binding get() = _binding!!

    private var _scanBinding: ViewMemScanBinding? = null
    private val scanBinding get() = _scanBinding!!

    private var _tableBinding: ViewMemTableBinding? = null
    private val tableBinding get() = _tableBinding!!

    private val viewModel: MemoryEditorViewModel by activityViewModels()
    private lateinit var scanAdapter: ScanResultAdapter
    private lateinit var freezeAdapter: FreezeEntryAdapter

    private var scanRegion: MemRegion? = null
    private var selectedTypes: List<MemValueType> = MemValueType.scanTypes
    private var selectedScanType: MemValueType = MemValueType.U32

    /** Whether the single value field is asking for the next-scan value. */
    private var refineModeActive = false

    /** Height one field of the value dialog gets, in px; set when the dialog is positioned. */
    private var valueFieldHeight = 0

    /** The field the built-in keypad types into; the value field unless the description has focus. */
    private var keypadTarget: EditText? = null

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
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
        _scanBinding = ViewMemScanBinding.inflate(inflater, container, false)
        _tableBinding = ViewMemTableBinding.inflate(inflater, container, false)
        return binding.root
    }

    override fun onViewCreated(view: View, savedInstanceState: Bundle?) {
        super.onViewCreated(view, savedInstanceState)

        binding.memoryEditorPager.adapter = MemoryPagerAdapter(scanBinding.root, tableBinding.root)
        binding.memoryEditorPager.registerOnPageChangeCallback(object :
            ViewPager2.OnPageChangeCallback() {
            override fun onPageSelected(position: Int) {
                if (position == 1) {
                    viewModel.refreshFreezeEntries()
                }
            }
        })

        scanAdapter = ScanResultAdapter(
            scope = viewLifecycleOwner.lifecycleScope,
            pageLoader = viewModel::loadResultPage,
            onAdd = { address -> promptAddToTable(address) }
        )
        scanBinding.memScanResults.apply {
            layoutManager = LinearLayoutManager(requireContext())
            adapter = scanAdapter
        }

        freezeAdapter = FreezeEntryAdapter(
            onToggle = viewModel::toggleFreeze,
            onEdit = ::promptEditValue,
            onDelete = { entry -> viewModel.removeFreeze(entry.address) }
        )
        tableBinding.memTableEntries.apply {
            layoutManager = LinearLayoutManager(requireContext())
            adapter = freezeAdapter
        }

        setupNavigation()
        setupHelpButton()
        setupScanControls()
        setupTableControls()

        viewModel.loadRegions()
        viewModel.refreshFreezeEntries()
        observeState()
    }

    private fun setupNavigation() {
        scanBinding.memScanGoTable.setOnClickListener {
            binding.memoryEditorPager.setCurrentItem(1, true)
        }
        tableBinding.memTableGoScan.setOnClickListener {
            binding.memoryEditorPager.setCurrentItem(0, true)
        }
    }

    private fun setupHelpButton() {
        scanBinding.memScanHelp.setOnClickListener {
            showTypeHelpDialog()
        }
    }

    private fun showTypeHelpDialog() {
        val dialogBinding = DialogMemTypeHelpBinding.inflate(layoutInflater)
        val inflater = LayoutInflater.from(requireContext())
        for (type in selectedTypes) {
            val itemView = inflater.inflate(
                R.layout.list_item_mem_scan_type,
                dialogBinding.memTypeHelpContainer,
                false
            )
            val typeExtra = if (type.width > 0) {
                getString(R.string.mem_type_bytes_format, type.id.uppercase(), type.width)
            } else {
                getString(R.string.mem_type_bytes_var, type.id.uppercase())
            }
            itemView.findViewById<TextView>(R.id.type_title).text =
                "${typeTitle(type)} · $typeExtra"
            itemView.findViewById<TextView>(R.id.type_desc).text = typeDesc(type)
            itemView.setPadding(0, 0, 0, (resources.displayMetrics.density * 12).toInt())
            dialogBinding.memTypeHelpContainer.addView(itemView)
        }

        MaterialAlertDialogBuilder(requireContext())
            .setTitle(R.string.mem_scan_type_help_title)
            .setView(dialogBinding.root)
            .setPositiveButton(android.R.string.ok, null)
            .show()
    }

    private fun setupScanControls() {
        val panel = scanBinding

        val typeItems = selectedTypes.map { type ->
            MemTypeItem(
                type = type,
                title = typeTitle(type),
                description = typeDesc(type)
            )
        }
        val typeAdapter = MemTypeAdapter(requireContext(), typeItems)
        panel.memScanType.setAdapter(typeAdapter)

        // Dropdown menu width: give it comfortable width so descriptions don't wrap awkwardly.
        panel.memScanType.dropDownWidth = (resources.displayMetrics.density * 280).toInt()

        val savedType = selectedTypes.firstOrNull { it.id == viewModel.selectedTypeId }
            ?: selectedTypes.first()
        selectedScanType = savedType
        panel.memScanType.setText(typeTitle(savedType), false)
        updateInputType(savedType)

        panel.memScanType.setOnItemClickListener { _, _, position, _ ->
            val item = typeAdapter.getItem(position) ?: return@setOnItemClickListener
            selectedScanType = item.type
            viewModel.selectedTypeId = item.type.id
            updateInputType(item.type)
            updateValueHint()
        }

        panel.memScanType.doAfterTextChanged {
            val label = panel.memScanType.text.toString()
            selectedTypes.firstOrNull { typeTitle(it) == label }?.let { matched ->
                if (selectedScanType != matched) {
                    selectedScanType = matched
                    viewModel.selectedTypeId = matched.id
                    updateInputType(matched)
                    updateValueHint()
                }
            }
        }

        panel.memScanFirst.setOnClickListener { startFirstScan() }
        panel.memScanNext.setOnClickListener { refineScan() }
        panel.memScanReset.setOnClickListener { viewModel.clearResults() }

        setupScanKeypad(panel)
    }

    /**
     * Wires the built-in keypad to the scan's value field.
     *
     * Numeric types type through the keypad, so focusing the field only slides the keypad up. Text
     * and byte patterns cannot be expressed with digits, so those types keep the system IME and the
     * keypad stays out of the way. The "keyboard" key switches to the system IME on demand either
     * way.
     */
    private fun setupScanKeypad(panel: ViewMemScanBinding) {
        val field = panel.memScanValue

        panel.memScanKeypad.onKey = { label -> insertIntoField(field, label) }
        panel.memScanKeypad.onBackspace = { deleteFromField(field) }
        panel.memScanKeypad.onSystemKeyboard = {
            // Closing the IME hands the field back to the keypad: focusing it again must not
            // raise the IME a second time.
            if (MemFieldInput.toggleSystemIme(field) && keypadSupports(currentType())) {
                field.showSoftInputOnFocus = false
            }
        }
        MemFieldInput.observeSystemIme(field) { visible ->
            panel.memScanKeypad.setSystemImeVisible(visible)
        }

        field.setOnFocusChangeListener { _, hasFocus ->
            if (hasFocus) {
                showScanKeypad(panel)
            } else {
                panel.memScanKeypad.setVisible(visible = false, gone = true)
                MemFieldInput.hideSystemIme(field)
                // Back to typing through the keypad: the system IME is only reopened by the
                // "keyboard" key, which opens it up for that one request.
                if (keypadSupports(currentType())) field.showSoftInputOnFocus = false
            }
        }

        // Tapping anywhere but the field's own box and the keypad drops the focus, which also
        // closes the keypad. The whole outlined box is kept rather than just the text view inside
        // it: its padding belongs to the field as far as the user is concerned.
        panel.root.keepFocusIn = listOf(panel.memScanValueLayout, panel.memScanKeypad)

        updateScanKeypad(panel, currentType())
    }

    /**
     * Slides the keypad in under the value field. It takes the bottom three fifths of the panel,
     * measured here because the panel has a size only once it is laid out.
     */
    private fun showScanKeypad(panel: ViewMemScanBinding) {
        if (!keypadSupports(currentType())) return
        val panelHeight = binding.root.height
        if (panelHeight > 0) {
            panel.memScanKeypad.updateLayoutParams {
                this.height = keypadHeight(panelHeight)
            }
        }
        panel.memScanKeypad.setVisible(visible = true, gone = true)
    }

    /**
     * The keypad's share of [panelHeight]: three fifths of it, capped so a tall panel does not
     * stretch the keys into slabs.
     */
    private fun keypadHeight(panelHeight: Int): Int {
        val density = resources.displayMetrics.density
        val padding = resources.getDimensionPixelSize(R.dimen.mem_keypad_padding)
        val rowCap = resources.getDimensionPixelSize(R.dimen.mem_keypad_row_max)
        val keyMargin = (KEY_MARGIN_DP * density).toInt()
        // Four key rows, each keeping its margin above and below, between the slab's two paddings.
        val cappedRows = 4 * (rowCap + 2 * keyMargin) + 2 * padding
        return (panelHeight * KEYPAD_SHARE).toInt().coerceAtMost(cappedRows)
    }

    /** Only digits, a decimal point and a sign can be produced by the built-in keypad. */
    private fun keypadSupports(type: MemValueType): Boolean = !type.isPattern

    /**
     * Puts the scan field on the keypad or back on the system IME, matching [type]. The keypad is
     * hidden here as well: changing the type while the field is focused must not leave a keypad
     * under a field it cannot type into.
     */
    private fun updateScanKeypad(panel: ViewMemScanBinding, type: MemValueType) {
        val supportsKeypad = keypadSupports(type)
        panel.memScanValue.showSoftInputOnFocus = !supportsKeypad
        panel.memScanKeypad.setVisible(
            visible = supportsKeypad && panel.memScanValue.hasFocus(),
            gone = true
        )
    }

    /**
     * Hands the panel back to the result list: the value field drops focus and the keyboard
     * goes away, so the blinking cursor does not sit on top of the scan results.
     *
     * Clearing focus alone is not enough on every IME, hence the explicit hide as well.
     */
    private fun releaseValueField() {
        scanBinding.memScanValue.clearFocus()
        scanBinding.memScanKeypad.setVisible(visible = false, gone = true)
        dismissKeyboard(scanBinding.memScanValue)
    }

    private fun dismissKeyboard(view: View) {
        val imm = view.context.getSystemService(Context.INPUT_METHOD_SERVICE)
            as? InputMethodManager ?: return
        imm.hideSoftInputFromWindow(view.windowToken, 0)
    }

    private fun updateInputType(type: MemValueType) {
        val input = scanBinding.memScanValue
        val targetInputType = when {
            type.isPattern -> InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS
            type == MemValueType.STRING -> InputType.TYPE_CLASS_TEXT
            type.isFloat -> InputType.TYPE_CLASS_NUMBER or InputType.TYPE_NUMBER_FLAG_DECIMAL or InputType.TYPE_NUMBER_FLAG_SIGNED
            type in listOf(MemValueType.S8, MemValueType.S16, MemValueType.S32, MemValueType.S64) ->
                InputType.TYPE_CLASS_NUMBER or InputType.TYPE_NUMBER_FLAG_SIGNED
            else -> InputType.TYPE_CLASS_NUMBER
        }
        if (input.inputType != targetInputType) {
            val selection = input.selectionStart to input.selectionEnd
            input.inputType = targetInputType
            input.setSelection(selection.first.coerceIn(0, input.text?.length ?: 0))
        }
        updateScanKeypad(scanBinding, type)
    }

    private fun updateValueHint() {
        val panel = scanBinding
        val type = currentType()
        panel.memScanValueLayout.hint = when {
            refineModeActive -> requireContext().getString(R.string.mem_scan_new_value)
            type == MemValueType.STRING -> requireContext().getString(R.string.mem_scan_value_text)
            type.isPattern -> requireContext().getString(R.string.mem_scan_value_pattern)
            type.isFloat -> requireContext().getString(R.string.mem_scan_value_float)
            else -> requireContext().getString(R.string.mem_scan_value)
        }
    }

    private fun setRefineMode(active: Boolean) {
        refineModeActive = active
        val panel = scanBinding
        panel.memScanFirst.setVisible(visible = !active, gone = true)
        panel.memScanNext.setVisible(visible = active, gone = true)
        panel.memScanReset.setVisible(visible = active, gone = true)
        updateValueHint()
    }

    private fun setupTableControls() {
        val panel = tableBinding
        panel.memTableClear.setOnClickListener { confirmClearTable() }
    }

    private fun currentRegion(): MemRegion? = scanRegion

    private fun currentType(): MemValueType = selectedScanType

    private fun startFirstScan() {
        val region = currentRegion() ?: return

        val type = currentType()
        val text = scanBinding.memScanValue.text?.toString().orEmpty()
        val value = if (type.isPattern) 0L else viewModel.parseInput(type, text) ?: run {
            scanBinding.memScanValueLayout.error =
                getString(R.string.mem_invalid_value)
            return
        }
        scanBinding.memScanValueLayout.error = null

        // String searches raw text (encoded as UTF-8 bytes); Aob still takes hex, where
        // "??" marks a wildcard byte.
        val pattern = if (type == MemValueType.STRING) {
            text.toByteArray(Charsets.UTF_8)
                .joinToString("") { "%02X".format(it.toInt() and 0xFF) }
        } else {
            text.replace(" ", "")
        }
        if (pattern.isEmpty() || (type == MemValueType.AOB && pattern.length % 2 != 0)) {
            scanBinding.memScanValueLayout.error =
                getString(R.string.mem_invalid_value)
            return
        }

        viewModel.startScan(region, type, value, pattern, FLOAT_EPSILON)
        releaseValueField()
    }

    private fun refineScan() {
        val type = currentType()

        val text = scanBinding.memScanValue.text?.toString().orEmpty()
        val value = viewModel.parseInput(type, text) ?: run {
            scanBinding.memScanValueLayout.error =
                getString(R.string.mem_invalid_value)
            return
        }
        scanBinding.memScanValueLayout.error = null
        viewModel.refine(type, MemCompare.EXACT, value, FLOAT_EPSILON)
        releaseValueField()
    }

    private fun promptAddToTable(address: Long) {
        val type = currentType()
        if (type.isPattern) return
        // Seed the value with whatever is in memory right now; the user can change it next.
        val current = viewModel.readValue(address, type)
        showValueDialog(
            type = type,
            initialValue = current?.let { formatValue(type, it) } ?: "",
            initialDescription = "",
            showDescription = true
        ) { value, description ->
            viewModel.freeze(address, type, value, description)
        }
    }

    private fun promptEditValue(entry: FreezeEntry) {
        showValueDialog(
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

        val dialog = MaterialAlertDialogBuilder(
            requireContext(),
            R.style.ThemeOverlay_Citron_ValueDialog
        )
            .setView(dialogBinding.root)
            .setBackgroundInsetStart(0)
            .setBackgroundInsetTop(0)
            .setBackgroundInsetEnd(0)
            .setBackgroundInsetBottom(0)
            .create()

        setupKeypad(dialogBinding)

        dialogBinding.memValueCancel.setOnClickListener { dialog.dismiss() }
        dialogBinding.memValueConfirm.setOnClickListener {
            val text = dialogBinding.memValue.text?.toString().orEmpty()
            val value = viewModel.parseInput(type, text)
            if (value == null) {
                dialogBinding.memValueLayout.error = getString(R.string.mem_invalid_value)
                applyValueFieldHeight(dialogBinding, hasError = true)
                return@setOnClickListener
            }
            val description = if (showDescription) {
                dialogBinding.memDescription.text?.toString()?.trim().orEmpty()
            } else {
                ""
            }
            dismissKeyboard(dialogBinding.memValue)
            dialog.dismiss()
            onConfirm(value, description)
        }

        dialog.show()
        positionDialogOnScanPanel(dialog, dialogBinding)
        dialog.setOnDismissListener {
            keypadTarget = null
        }
    }

    /**
     * Wires the built-in keypad to the field it should type into.
     *
     * The value field never raises the system IME on its own: the keypad under the dialog is its
     * input. The description field is text, so it keeps the system IME and the keypad follows
     * whichever field was touched last. The "keyboard" key hands the current field over to the
     * user's own IME for anything the keypad cannot express.
     */
    private fun setupKeypad(dialogBinding: DialogMemValueBinding) {
        val valueField = dialogBinding.memValue
        val descriptionField = dialogBinding.memDescription

        // The value field types through the keypad, so focus alone must not raise the system IME.
        valueField.showSoftInputOnFocus = false
        keypadTarget = valueField

        valueField.setOnFocusChangeListener { _, hasFocus ->
            if (hasFocus) {
                keypadTarget = valueField
            } else {
                // A later "keyboard" tap has to start from the keypad-only state again.
                valueField.showSoftInputOnFocus = false
            }
        }
        descriptionField.setOnFocusChangeListener { _, hasFocus ->
            if (hasFocus) keypadTarget = descriptionField
        }

        dialogBinding.memKeypad.onKey = { label ->
            insertIntoField(keypadTarget ?: valueField, label)
        }
        dialogBinding.memKeypad.onBackspace = {
            deleteFromField(keypadTarget ?: valueField)
        }
        dialogBinding.memKeypad.onSystemKeyboard = {
            val target = keypadTarget ?: valueField
            // Closing the IME on the value field hands it back to the keypad, so the next focus
            // there does not raise the IME again. The description field is text and keeps it.
            if (MemFieldInput.toggleSystemIme(target) && target === valueField) {
                valueField.showSoftInputOnFocus = false
            }
        }
        MemFieldInput.observeSystemIme(valueField) { visible ->
            dialogBinding.memKeypad.setSystemImeVisible(visible)
        }
    }

    /** Types [label] at the caret of [field], replacing the selection if it has one. */
    private fun insertIntoField(field: EditText, label: String) {
        MemFieldInput.insert(field, label)
    }

    /** Deletes the selection of [field], or the character before the caret when there is none. */
    private fun deleteFromField(field: EditText) {
        MemFieldInput.delete(field)
    }

    /**
     * Puts the value dialog on top of the panel it was opened from: same width, flush against
     * the panel's top-left corner. The window covers the whole panel so the built-in keypad can
     * reach the bottom of it; the field card takes [FIELD_CARD_SHARE] of that and the keypad the
     * rest, both capped by [fieldCardHeight] and [keypadHeight].
     */
    private fun positionDialogOnScanPanel(dialog: AlertDialog, dialogBinding: DialogMemValueBinding) {
        val window = dialog.window ?: return
        val panel = binding.root
        val panelLocation = IntArray(2)
        panel.getLocationOnScreen(panelLocation)

        // The panel window is MATCH_PARENT tall, so it normally spans the whole screen. Its own
        // height still wins when it is laid out: measuring it is only unreliable for the pages
        // inside the pager, and those are what read back as zero.
        val panelHeight = panel.height.takeIf { it > 0 } ?: resources.displayMetrics.heightPixels
        val cardHeight = fieldCardHeight(panelHeight)

        // The keypad sits at the bottom of the panel and the system IME (raised by the "keyboard"
        // key) draws over it: resizing the window for the IME would squash the card instead.
        window.setSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_ADJUST_NOTHING)
        window.setGravity(Gravity.TOP or Gravity.START)
        window.attributes = window.attributes.apply {
            x = panelLocation[0]
            y = panelLocation[1]
            width = panel.width
            height = panelHeight
        }

        // The window height is not the only thing that decides the dialog's size, so the card is
        // sized directly, and the rows inside it as well.
        val density = resources.displayMetrics.density
        val verticalExtras = (12 + 12 + 8) * density
        val rowHeight = ((cardHeight - verticalExtras) / 2).toInt().coerceAtLeast(1)
        // Half of the card per row, and each outlined box fills its row: the box keeps a slice
        // of its own space around the text field, so the field starts out a slice short of the row.
        val minFieldHeight = (MIN_FIELD_HEIGHT_DP * density).toInt()
        val maxFieldHeight = resources.getDimensionPixelSize(R.dimen.mem_field_row_max)
        valueFieldHeight = (rowHeight - FIELD_BOX_EXTRA_DP * density).toInt()
            .coerceIn(minFieldHeight, maxFieldHeight)
        // The alert's own content holder is wrap_content, so the root is told to fill the window.
        dialogBinding.root.updateLayoutParams { this.height = panelHeight }
        dialogBinding.memValueCard.updateLayoutParams { this.height = cardHeight }
        dialogBinding.memKeypad.updateLayoutParams { this.height = keypadHeight(panelHeight) }
        dialogBinding.memValueRow.updateLayoutParams { this.height = rowHeight }
        dialogBinding.memDescriptionRow.updateLayoutParams { this.height = rowHeight }
        dialogBinding.memDescription.minHeight = valueFieldHeight
        applyValueFieldHeight(dialogBinding, hasError = false)

        // The slice the box adds is style-dependent, so once the first layout has run the field
        // takes exactly that slice back: an over-tall box would be clipped by the row instead of
        // filling it, and a short one would leave a gap.
        dialogBinding.root.post {
            val boxOverflow = dialogBinding.memValueLayout.height - rowHeight
            // A layout that has not run yet reports zero height; measuring it then would "fix" the
            // field height against a box that does not exist.
            if (dialogBinding.memValueLayout.height > 0 && boxOverflow != 0) {
                valueFieldHeight = (valueFieldHeight - boxOverflow).coerceAtLeast(minFieldHeight)
                dialogBinding.memDescription.minHeight = valueFieldHeight
                applyValueFieldHeight(dialogBinding, hasError = false)
            }
        }
    }

    /**
     * Keeps the value field at its full half-height, and only lifts it while the layout shows an
     * error: the error label sits inside the layout's height and would be clipped otherwise.
     */
    private fun applyValueFieldHeight(dialogBinding: DialogMemValueBinding, hasError: Boolean) {
        val reserve = if (hasError) {
            (FIELD_ERROR_RESERVE_DP * resources.displayMetrics.density).toInt()
        } else {
            0
        }
        dialogBinding.memValue.minHeight = (valueFieldHeight - reserve).coerceAtLeast(0)
    }

    /**
     * The field card's share of [panelHeight]: two fifths of it, capped so a tall panel does not
     * stretch the two fields into slabs. Below the cap the card grows with the panel.
     */
    private fun fieldCardHeight(panelHeight: Int): Int {
        val extras = resources.getDimensionPixelSize(R.dimen.mem_field_pane_padding_top) +
            resources.getDimensionPixelSize(R.dimen.mem_field_pane_padding_bottom) +
            resources.getDimensionPixelSize(R.dimen.mem_field_row_gap)
        val rowCap = resources.getDimensionPixelSize(R.dimen.mem_field_row_max)
        return (panelHeight * FIELD_CARD_SHARE).toInt().coerceAtMost(2 * rowCap + extras)
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
            }
        }
    }

    private fun onRegionsChanged(regions: List<MemRegion>) {
        // Native now reports only the main code+data region; just record it for scanning.
        scanRegion = regions.firstOrNull()
    }

    private fun renderStatus(status: MemoryEditorViewModel.ScanStatus) {
        val panel = scanBinding
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
        // Count both numbers: “N frozen addresses” lied whenever some rows were unticked.
        tableBinding.memTableStatus.text = if (entries.isEmpty()) {
            getString(R.string.mem_freeze_none)
        } else {
            getString(R.string.mem_freeze_count, entries.size, entries.count { it.enabled })
        }
    }

    private fun typeTitle(type: MemValueType): String = getString(
        when (type) {
            MemValueType.U32 -> R.string.mem_type_u32_title
            MemValueType.F32 -> R.string.mem_type_f32_title
            MemValueType.U16 -> R.string.mem_type_u16_title
            MemValueType.U8 -> R.string.mem_type_u8_title
            MemValueType.U64 -> R.string.mem_type_u64_title
            MemValueType.S32 -> R.string.mem_type_s32_title
            MemValueType.F64 -> R.string.mem_type_f64_title
            MemValueType.S16 -> R.string.mem_type_s16_title
            MemValueType.S8 -> R.string.mem_type_s8_title
            MemValueType.S64 -> R.string.mem_type_s64_title
            MemValueType.STRING -> R.string.mem_type_str_title
            MemValueType.AOB -> R.string.mem_type_aob_title
        }
    )

    private fun typeDesc(type: MemValueType): String = getString(
        when (type) {
            MemValueType.U32 -> R.string.mem_type_u32_desc
            MemValueType.F32 -> R.string.mem_type_f32_desc
            MemValueType.U16 -> R.string.mem_type_u16_desc
            MemValueType.U8 -> R.string.mem_type_u8_desc
            MemValueType.U64 -> R.string.mem_type_u64_desc
            MemValueType.S32 -> R.string.mem_type_s32_desc
            MemValueType.F64 -> R.string.mem_type_f64_desc
            MemValueType.S16 -> R.string.mem_type_s16_desc
            MemValueType.S8 -> R.string.mem_type_s8_desc
            MemValueType.S64 -> R.string.mem_type_s64_desc
            MemValueType.STRING -> R.string.mem_type_str_desc
            MemValueType.AOB -> R.string.mem_type_aob_desc
        }
    )

    private fun describeType(type: MemValueType): String = "${typeTitle(type)} — ${typeDesc(type)}"

    private fun formatValue(type: MemValueType, bits: Long): String =
        FreezeEntry(0, type, bits, null, type.width, true, "").formatValue(bits)

    override fun onDestroyView() {
        super.onDestroyView()
        _binding = null
        _scanBinding = null
        _tableBinding = null
    }

    override fun onDestroy() {
        // Tell the host to resume emulation; a dismissed dialog must not leave the game paused.
        parentFragmentManager.setFragmentResult(REQUEST_KEY, Bundle.EMPTY)
        super.onDestroy()
    }

    companion object {
        const val TAG = "MemoryEditorFragment"
        const val REQUEST_KEY = "MemoryEditorFragment.result"
        private const val FLOAT_EPSILON = 0.001
        private const val WINDOW_WIDTH_RATIO = 0.4f

        /** Share of the panel the field card takes; the keypad takes [KEYPAD_SHARE] under it. */
        private const val FIELD_CARD_SHARE = 0.4f
        private const val KEYPAD_SHARE = 0.6f

        /** Margin around each key, which the row cap has to account for. */
        private const val KEY_MARGIN_DP = 3f

        // Space the outlined box adds above the text field, plus a small margin so the box never
        // outgrows the row that holds it.
        private const val FIELD_BOX_EXTRA_DP = 10f
        private const val FIELD_ERROR_RESERVE_DP = 28f
        private const val MIN_FIELD_HEIGHT_DP = 48f

        // No arguments: the address table is session-only, so the editor no longer needs the
        // program id it used to key the saved table on.
        fun newInstance(): MemoryEditorFragment = MemoryEditorFragment()
    }
}
