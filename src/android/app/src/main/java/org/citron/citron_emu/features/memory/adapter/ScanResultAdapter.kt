// SPDX-FileCopyrightText: Copyright 2025 Citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

package org.citron.citron_emu.features.memory.adapter

import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import androidx.recyclerview.widget.RecyclerView
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.citron.citron_emu.R
import org.citron.citron_emu.databinding.ListItemScanResultBinding
import org.citron.citron_emu.viewholder.AbstractViewHolder

/**
 * Renders the scan result list as a window over the native result set.
 *
 * A first scan can produce millions of candidates, so the adapter never holds them all: it knows
 * only the total count and pulls [PAGE_SIZE]-sized slices from native on demand. At most
 * [MAX_CACHED_PAGES] pages stay resident, so memory is bounded by roughly
 * `PAGE_SIZE * MAX_CACHED_PAGES` addresses no matter how many the scan found.
 *
 * Pages are loaded **asynchronously**. [pageLoader] must never be called from the binding pass:
 * the native scan session holds its mutex for the whole of a refinement, which walks every
 * candidate, so a synchronous load would block the UI thread for seconds and ANR. A missing page
 * renders as a placeholder row and is filled in by a background load, then repainted with a
 * targeted [notifyItemRangeChanged].
 *
 * All adapter state is touched on the UI thread only; [scope] must be a main-dispatcher scope
 * (a lifecycle scope) so that a load in flight cannot outlive the view.
 */
class ScanResultAdapter(
    private val scope: CoroutineScope,
    private val pageLoader: (offset: Int, count: Int) -> List<Long>,
    private val onAdd: (address: Long) -> Unit
) : RecyclerView.Adapter<ScanResultAdapter.ResultViewHolder>() {

    private var total = 0

    /** Bumped whenever the underlying result set is replaced, so stale loads are dropped. */
    private var generation = 0

    /** Small LRU of loaded pages, keyed by page index. Access order, so visible pages survive. */
    private val pages = object : LinkedHashMap<Int, List<Long>>(MAX_CACHED_PAGES, 0.75f, true) {
        override fun removeEldestEntry(
            eldest: MutableMap.MutableEntry<Int, List<Long>>
        ): Boolean = size > MAX_CACHED_PAGES
    }

    /** Page indexes with a load in flight, to avoid queueing duplicates while scrolling. */
    private val pending = HashSet<Int>()

    /**
     * Resets the window to a result set of [count] entries.
     *
     * Called when a scan finishes or is refined, so every cached page belongs to an older list
     * and must be dropped.
     */
    fun submitCount(count: Int) {
        total = count
        generation++
        pages.clear()
        pending.clear()
        notifyDataSetChanged()
    }

    private fun cachedPage(pageIndex: Int): List<Long>? = pages[pageIndex]

    private fun requestPage(pageIndex: Int) {
        if (!pending.add(pageIndex)) {
            return
        }
        val requestGeneration = generation
        val offset = pageIndex * PAGE_SIZE
        scope.launch {
            val loaded = withContext(Dispatchers.IO) { pageLoader(offset, PAGE_SIZE) }
            if (requestGeneration != generation) {
                // The result set was replaced while this page was being copied; drop it.
                return@launch
            }
            pending.remove(pageIndex)
            if (loaded.isEmpty()) {
                // The native list shrank (refine raced the bind); the row stays a placeholder.
                return@launch
            }
            pages[pageIndex] = loaded
            val visibleCount = minOf(loaded.size, total - offset)
            if (visibleCount > 0) {
                notifyItemRangeChanged(offset, visibleCount)
            }
        }
    }

    override fun onCreateViewHolder(parent: ViewGroup, viewType: Int): ResultViewHolder {
        val binding = ListItemScanResultBinding.inflate(
            LayoutInflater.from(parent.context), parent, false
        )
        return ResultViewHolder(binding)
    }

    override fun onBindViewHolder(holder: ResultViewHolder, position: Int) {
        val pageIndex = position / PAGE_SIZE
        val page = cachedPage(pageIndex) ?: run {
            requestPage(pageIndex)
            holder.bind(null)
            return
        }
        holder.bind(page.getOrNull(position - pageIndex * PAGE_SIZE))
    }

    override fun getItemCount(): Int = total

    inner class ResultViewHolder(
        private val binding: ListItemScanResultBinding
    ) : AbstractViewHolder<Long?>(binding) {
        override fun bind(model: Long?) {
            val context = binding.root.context

            if (model == null) {
                // Page not loaded yet. Keep the row's height and show a subdued placeholder so
                // the list does not jump when the real address arrives.
                binding.scanResultAddress.text = context.getString(R.string.mem_scan_loading)
                binding.scanResultAdd.visibility = View.INVISIBLE
                binding.root.isClickable = false
                binding.root.setOnClickListener(null)
                binding.scanResultAdd.setOnClickListener(null)
                return
            }

            binding.scanResultAddress.text = "0x%016X".format(model)
            binding.scanResultAdd.visibility = View.VISIBLE
            binding.root.isClickable = true
            binding.scanResultAdd.setOnClickListener { onAdd(model) }
            binding.root.setOnClickListener { onAdd(model) }
        }
    }

    private companion object {
        /**
         * Page granularity. Must stay below the native per-call transfer cap
         * (`MaxTransferCount` = 20000) so a page is always one JNI call.
         */
        const val PAGE_SIZE = 512
        const val MAX_CACHED_PAGES = 8
    }
}
