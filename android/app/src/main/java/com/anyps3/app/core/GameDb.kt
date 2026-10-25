package com.anyps3.app.core

import android.content.ContentValues
import android.content.Context
import android.database.sqlite.SQLiteDatabase
import android.database.sqlite.SQLiteOpenHelper

data class GameRow(
    val gameId: String,
    val title: String,
    val displayName: String,
    val sourceUri: String,
    val sourceKind: String, // "iso" | "folder"
    val sizeBytes: Long,
    val coverPath: String?,
    val importedAt: Long,
    val status: String,     // not_compiled | compiling | analyzed | ready | outdated | error
    val missing: Boolean,
    val favorite: Boolean,
    val notes: String,
)

data class LogRow(val id: Long, val ts: Long, val level: String, val msg: String)

/**
 * Small metadata-only database. The actual game content (ISO/folder) is
 * NEVER copied — only its SAF URI and metadata live here.
 */
class GameDb(context: Context) : SQLiteOpenHelper(context, "anyps3.db", null, 1) {

    override fun onCreate(db: SQLiteDatabase) {
        db.execSQL(
            """CREATE TABLE games(
                game_id TEXT PRIMARY KEY,
                title TEXT NOT NULL,
                display_name TEXT NOT NULL DEFAULT '',
                source_uri TEXT NOT NULL,
                source_kind TEXT NOT NULL,
                size_bytes INTEGER NOT NULL DEFAULT 0,
                cover_path TEXT,
                imported_at INTEGER NOT NULL,
                status TEXT NOT NULL DEFAULT 'not_compiled',
                missing INTEGER NOT NULL DEFAULT 0,
                favorite INTEGER NOT NULL DEFAULT 0,
                notes TEXT NOT NULL DEFAULT ''
            )"""
        )
        db.execSQL(
            """CREATE TABLE log(
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                ts INTEGER NOT NULL,
                level TEXT NOT NULL,
                msg TEXT NOT NULL
            )"""
        )
    }

    override fun onUpgrade(db: SQLiteDatabase, oldVersion: Int, newVersion: Int) {
        // v1 — nothing yet.
    }

    fun upsertGame(g: GameRow) {
        val cv = ContentValues().apply {
            put("game_id", g.gameId)
            put("title", g.title)
            put("display_name", g.displayName)
            put("source_uri", g.sourceUri)
            put("source_kind", g.sourceKind)
            put("size_bytes", g.sizeBytes)
            put("cover_path", g.coverPath)
            put("imported_at", g.importedAt)
            put("status", g.status)
            put("missing", if (g.missing) 1 else 0)
            put("favorite", if (g.favorite) 1 else 0)
            put("notes", g.notes)
        }
        writableDatabase.insertWithOnConflict("games", null, cv, SQLiteDatabase.CONFLICT_REPLACE)
    }

    fun updateFields(gameId: String, vararg pairs: Pair<String, Any?>) {
        val cv = ContentValues()
        for ((k, v) in pairs) {
            when (v) {
                is String -> cv.put(k, v)
                is Int -> cv.put(k, v)
                is Long -> cv.put(k, v)
                is Boolean -> cv.put(k, if (v) 1 else 0)
                null -> cv.putNull(k)
            }
        }
        writableDatabase.update("games", cv, "game_id = ?", arrayOf(gameId))
    }

    fun deleteGame(gameId: String) {
        writableDatabase.delete("games", "game_id = ?", arrayOf(gameId))
    }

    fun listGames(): List<GameRow> {
        val out = mutableListOf<GameRow>()
        readableDatabase.rawQuery(
            "SELECT game_id,title,display_name,source_uri,source_kind,size_bytes,cover_path," +
                "imported_at,status,missing,favorite,notes FROM games ORDER BY favorite DESC, title COLLATE NOCASE",
            null
        ).use { c ->
            while (c.moveToNext()) {
                out.add(rowOf(c))
            }
        }
        return out
    }

    fun getGame(gameId: String): GameRow? {
        readableDatabase.rawQuery(
            "SELECT game_id,title,display_name,source_uri,source_kind,size_bytes,cover_path," +
                "imported_at,status,missing,favorite,notes FROM games WHERE game_id = ?",
            arrayOf(gameId)
        ).use { c -> if (c.moveToNext()) return rowOf(c) }
        return null
    }

    private fun rowOf(c: android.database.Cursor) = GameRow(
        gameId = c.getString(0),
        title = c.getString(1),
        displayName = c.getString(2),
        sourceUri = c.getString(3),
        sourceKind = c.getString(4),
        sizeBytes = c.getLong(5),
        coverPath = if (c.isNull(6)) null else c.getString(6),
        importedAt = c.getLong(7),
        status = c.getString(8),
        missing = c.getInt(9) != 0,
        favorite = c.getInt(10) != 0,
        notes = c.getString(11),
    )

    fun log(level: String, msg: String) {
        val cv = ContentValues().apply {
            put("ts", System.currentTimeMillis())
            put("level", level)
            put("msg", msg.take(2000))
        }
        writableDatabase.insert("log", null, cv)
        writableDatabase.delete("log", "id <= (SELECT IFNULL(MAX(id),0)-500 FROM log)", null)
    }

    fun listLog(): List<LogRow> {
        val out = mutableListOf<LogRow>()
        readableDatabase.rawQuery(
            "SELECT id,ts,level,msg FROM log ORDER BY id DESC LIMIT 500", null
        ).use { c ->
            while (c.moveToNext()) {
                out.add(LogRow(c.getLong(0), c.getLong(1), c.getString(2), c.getString(3)))
            }
        }
        return out
    }

    fun clearLog() {
        writableDatabase.delete("log", null, null)
    }
}
