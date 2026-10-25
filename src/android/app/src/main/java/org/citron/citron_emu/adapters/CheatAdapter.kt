// SPDX-FileCopyrightText: 2026 citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

package org.citron.citron_emu.adapters

import android.view.LayoutInflater
import android.view.ViewGroup
import org.citron.citron_emu.databinding.ListItemCheatBinding
import org.citron.citron_emu.model.Patch
import org.citron.citron_emu.viewholder.AbstractViewHolder

class CheatAdapter(
    private val onToggle: (Patch, Boolean) -> Unit,
    private val onEdit: (Patch) -> Unit,
    private val onDelete: (Patch) -> Unit
) : AbstractDiffAdapter<Patch, CheatAdapter.CheatViewHolder>() {
    override fun onCreateViewHolder(parent: ViewGroup, viewType: Int): CheatViewHolder {
        ListItemCheatBinding.inflate(LayoutInflater.from(parent.context), parent, false)
            .also { return CheatViewHolder(it) }
    }

    inner class CheatViewHolder(val binding: ListItemCheatBinding) :
        AbstractViewHolder<Patch>(binding) {
        override fun bind(model: Patch) {
            binding.root.setOnClickListener {
                binding.cheatCheckbox.isChecked = !binding.cheatCheckbox.isChecked
            }
            binding.title.text = model.name
            binding.cheatCheckbox.setOnCheckedChangeListener(null)
            binding.cheatCheckbox.isChecked = model.enabled
            binding.cheatCheckbox.setOnCheckedChangeListener { _, checked ->
                onToggle(model, checked)
            }
            binding.buttonEdit.setOnClickListener { onEdit(model) }
            binding.buttonDelete.setOnClickListener { onDelete(model) }
        }
    }
}
