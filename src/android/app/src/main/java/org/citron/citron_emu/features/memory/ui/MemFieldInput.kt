// SPDX-FileCopyrightText: Copyright 2025 Citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

package org.citron.citron_emu.features.memory.ui

import android.content.Context
import android.view.inputmethod.InputMethodManager
import android.widget.EditText
import androidx.core.view.ViewCompat
import androidx.core.view.WindowInsetsCompat

/**
 * Editing the built-in keypad performs on a text field: it works on the field's editable at the
 * caret, so a selection is replaced the same way the system IME would replace it.
 */
object MemFieldInput {

    /** Types [label] at the caret, replacing the selection if the field has one. */
    fun insert(field: EditText, label: String) {
        val editable = field.text ?: return
        val start = field.selectionStart.coerceIn(0, editable.length)
        val end = field.selectionEnd.coerceIn(0, editable.length)
        val from = minOf(start, end)
        editable.replace(from, maxOf(start, end), label)
        field.setSelection((from + label.length).coerceAtMost(editable.length))
    }

    /** Deletes the selection, or the character before the caret when there is none. */
    fun delete(field: EditText) {
        val editable = field.text ?: return
        val start = field.selectionStart.coerceIn(0, editable.length)
        val end = field.selectionEnd.coerceIn(0, editable.length)
        val from = minOf(start, end)
        val to = maxOf(start, end)
        if (from != to) {
            editable.delete(from, to)
            field.setSelection(from)
            return
        }
        if (from == 0) return
        editable.delete(from - 1, from)
        field.setSelection(from - 1)
    }

    /**
     * Hands the field over to the system IME, for whatever the keypad cannot express.
     *
     * Fields under the keypad run with `showSoftInputOnFocus = false`, which is what normally
     * blocks the IME, so it has to be opened up first.
     */
    fun openSystemIme(field: EditText) {
        val imm = field.context.getSystemService(Context.INPUT_METHOD_SERVICE)
            as? InputMethodManager ?: return
        field.showSoftInputOnFocus = true
        field.requestFocus()
        field.post { imm.showSoftInput(field, InputMethodManager.SHOW_IMPLICIT) }
    }

    /** Hides the system IME, if it is up. */
    fun hideSystemIme(field: EditText) {
        val imm = field.context.getSystemService(Context.INPUT_METHOD_SERVICE)
            as? InputMethodManager ?: return
        imm.hideSoftInputFromWindow(field.windowToken, 0)
    }

    /**
     * Shows or hides the system IME for [field], whichever it is currently doing.
     *
     * The IME's own state is what decides, not what was asked for last: the IME can be opened by the
     * "keyboard" key, by the system itself, or it can be closed from the navigation bar without the
     * app hearing about it.
     */
    fun toggleSystemIme(field: EditText): Boolean {
        val wasVisible = isSystemImeVisible(field)
        if (wasVisible) {
            // Only the IME is dismissed; whether focusing the field may raise it again is the
            // caller's business, since a text field must keep that ability.
            hideSystemIme(field)
        } else {
            openSystemIme(field)
        }
        return wasVisible
    }

    /**
     * Whether the system IME is up for the window [field] lives in.
     *
     * The window insets are what the IME actually does to the window, which stays true even when
     * the focused view is not one the IME accepts text from; the IME service only reports the
     * focused editor, so it is just the fallback for a field that is not attached yet.
     */
    fun isSystemImeVisible(field: EditText): Boolean {
        val insets = ViewCompat.getRootWindowInsets(field)
        if (insets != null) return insets.isVisible(WindowInsetsCompat.Type.ime())
        val imm = field.context.getSystemService(Context.INPUT_METHOD_SERVICE)
            as? InputMethodManager ?: return false
        return imm.isAcceptingText
    }
}
