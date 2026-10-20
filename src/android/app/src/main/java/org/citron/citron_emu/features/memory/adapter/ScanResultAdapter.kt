// SPDX-FileCopyrightText: Copyright 2025 Citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

package org.citron.citron_emu.features.memory.adapter

import android.view.LayoutInflater
import android.view.ViewGroup
import org.citron.citron_emu.databinding.ListItemScanResultBinding
import org.citron.citron_emu.viewholder.AbstractViewHolder
import androidx.recyclerview.widget.RecyclerView

/**
 * Renders a page of scan result addresses.
 *
 * The list can hold tens of thousands of entries, so only a window of the full result set is
 * materialised here. [onAdd] is invoked with the absolute address of the tapped row.
 */
class ScanResultAdapter(
    private val onAdd: (address: Long) -> Unit
) : RecyclerView.Adapter<ScanResultAdapter.ResultViewHolder>() {

    private val addresses = mutableListOf<Long>()

    fun submit(newAddresses: List<Long>) {
        addresses.clear()
        addresses.addAll(newAddresses)
        notifyDataSetChanged()
    }

    override fun onCreateViewHolder(parent: ViewGroup, viewType: Int): ResultViewHolder {
        val binding = ListItemScanResultBinding.inflate(
            LayoutInflater.from(parent.context), parent, false
        )
        return ResultViewHolder(binding)
    }

    override fun onBindViewHolder(holder: ResultViewHolder, position: Int) =
        holder.bind(addresses[position])

    override fun getItemCount(): Int = addresses.size

    inner class ResultViewHolder(
        private val binding: ListItemScanResultBinding
    ) : AbstractViewHolder<Long>(binding) {
        override fun bind(model: Long) {
            binding.scanResultAddress.text = "0x%016X".format(model)
            binding.scanResultAdd.setOnClickListener { onAdd(model) }
            binding.root.setOnClickListener { onAdd(model) }
        }
    }
}
