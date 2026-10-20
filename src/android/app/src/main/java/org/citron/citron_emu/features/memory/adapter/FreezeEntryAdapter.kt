// SPDX-FileCopyrightText: Copyright 2025 Citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

package org.citron.citron_emu.features.memory.adapter

import android.view.LayoutInflater
import android.view.ViewGroup
import androidx.recyclerview.widget.RecyclerView
import org.citron.citron_emu.databinding.ListItemFreezeEntryBinding
import org.citron.citron_emu.features.memory.model.FreezeEntry
import org.citron.citron_emu.R
import org.citron.citron_emu.viewholder.AbstractViewHolder

/** Renders the frozen address table. */
class FreezeEntryAdapter(
    private val onToggle: (FreezeEntry, Boolean) -> Unit,
    private val onEdit: (FreezeEntry) -> Unit,
    private val onDelete: (FreezeEntry) -> Unit
) : RecyclerView.Adapter<FreezeEntryAdapter.EntryViewHolder>() {

    private val entries = mutableListOf<FreezeEntry>()

    fun submit(newEntries: List<FreezeEntry>) {
        entries.clear()
        entries.addAll(newEntries)
        notifyDataSetChanged()
    }

    override fun onCreateViewHolder(parent: ViewGroup, viewType: Int): EntryViewHolder {
        val binding = ListItemFreezeEntryBinding.inflate(
            LayoutInflater.from(parent.context), parent, false
        )
        return EntryViewHolder(binding)
    }

    override fun onBindViewHolder(holder: EntryViewHolder, position: Int) =
        holder.bind(entries[position])

    override fun getItemCount(): Int = entries.size

    inner class EntryViewHolder(
        private val binding: ListItemFreezeEntryBinding
    ) : AbstractViewHolder<FreezeEntry>(binding) {
        override fun bind(model: FreezeEntry) {
            val context = binding.root.context

            binding.freezeEntryTitle.text = model.description.ifEmpty {
                context.getString(R.string.mem_freeze_untitled)
            }
            binding.freezeEntryAddress.text =
                context.getString(R.string.mem_freeze_address_line, model.addressText, model.typeText())

            // Show the value being enforced alongside the live one so a mismatch is visible.
            binding.freezeEntryValue.text = if (model.current == null) {
                context.getString(R.string.mem_freeze_value_unmapped, model.valueText)
            } else {
                context.getString(R.string.mem_freeze_value_line, model.valueText, model.currentText)
            }

            // Rebind listeners explicitly: the holder is recycled, and an unguarded listener
            // assignment inside setOnCheckedChangeListener would fire on the next bind.
            binding.freezeEntryCheckbox.setOnCheckedChangeListener(null)
            binding.freezeEntryCheckbox.isChecked = model.enabled
            binding.freezeEntryCheckbox.setOnCheckedChangeListener { _, isChecked ->
                onToggle(model, isChecked)
            }

            binding.freezeEntryEdit.setOnClickListener { onEdit(model) }
            binding.freezeEntryDelete.setOnClickListener { onDelete(model) }
        }
    }
}
