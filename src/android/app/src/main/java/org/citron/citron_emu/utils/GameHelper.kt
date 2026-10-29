// SPDX-FileCopyrightText: 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

package org.citron.citron_emu.utils

import android.content.SharedPreferences
import android.net.Uri
import androidx.preference.PreferenceManager
import kotlinx.serialization.decodeFromString
import kotlinx.serialization.encodeToString
import kotlinx.serialization.json.Json
import org.citron.citron_emu.NativeLibrary
import org.citron.citron_emu.CitronApplication
import org.citron.citron_emu.model.Game
import org.citron.citron_emu.model.GameDir
import org.citron.citron_emu.model.MinimalDocumentFile

object GameHelper {
    private const val KEY_OLD_GAME_PATH = "game_path"
    private const val KEY_GAMES_STALE = "games_stale"
    const val KEY_GAMES = "Games"

    private lateinit var preferences: SharedPreferences

    /**
     * Marks the cached game list as stale.
     *
     * Titles come out of each game's NACP, and which entry is read depends on the emulated
     * language. Changing a setting in the settings screen therefore leaves the titles already
     * scanned in memory and in [KEY_GAMES] wrong, and nothing in that screen knows which of its
     * settings a title depends on. The game list rescans once when it is shown again.
     */
    fun markGamesStale() {
        PreferenceManager.getDefaultSharedPreferences(CitronApplication.appContext)
            .edit()
            .putBoolean(KEY_GAMES_STALE, true)
            .apply()
    }

    fun getCustomTitle(path: String): String =
        PreferenceManager.getDefaultSharedPreferences(CitronApplication.appContext)
            .getString(customTitleKey(path), "") ?: ""

    fun setCustomTitle(path: String, title: String) {
        PreferenceManager.getDefaultSharedPreferences(CitronApplication.appContext)
            .edit()
            .putString(customTitleKey(path), title)
            .apply()
    }

    /**
     * Rewrites one entry of the cached game list. The cache is what the list shows on the next
     * start when no rescan is needed, so a title changed outside of a scan has to reach it too.
     */
    fun replaceCachedGame(updated: Game) {
        val preferences =
            PreferenceManager.getDefaultSharedPreferences(CitronApplication.appContext)
        val stored = preferences.getStringSet(KEY_GAMES, emptySet())
        if (stored.isNullOrEmpty()) {
            return
        }

        val replaced = stored.map { json ->
            val cached: Game = try {
                Json.decodeFromString(json)
            } catch (e: Exception) {
                return@map json
            }
            if (cached.path == updated.path) {
                Json.encodeToString(updated)
            } else {
                json
            }
        }
        preferences.edit().putStringSet(KEY_GAMES, replaced.toSet()).apply()
    }

    private fun customTitleKey(path: String) = "${path}_CustomTitle"

    /**
     * Returns true once after [markGamesStale], so a change triggers exactly one rescan.
     */
    fun consumeGamesStale(): Boolean {
        val preferences =
            PreferenceManager.getDefaultSharedPreferences(CitronApplication.appContext)
        if (!preferences.getBoolean(KEY_GAMES_STALE, false)) {
            return false
        }
        preferences.edit().putBoolean(KEY_GAMES_STALE, false).apply()
        return true
    }

    fun getGames(): List<Game> {
        val games = mutableListOf<Game>()
        val context = CitronApplication.appContext
        preferences = PreferenceManager.getDefaultSharedPreferences(context)

        val gameDirs = mutableListOf<GameDir>()
        val oldGamesDir = preferences.getString(KEY_OLD_GAME_PATH, "") ?: ""
        if (oldGamesDir.isNotEmpty()) {
            gameDirs.add(GameDir(oldGamesDir, true))
            preferences.edit().remove(KEY_OLD_GAME_PATH).apply()
        }
        gameDirs.addAll(NativeConfig.getGameDirs())

        // Ensure keys are loaded so that ROM metadata can be decrypted.
        NativeLibrary.reloadKeys()

        // Reset metadata so we don't use stale information
        GameMetadata.resetMetadata()

        // Remove previous filesystem provider information so we can get up to date version info
        NativeLibrary.clearFilesystemProvider()

        val badDirs = mutableListOf<Int>()
        gameDirs.forEachIndexed { index: Int, gameDir: GameDir ->
            val gameDirUri = Uri.parse(gameDir.uriString)
            val isValid = FileUtil.isTreeUriValid(gameDirUri)
            if (isValid) {
                addGamesRecursive(
                    games,
                    FileUtil.listFiles(gameDirUri),
                    if (gameDir.deepScan) 3 else 1
                )
            } else {
                badDirs.add(index)
            }
        }

        // Remove all game dirs with insufficient permissions from config
        if (badDirs.isNotEmpty()) {
            var offset = 0
            badDirs.forEach {
                gameDirs.removeAt(it - offset)
                offset++
            }
        }
        NativeConfig.setGameDirs(gameDirs.toTypedArray())

        // Cache list of games found on disk
        val serializedGames = mutableSetOf<String>()
        games.forEach {
            serializedGames.add(Json.encodeToString(it))
        }
        preferences.edit()
            .remove(KEY_GAMES)
            .putStringSet(KEY_GAMES, serializedGames)
            .apply()

        return games.toList()
    }

    private fun addGamesRecursive(
        games: MutableList<Game>,
        files: Array<MinimalDocumentFile>,
        depth: Int
    ) {
        if (depth <= 0) {
            return
        }

        files.forEach {
            if (it.isDirectory) {
                addGamesRecursive(
                    games,
                    FileUtil.listFiles(it.uri),
                    depth - 1
                )
            } else {
                if (Game.extensions.contains(FileUtil.getExtension(it.uri))) {
                    val game = getGame(it.uri, true)
                    if (game != null) {
                        games.add(game)
                    }
                }
            }
        }
    }

    fun getGame(uri: Uri, addedToLibrary: Boolean): Game? {
        val filePath = uri.toString()
        if (!GameMetadata.getIsValid(filePath)) {
            return null
        }

        // Needed to update installed content information
        NativeLibrary.addFileToFilesystemProvider(filePath)

        var name = GameMetadata.getTitle(filePath)

        // If the game's title field is empty, use the filename.
        if (name.isEmpty()) {
            name = FileUtil.getFilename(uri)
        }
        var programId = GameMetadata.getProgramId(filePath)

        // If the game's ID field is empty, use the filename without extension.
        if (programId.isEmpty()) {
            programId = name.substring(0, name.lastIndexOf("."))
        }

        val newGame = Game(
            name,
            filePath,
            programId,
            GameMetadata.getDeveloper(filePath),
            GameMetadata.getVersion(filePath, false),
            GameMetadata.getIsHomebrew(filePath),
            getCustomTitle(filePath)
        )

        if (addedToLibrary) {
            val addedTime = preferences.getLong(newGame.keyAddedToLibraryTime, 0L)
            if (addedTime == 0L) {
                preferences.edit()
                    .putLong(newGame.keyAddedToLibraryTime, System.currentTimeMillis())
                    .apply()
            }
        }

        return newGame
    }
}
