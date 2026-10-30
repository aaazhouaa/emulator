// SPDX-FileCopyrightText: 2026 Citron Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

package org.citron.citron_emu.utils

import android.app.ActivityManager
import org.citron.citron_emu.CitronApplication

/**
 * Keeps the library out of the recents screen while a game is running.
 *
 * The game is started in its own task so that the library's singleTask launch cannot clear it.
 * The side effect is two cards in recents: the game, and the library behind it. The library card
 * is only ever a detour on the way back to the game, so it is hidden for as long as a game is up
 * and restored once the session ends, which leaves it as the only thing recents can return to.
 *
 * A manifest flag cannot express this, since it is fixed per activity and the library is not
 * always sitting behind a game.
 */
object RecentTaskVisibility {
    /** Task id of the library, recorded when it is created; -1 until then. */
    private var libraryTaskId: Int = -1

    fun registerLibraryTask(taskId: Int) {
        libraryTaskId = taskId
    }

    /** Hides the library's task from recents while a game session is up. */
    fun hideLibrary() = setLibraryHidden(true)

    /** Restores the library's task to recents once no game is running. */
    fun showLibrary() = setLibraryHidden(false)

    private fun setLibraryHidden(hidden: Boolean) {
        val taskId = libraryTaskId
        if (taskId == -1) {
            return
        }
        val activityManager =
            CitronApplication.appContext.getSystemService(ActivityManager::class.java)
                ?: return
        val task = try {
            activityManager.appTasks.firstOrNull { it.taskInfo?.taskId == taskId }
        } catch (e: Exception) {
            // Without the task list there is nothing to update. Leaving recents as it is beats
            // crashing the session that just started.
            Log.warning("[RecentTaskVisibility] Could not read app tasks: ${e.message}")
            return
        }
        if (task == null) {
            // The library was finished, so a later game start has nothing to hide.
            libraryTaskId = -1
            return
        }
        try {
            task.setExcludeFromRecents(hidden)
        } catch (e: Exception) {
            Log.warning("[RecentTaskVisibility] Could not update recents: ${e.message}")
        }
    }
}
