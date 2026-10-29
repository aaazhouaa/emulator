// SPDX-FileCopyrightText: Copyright 2025 Citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

package org.citron.citron_emu.features.memory.ui

import android.content.Context
import android.graphics.Rect
import android.util.AttributeSet
import android.view.MotionEvent
import android.view.View
import android.widget.LinearLayout

/**
 * Layout that drops the focus of whatever it holds as soon as the user touches outside [keepFocusIn].
 *
 * Touches are inspected on the way down instead of through click listeners: the children here are a
 * scrollable list and a row of buttons, which consume the touch themselves, so a listener on the
 * layout would never fire for exactly the areas that need this.
 */
class TapOutsideFocusLayout @JvmOverloads constructor(
    context: Context,
    attrs: AttributeSet? = null
) : LinearLayout(context, attrs) {

    /**
     * Views whose own touches must leave the focus alone: the field being typed into, and the keypad
     * that types into it. Anything else inside this layout counts as outside.
     */
    var keepFocusIn: List<View> = emptyList()

    override fun dispatchTouchEvent(event: MotionEvent): Boolean {
        if (event.actionMasked == MotionEvent.ACTION_DOWN) {
            clearFocusTouchedOutside(event)
        }
        return super.dispatchTouchEvent(event)
    }

    private fun clearFocusTouchedOutside(event: MotionEvent) {
        val focused = findFocus() ?: return
        // Only a touch on a kept view keeps the focus. The focused view is not exempt by identity:
        // it is kept because it is one of the kept views, and every other touch drops the focus.
        if (keepFocusIn.any { isTouched(it, event) }) return
        focused.clearFocus()
    }

    /** Whether [event] lands inside [view], which is measured in this layout's own coordinates. */
    private fun isTouched(view: View, event: MotionEvent): Boolean {
        if (view.visibility != VISIBLE || view.width == 0 || view.height == 0) return false
        if (!isDescendant(view)) return false
        val bounds = Rect(0, 0, view.width, view.height)
        val located = runCatching { offsetDescendantRectToMyCoords(view, bounds) }.isSuccess
        return located && bounds.contains(event.x.toInt(), event.y.toInt())
    }

    /** [offsetDescendantRectToMyCoords] only accepts descendants, and throws for anything else. */
    private fun isDescendant(view: View): Boolean {
        var parent = view.parent
        while (parent is View) {
            if (parent === this) return true
            parent = parent.parent
        }
        return false
    }
}
