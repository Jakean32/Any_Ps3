package com.anyps3.app;

import android.content.Context;
import android.content.Intent;
import android.net.Uri;
import android.os.Build;
import android.os.Environment;
import android.os.storage.StorageManager;
import android.provider.DocumentsContract;
import android.system.StructStatVfs;
import android.system.Os;
import android.text.TextUtils;

import java.io.File;
import java.util.ArrayList;
import java.util.List;

/**
 * SAF helpers: tree-URI <-> raw path derivation, persistable permissions,
 * document-tree walking, and free-space queries.
 */
public final class SafUtils {

    private SafUtils() {}

    /** Take persistable read+write permission for a tree/folder/document URI. */
    public static void takePersistable(Context ctx, Uri uri, boolean forTree) {
        try {
            int flags = Intent.FLAG_GRANT_READ_URI_PERMISSION
                    | Intent.FLAG_GRANT_WRITE_URI_PERMISSION;
            ctx.getContentResolver().takePersistableUriPermission(uri, flags);
        } catch (SecurityException e) {
            // some pickers hand out non-persistable grants; keep going
        }
    }

    /**
     * Derive the raw filesystem path for a SAF tree URI on primary external
     * storage, e.g. content://com.android.externalstorage.documents/tree/primary%3AGames
     *   -> /storage/emulated/0/Games
     * Returns "" when the tree is not on primary external storage.
     */
    public static String treeUriToPath(Uri treeUri) {
        if (treeUri == null) return "";
        String docId = DocumentsContract.getTreeDocumentId(treeUri);
        if (docId == null) return "";
        // format: "primary:Path/To/Dir" or a raw volume UUID
        int colon = docId.indexOf(':');
        String volume = colon >= 0 ? docId.substring(0, colon) : docId;
        String path = colon >= 0 ? docId.substring(colon + 1) : "";
        if ("primary".equals(volume)) {
            String base = Environment.getExternalStorageDirectory().getAbsolutePath();
            return path.isEmpty() ? base : base + "/" + path;
        }
        return "";
    }

    /** True if the app can create files at this raw path (probe write). */
    public static boolean pathWritable(String dirPath) {
        if (TextUtils.isEmpty(dirPath)) return false;
        try {
            File dir = new File(dirPath);
            if (!dir.exists() && !dir.mkdirs()) return false;
            File probe = new File(dir, ".anyps3_probe");
            if (!probe.createNewFile()) return false;
            boolean ok = probe.delete();
            return ok || true;
        } catch (Exception e) {
            return false;
        }
    }

    public static boolean hasAllFilesAccess() {
        return Build.VERSION.SDK_INT < 30 || Environment.isExternalStorageManager();
    }

    public static Intent allFilesAccessSettingsIntent() {
        return new Intent(android.provider.Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                Uri.parse("package:" + "com.anyps3.app"));
    }

    /** Free space at the given path via statvfs (fallback: java.io.File). */
    public static long freeSpace(String path) {
        try {
            StructStatVfs st = Os.statvfs(path);
            return (long) st.f_bavail * (long) st.f_bsize;
        } catch (Exception e) {
            File f = new File(path);
            return f.getFreeSpace();
        }
    }

    public static String humanBytes(long b) {
        if (b < 1024) return b + " B";
        double v = b;
        String[] units = {"KB", "MB", "GB", "TB"};
        int u = -1;
        do { v /= 1024; u++; } while (v >= 1024 && u < units.length - 1);
        return String.format("%.1f %s", v, units[u]);
    }

    /**
     * Walk a SAF document tree (bounded depth) and list document IDs of
     * interest: .iso files and directories that look like PS3 games.
     */
    public static class DocEntry {
        public String docId;
        public String name;
        public boolean isDir;
        public long size;
        public String mime;
    }

    public static List<DocEntry> listChildren(Context ctx, Uri treeUri, String parentDocId) {
        List<DocEntry> out = new ArrayList<>();
        try {
            Uri childrenUri = DocumentsContract.buildChildDocumentsUriUsingTree(
                    treeUri, parentDocId);
            android.database.Cursor c = ctx.getContentResolver().query(
                    childrenUri,
                    new String[]{
                            DocumentsContract.Document.COLUMN_DOCUMENT_ID,
                            DocumentsContract.Document.COLUMN_DISPLAY_NAME,
                            DocumentsContract.Document.COLUMN_MIME_TYPE,
                            DocumentsContract.Document.COLUMN_SIZE},
                    null, null, null);
            if (c == null) return out;
            while (c.moveToNext()) {
                DocEntry e = new DocEntry();
                e.docId = c.getString(0);
                e.name = c.getString(1);
                e.mime = c.getString(2);
                e.size = c.getLong(3);
                e.isDir = DocumentsContract.Document.MIME_TYPE_DIR.equals(e.mime);
                out.add(e);
            }
            c.close();
        } catch (Exception ignored) {
        }
        return out;
    }

    /** Uri for a document inside a tree. */
    public static Uri documentUri(Uri treeUri, String docId) {
        return DocumentsContract.buildDocumentUriUsingTree(treeUri, docId);
    }

    /** Open a document's fd; -1 on failure (=> "file missing"). */
    public static int openFd(Context ctx, Uri docUri) {
        try {
            android.os.ParcelFileDescriptor pfd =
                    ctx.getContentResolver().openFileDescriptor(docUri, "r");
            if (pfd == null) return -1;
            return pfd.detachFd();
        } catch (Exception e) {
            return -1;
        }
    }

    /**
     * Presence check for a stored SAF URI, used after reboots:
     * returns fd >= 0 when the file can be opened for random reads,
     * 0 when the URI is a *directory* document (present, but no fd to
     * hand out), or -1 when the document is gone (=> "File missing").
     */
    public static int checkUriPresent(Context ctx, Uri uri) {
        int fd = openFd(ctx, uri);
        if (fd >= 0) return fd;
        // maybe a directory document: query its mime type
        try {
            android.database.Cursor c = ctx.getContentResolver().query(
                    uri,
                    new String[]{DocumentsContract.Document.COLUMN_MIME_TYPE},
                    null, null, null);
            if (c != null) {
                boolean has = c.moveToFirst();
                String mime = has ? c.getString(0) : null;
                c.close();
                if (DocumentsContract.Document.MIME_TYPE_DIR.equals(mime)) return 0;
                return has ? 0 : -1;  // row exists but not openable: keep as present
            }
        } catch (Exception ignored) {
        }
        return -1;
    }
}
