// SPDX-FileCopyrightText: Copyright 2025 Citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

package org.citron.citron_emu.features.memory.ui

import android.content.Context
import android.util.AttributeSet
import android.view.LayoutInflater
import android.view.ViewGroup
import android.widget.LinearLayout
import com.google.android.material.button.MaterialButton
import org.citron.citron_emu.R

/**
 * Built-in keypad shared by the value dialog and the scan panel.
 *
 * The digit keys carry their character in `android:tag`, which is what gets typed into the target
 * field. The two keys on the right do something else than insert text, so they report through
 * their own callbacks: one deletes, the other hands the field to the user's own IME.
 */
class MemKeypadView @JvmOverloads constructor(
    context: Context,
    attrs: AttributeSet? = null
) : LinearLayout(context, attrs) {

    /** Called with the character of a digit key. */
    var onKey: ((String) -> Unit)? = null

    /** Called when the delete key is tapped. */
    var onBackspace: (() -> Unit)? = null

    /** Called when the keyboard key is tapped, which raises the system IME instead. */
    var onSystemKeyboard: (() -> Unit)? = null

    init {
        // The layout is a merge, so this view is the container the key rows sit in.
        orientation = HORIZONTAL
        isBaselineAligned = false
        LayoutInflater.from(context).inflate(R.layout.view_mem_keypad, this, true)
        // Tapping a key must not move the focus: the field keeps its caret, and the system IME, if
        // it is up, stays up while the keys are used.
        descendantFocusability = FOCUS_BLOCK_DESCENDANTS

        for (key in keycaps()) {
            val label = key.tag as? String ?: continue
            key.setOnClickListener { onKey?.invoke(label) }
        }
        findViewById<MaterialButton>(R.id.mem_keypad_backspace).setOnClickListener {
            onBackspace?.invoke()
        }
        findViewById<MaterialButton>(R.id.mem_keypad_ime).setOnClickListener {
            onSystemKeyboard?.invoke()
        }
    }

    private fun keycaps(): List<MaterialButton> {
        val keys = mutableListOf<MaterialButton>()
        collectKeycaps(this, keys)
        return keys
    }

    private fun collectKeycaps(group: ViewGroup, into: MutableList<MaterialButton>) {
        for (index in 0 until group.childCount) {
            when (val child = group.getChildAt(index)) {
                is MaterialButton -> into.add(child)
                is ViewGroup -> collectKeycaps(child, into)
            }
        }
    }
}
