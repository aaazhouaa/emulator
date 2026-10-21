// SPDX-FileCopyrightText: Copyright 2025 Citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

package org.citron.citron_emu.features.memory.ui

import android.content.Context
import android.util.AttributeSet
import android.view.MotionEvent
import android.view.View
import android.view.ViewConfiguration
import android.widget.FrameLayout
import androidx.recyclerview.widget.RecyclerView
import androidx.viewpager2.widget.ViewPager2
import kotlin.math.absoluteValue

/**
 * An orientation-aware container that intercepts gestures cleanly between [ViewPager2]
 * and its nested [RecyclerView], avoiding horizontal/vertical swipe conflicts.
 */
class NestedScrollableHost @JvmOverloads constructor(
    context: Context,
    attrs: AttributeSet? = null
) : FrameLayout(context, attrs) {

    private val touchSlop = ViewConfiguration.get(context).scaledTouchSlop
    private var initialX = 0f
    private var initialY = 0f

    private val parentViewPager: ViewPager2?
        get() {
            var v: View? = parent as? View
            while (v != null && v !is ViewPager2) {
                v = v.parent as? View
            }
            return v as? ViewPager2
        }

    private val childRecyclerView: RecyclerView?
        get() {
            if (childCount == 0) return null
            return getChildAt(0) as? RecyclerView
        }

    override fun onInterceptTouchEvent(e: MotionEvent): Boolean {
        handleInterceptTouchEvent(e)
        return super.onInterceptTouchEvent(e)
    }

    private fun handleInterceptTouchEvent(e: MotionEvent) {
        val viewPager = parentViewPager ?: return

        when (e.action) {
            MotionEvent.ACTION_DOWN -> {
                initialX = e.x
                initialY = e.y
                parent.requestDisallowInterceptTouchEvent(true)
            }
            MotionEvent.ACTION_MOVE -> {
                val dx = (e.x - initialX).absoluteValue
                val dy = (e.y - initialY).absoluteValue

                // If the child RecyclerView is actively scrolling vertically,
                // strictly disallow ViewPager interception.
                val rv = childRecyclerView
                if (rv != null && rv.scrollState != RecyclerView.SCROLL_STATE_IDLE) {
                    parent.requestDisallowInterceptTouchEvent(true)
                    return
                }

                // If horizontal movement is at least 1.8x the vertical movement,
                // and exceeds touch slop, it is a deliberate horizontal swipe:
                // allow the ViewPager to intercept.
                if (dx > touchSlop && dx > dy * 1.8f) {
                    parent.requestDisallowInterceptTouchEvent(false)
                } else {
                    // Otherwise, any vertical or ambiguous movement is reserved for the list.
                    parent.requestDisallowInterceptTouchEvent(true)
                }
            }
            MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> {
                parent.requestDisallowInterceptTouchEvent(false)
            }
        }
    }
}
