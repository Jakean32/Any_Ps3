package com.anyps3.app;

import android.app.Activity;
import android.app.AlertDialog;
import android.app.Dialog;
import android.content.Context;
import android.content.Intent;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.net.Uri;
import android.os.Bundle;
import android.view.LayoutInflater;
import android.view.View;
import android.view.ViewGroup;
import android.widget.BaseAdapter;
import android.widget.Button;
import android.widget.GridView;
import android.widget.ImageButton;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.ListView;
import android.widget.TextView;
import android.widget.Toast;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.File;
import java.util.ArrayList;
import java.util.List;

/**
 * Main shell: Library / Settings / Logs pages with a bottom navigation.
 * All data flows through the native core (no game data is ever copied).
 */
public class MainActivity extends Activity {

    private static final int REQ_IMPORT_ISOS = 51;
    private static final int REQ_SCAN_TREE = 52;

    private int page = 0;
    private View[] pages;
    private ImageView[] navIcons;
    private TextView[] navLabels;

    private GridView grid;
    private BaseAdapter adapter;
    private JSONArray entries = new JSONArray();
    private final List<Bitmap> coverCache = new ArrayList<>();

    // settings page widgets
    private TextView setFreeSpace, setAllfilesState, setAbout;
    private Button setAllfiles;

    // logs page
    private ArrayAdapterLogs logsAdapter;

    // compile progress dialog
    private Dialog progressDialog;
    private TextView progNote, progTitle;

    private long h() {
        return AnyPs3App.get(this).library();
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_main);

        // config: restore cache root
        AnyPs3App app = AnyPs3App.get(this);
        try {
            JSONObject cfg = new JSONObject(NativeBridge.nativeConfigJson(h()));
            String root = cfg.optString("cache_root", "");
            if (!root.isEmpty()) app.ensureLibrary(root);
        } catch (Exception ignored) {
        }

        pages = new View[3];
        LayoutInflater inf = getLayoutInflater();
        pages[0] = inf.inflate(R.layout.page_library, null);
        pages[1] = inf.inflate(R.layout.page_settings, null);
        pages[2] = inf.inflate(R.layout.page_logs, null);

        ViewGroup frame = findViewById(R.id.content_frame);
        for (View p : pages) {
            frame.addView(p, new ViewGroup.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
            p.setVisibility(View.GONE);
        }

        navIcons = new ImageView[]{
                findViewById(R.id.nav_library_icon),
                findViewById(R.id.nav_settings_icon),
                findViewById(R.id.nav_logs_icon)};
        navLabels = new TextView[]{
                findViewById(R.id.nav_library_label),
                findViewById(R.id.nav_settings_label),
                findViewById(R.id.nav_logs_label)};
        bindNav(R.id.nav_library, 0);
        bindNav(R.id.nav_settings, 1);
        bindNav(R.id.nav_logs, 2);

        setupLibraryPage();
        setupSettingsPage();
        setupLogsPage();

        showPage(0, false);
        // start with a refresh so statuses are current
        refreshLibrary();
    }

    private void bindNav(int id, final int idx) {
        findViewById(id).setOnClickListener(v -> showPage(idx, true));
    }

    private void showPage(int idx, boolean animate) {
        for (int i = 0; i < pages.length; i++) {
            pages[i].setVisibility(i == idx ? View.VISIBLE : View.GONE);
            navIcons[i].setAlpha(i == idx ? 1f : 0.45f);
            navLabels[i].setTextColor(idx == i
                    ? getColor(R.color.accent_gold_bright)
                    : getColor(R.color.nav_inactive));
        }
        page = idx;
        if (idx == 0) refreshLibrary();
        if (idx == 1) refreshSettings();
        if (idx == 2) refreshLogs();
    }

    // ==================== LIBRARY ====================

    private void setupLibraryPage() {
        grid = pages[0].findViewById(R.id.game_grid);
        View empty = pages[0].findViewById(R.id.empty_state);
        adapter = new BaseAdapter() {
            @Override public int getCount() {
                return entries.length();
            }
            @Override public Object getItem(int position) {
                return entries.opt(position);
            }
            @Override public long getItemId(int position) {
                return position;
            }
            @Override public View getView(int position, View cv, ViewGroup parent) {
                View v = cv;
                if (v == null) {
                    v = getLayoutInflater().inflate(R.layout.item_game, parent, false);
                }
                JSONObject e = entries.optJSONObject(position);
                if (e == null) return v;
                ((TextView) v.findViewById(R.id.game_title)).setText(e.optString("title"));
                ((TextView) v.findViewById(R.id.game_id)).setText(e.optString("title_id"));
                TextView status = v.findViewById(R.id.game_status);
                String statusName = e.optString("status_name", "Not compiled");
                status.setText(e.optBoolean("missing")
                        ? getString(R.string.status_missing) : statusName);
                status.setTextColor(getColor(statusColor(e)));
                ImageView cover = v.findViewById(R.id.game_cover);
                String icon = e.optString("icon_path", "");
                if (!icon.isEmpty() && new File(icon).exists()) {
                    Bitmap b = decodeSampled(icon, 320, 96);
                    if (b != null) cover.setImageBitmap(b);
                } else {
                    cover.setImageResource(R.drawable.cover_placeholder);
                }
                v.setOnClickListener(x -> showDetail(e));
                return v;
            }
        };
        grid.setAdapter(adapter);
        grid.setEmptyView(empty);

        pages[0].findViewById(R.id.action_import_iso).setOnClickListener(v -> {
            Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
            i.addCategory(Intent.CATEGORY_OPENABLE);
            i.setType("*/*");
            i.putExtra(Intent.EXTRA_ALLOW_MULTIPLE, true);   // multi-select
            i.putExtra(Intent.EXTRA_MIME_TYPES, new String[]{"application/octet-stream",
                    "application/x-iso9660-image", "application/x-cd-image"});
            i.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION
                    | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
            startActivityForResult(i, REQ_IMPORT_ISOS);
        });
        pages[0].findViewById(R.id.action_scan).setOnClickListener(v -> {
            Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
            i.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION
                    | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION
                    | Intent.FLAG_GRANT_WRITE_URI_PERMISSION);
            startActivityForResult(i, REQ_SCAN_TREE);
        });
    }

    private int statusColor(JSONObject e) {
        if (e.optBoolean("missing")) return R.color.status_missing;
        switch (e.optString("status", "not_compiled")) {
            case "ready": return R.color.status_ready;
            case "stale": return R.color.status_stale;
            case "compiling": return R.color.status_compiling;
            case "failed": return R.color.status_failed;
            default: return R.color.status_not_compiled;
        }
    }

    private void refreshLibrary() {
        new Thread(() -> {
            try {
                // SAF records need the platform to re-open their URIs
                // (persistable permission survives reboot) so the core can
                // mark them present/missing and refresh sizes.
                String json0 = NativeBridge.nativeEntriesJson(h());
                org.json.JSONArray cur = new org.json.JSONArray(json0);
                for (int i = 0; i < cur.length(); i++) {
                    org.json.JSONObject e = cur.optJSONObject(i);
                    if (e == null || !"saf_uri".equals(e.optString("src_type"))) continue;
                    String ref = e.optString("src_ref", "");
                    if (ref.isEmpty()) continue;
                    try {
                        int fd = SafUtils.checkUriPresent(this, Uri.parse(ref));
                        NativeBridge.nativeUpdateSafRecord(h(), ref, fd);
                    } catch (Exception ignored) {
                    }
                }
                NativeBridge.nativeRefresh(h());
            } catch (Exception ignored) {
            }
            final String json = NativeBridge.nativeEntriesJson(h());
            runOnUiThread(() -> {
                try {
                    entries = new JSONArray(json);
                    adapter.notifyDataSetChanged();
                    TextView hint = pages[0].findViewById(R.id.hint_bar);
                    boolean anyMissing = false;
                    for (int i = 0; i < entries.length(); i++)
                        anyMissing |= entries.optJSONObject(i).optBoolean("missing");
                    hint.setVisibility(anyMissing ? View.VISIBLE : View.GONE);
                    if (anyMissing) hint.setText(R.string.missing_hint);
                } catch (Exception e) {
                    Toast.makeText(this, "library: " + e.getMessage(), Toast.LENGTH_SHORT).show();
                }
            });
        }).start();
    }

    private Bitmap decodeSampled(String path, int maxW, int maxH) {
        BitmapFactory.Options o = new BitmapFactory.Options();
        o.inJustDecodeBounds = true;
        BitmapFactory.decodeFile(path, o);
        int sample = 1;
        while (o.outWidth / (sample * 2) >= maxW && o.outHeight / (sample * 2) >= maxH)
            sample *= 2;
        BitmapFactory.Options o2 = new BitmapFactory.Options();
        o2.inSampleSize = sample;
        return BitmapFactory.decodeFile(path, o2);
    }

    // ---------- import results ----------

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (resultCode != RESULT_OK || data == null) return;
        if (requestCode == REQ_IMPORT_ISOS) {
            List<Uri> uris = new ArrayList<>();
            if (data.getClipData() != null) {
                for (int i = 0; i < data.getClipData().getItemCount(); i++)
                    uris.add(data.getClipData().getItemAt(i).getUri());
            } else if (data.getData() != null) {
                uris.add(data.getData());
            }
            importIsos(uris);
        } else if (requestCode == REQ_SCAN_TREE) {
            scanTree(data.getData());
        }
    }

    private void importIsos(List<Uri> uris) {
        final int total = uris.size();
        for (Uri u : uris) SafUtils.takePersistable(this, u, false);  // survives reboot
        Toast.makeText(this, "Importing " + total + " item(s)…", Toast.LENGTH_SHORT).show();
        new Thread(() -> {
            int ok = 0;
            for (Uri u : uris) {
                int fd = SafUtils.openFd(this, u);
                if (fd < 0) continue;
                try {
                    if (NativeBridge.nativeImportIsoFd(h(), u.toString(), fd)) ok++;
                } catch (Exception ignored) {
                }
            }
            final int fok = ok;
            runOnUiThread(() -> {
                Toast.makeText(this, "Imported " + fok + "/" + total
                        + (fok < total ? " (not PS3 images?)" : ""), Toast.LENGTH_SHORT).show();
                refreshLibrary();
            });
        }).start();
    }

    private void scanTree(Uri treeUri) {
        SafUtils.takePersistable(this, treeUri, true);
        Toast.makeText(this, R.string.scan_folder, Toast.LENGTH_SHORT).show();
        new Thread(() -> {
            String raw = SafUtils.treeUriToPath(treeUri);
            int imported = 0;
            if (!raw.isEmpty() && new File(raw).canRead()) {
                // fast path: real filesystem scan in native code
                try {
                    org.json.JSONObject st = new org.json.JSONObject(
                            NativeBridge.nativeScanFolder(h(), raw, true));
                    imported = st.optInt("imported", 0) + st.optInt("updated", 0);
                } catch (Exception ignored) {
                }
            } else {
                // SAF walk (no raw access): one tree, two levels deep
                imported = scanSaf(treeUri, DocumentsContractRoot(treeUri));
            }
            final int fi = imported;
            runOnUiThread(() -> {
                Toast.makeText(this, fi + " game(s) found", Toast.LENGTH_SHORT).show();
                refreshLibrary();
            });
        }).start();
    }

    private String DocumentsContractRoot(Uri treeUri) {
        return android.provider.DocumentsContract.getTreeDocumentId(treeUri);
    }

    private int scanSaf(Uri treeUri, String rootDocId) {
        int imported = 0;
        for (SafUtils.DocEntry e : SafUtils.listChildren(this, treeUri, rootDocId)) {
            String nameLower = e.name == null ? "" : e.name.toLowerCase();
            if (e.isDir) {
                // folder game: <dir>/PS3_GAME/PARAM.SFO (one level down)
                for (SafUtils.DocEntry c : SafUtils.listChildren(this, treeUri, e.docId)) {
                    if ("ps3_game".equalsIgnoreCase(c.name)) {
                        for (SafUtils.DocEntry g : SafUtils.listChildren(this, treeUri, c.docId)) {
                            if ("param.sfo".equalsIgnoreCase(g.name)) {
                                // it is a folder game: import via its parent document
                                // (native import needs an ISO or a real path; for SAF
                                // folder games we construct a pseudo-ref and rely on
                                // the PARAM.SFO fd at import time)
                                importFolderDoc(treeUri, e);
                                break;
                            }
                        }
                        break;
                    }
                }
            } else if (nameLower.endsWith(".iso")) {
                Uri doc = SafUtils.documentUri(treeUri, e.docId);
                SafUtils.takePersistable(this, doc, false);
                int fd = SafUtils.openFd(this, doc);
                if (fd >= 0) {
                    try {
                        if (NativeBridge.nativeImportIsoFd(h(), doc.toString(), fd)) imported++;
                    } catch (Exception ignored) {
                    }
                }
            }
        }
        return imported;
    }

    /** Folder games via SAF: open PARAM.SFO / ICON0 / PIC1 fds, import natively. */
    private void importFolderDoc(Uri treeUri, SafUtils.DocEntry dir) {
        String gameDirUri = SafUtils.documentUri(treeUri, dir.docId).toString();
        for (SafUtils.DocEntry c : SafUtils.listChildren(this, treeUri, dir.docId)) {
            if ("ps3_game".equalsIgnoreCase(c.name)) {
                int sfoFd = -1, iconFd = -1, pic1Fd = -1;
                String sfoUri = "";
                for (SafUtils.DocEntry g : SafUtils.listChildren(this, treeUri, c.docId)) {
                    String n = g.name == null ? "" : g.name.toLowerCase();
                    if ("param.sfo".equalsIgnoreCase(n)) {
                        sfoUri = SafUtils.documentUri(treeUri, g.docId).toString();
                        sfoFd = SafUtils.openFd(this, SafUtils.documentUri(treeUri, g.docId));
                    } else if ("icon0.png".equalsIgnoreCase(n)) {
                        iconFd = SafUtils.openFd(this, SafUtils.documentUri(treeUri, g.docId));
                    } else if ("pic1.png".equalsIgnoreCase(n)) {
                        pic1Fd = SafUtils.openFd(this, SafUtils.documentUri(treeUri, g.docId));
                    }
                }
                if (sfoFd >= 0) {
                    try {
                        NativeBridge.nativeImportSfoFd(h(), gameDirUri, sfoUri, sfoFd, iconFd,
                                pic1Fd);
                    } catch (Exception ignored) {
                    }
                }
                return;
            }
        }
    }

    // ---------- detail sheet ----------

    private void showDetail(JSONObject e) {
        String id = e.optString("title_id");
        View sheet = getLayoutInflater().inflate(R.layout.sheet_game_detail, null);
        ImageView cover = sheet.findViewById(R.id.detail_cover);
        String icon = e.optString("icon_path", "");
        if (!icon.isEmpty() && new File(icon).exists()) {
            Bitmap b = decodeSampled(icon, 640, 260);
            if (b != null) cover.setImageBitmap(b);
        }
        ((TextView) sheet.findViewById(R.id.detail_title)).setText(e.optString("title"));
        ((TextView) sheet.findViewById(R.id.detail_subtitle)).setText(
                e.optString("title_id") + " · " + e.optString("app_ver", "-") + " · "
                        + e.optString("src_type", ""));
        TextView note = sheet.findViewById(R.id.detail_status_note);
        String noteText = e.optString("note", "");
        if (e.optBoolean("missing")) {
            note.setText(getString(R.string.status_missing) + "\n" + getString(R.string.missing_hint));
            note.setTextColor(getColor(R.color.status_missing));
        } else {
            note.setText(noteText.isEmpty()
                    ? "Analysis not run yet — press Compile to scan the game's EBOOT.BIN." : noteText);
            note.setTextColor(getColor(R.color.text_secondary));
        }

        // stage list from the manifest (honest per-stage statuses)
        LinearLayout stages = sheet.findViewById(R.id.detail_stages);
        try {
            JSONArray st = e.optJSONArray("stages");
            if (st != null) {
                for (int i = 0; i < st.length(); i++) {
                    JSONObject s = st.optJSONObject(i);
                    TextView tv = new TextView(this);
                    String mark = "•";
                    String col = "#8A93A3";
                    switch (s.optString("status")) {
                        case "ok": mark = "✓"; col = "#5FBF77"; break;
                        case "not_implemented": mark = "—"; col = "#8A93A3"; break;
                        case "blocked": mark = "!"; col = "#D98C3F"; break;
                        case "failed": mark = "✕"; col = "#C75B5B"; break;
                    }
                    String label = s.optString("name", "?");
                    if ("not_implemented".equals(s.optString("status")))
                        label += " — " + getString(R.string.stage_not_implemented_msg);
                    tv.setText(mark + "  " + label);
                    tv.setTextColor(android.graphics.Color.parseColor(col));
                    tv.setTextSize(12);
                    tv.setTypeface(android.graphics.Typeface.MONOSPACE);
                    stages.addView(tv);
                }
            }
        } catch (Exception ignored) {
        }

        Dialog dlg = new Dialog(this);
        dlg.setContentView(sheet);
        android.view.Window w = dlg.getWindow();
        if (w != null) {
            w.setBackgroundDrawable(new android.graphics.drawable.ColorDrawable(
                    android.graphics.Color.TRANSPARENT));
            w.setLayout(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
            w.setGravity(android.view.Gravity.BOTTOM);
            w.setWindowAnimations(android.R.style.Animation_InputMethod);
        }

        sheet.findViewById(R.id.detail_compile).setOnClickListener(v -> {
            dlg.dismiss();
            startCompile(id, false);
        });
        sheet.findViewById(R.id.detail_launch).setOnClickListener(v -> {
            Toast.makeText(this, R.string.launch_not_ready, Toast.LENGTH_LONG).show();
        });
        sheet.findViewById(R.id.detail_delete_cache).setOnClickListener(v -> {
            NativeBridge.nativeDeleteCache(h(), id);
            Toast.makeText(this, "Cache deleted", Toast.LENGTH_SHORT).show();
            dlg.dismiss();
            refreshLibrary();
        });
        sheet.findViewById(R.id.detail_remove).setOnClickListener(v -> {
            new AlertDialog.Builder(this)
                    .setMessage("Remove " + id + " from the library? The game files themselves are never touched.")
                    .setPositiveButton(R.string.ok, (d, w2) -> {
                        NativeBridge.nativeRemoveGame(h(), id, true);
                        dlg.dismiss();
                        refreshLibrary();
                    })
                    .setNegativeButton(R.string.cancel, null)
                    .show();
        });
        dlg.show();
    }

    private void startCompile(String id, boolean recompile) {
        View v = getLayoutInflater().inflate(R.layout.dialog_progress, null);
        progressDialog = new AlertDialog.Builder(this).setView(v).setCancelable(true).create();
        progressDialog.show();
        progTitle = v.findViewById(R.id.prog_title);
        progNote = v.findViewById(R.id.prog_note);
        android.widget.ProgressBar bar = v.findViewById(R.id.prog_bar);
        bar.setMax(1000);

        NativeBridge.setProgressListener((stage, fraction, note) -> runOnUiThread(() -> {
            if (progressDialog == null) return;
            progTitle.setText((recompile ? R.string.action_recompile : R.string.action_compile)
                    + " — " + stage);
            bar.setProgress((int) (fraction * 1000));
            progNote.setText(note);
        }));

        new Thread(() -> {
            boolean ok = recompile
                    ? NativeBridge.nativeRecompile(h(), id)
                    : NativeBridge.nativeCompile(h(), id);
            NativeBridge.setProgressListener(null);
            runOnUiThread(() -> {
                if (progressDialog != null) {
                    try { progressDialog.dismiss(); } catch (Exception ignored) {}
                    progressDialog = null;
                }
                Toast.makeText(this, ok ? "Compile run finished — see Library status"
                        : "Compile failed (see Logs)", Toast.LENGTH_SHORT).show();
                refreshLibrary();
                refreshLogs();
            });
        }).start();
    }

    // ==================== SETTINGS ====================

    private void setupSettingsPage() {
        View p = pages[1];
        setFreeSpace = p.findViewById(R.id.set_free_space);
        setAllfiles = p.findViewById(R.id.set_allfiles);
        setAllfilesState = p.findViewById(R.id.set_allfiles_state);
        setAbout = p.findViewById(R.id.set_about);
        p.findViewById(R.id.set_cache_folder).setOnClickListener(v -> {
            Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
            i.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION
                    | Intent.FLAG_GRANT_WRITE_URI_PERMISSION
                    | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
            startActivityForResult(i, REQ_SCAN_TREE);
        });
        p.findViewById(R.id.set_scan).setOnClickListener(v -> {
            try {
                JSONObject cfg = new JSONObject(NativeBridge.nativeConfigJson(h()));
                String raw = cfg.optString("cache_raw_path", "");
                if (!raw.isEmpty() && new File(raw).canRead()) {
                    new Thread(() -> {
                        NativeBridge.nativeScanFolder(h(), raw, true);
                        runOnUiThread(() ->
                                Toast.makeText(this, "Scan finished", Toast.LENGTH_SHORT).show());
                    }).start();
                } else {
                    Toast.makeText(this, "Pick the folder first (Scan folder above)",
                            Toast.LENGTH_SHORT).show();
                }
            } catch (Exception ignored) {
            }
        });
        p.findViewById(R.id.set_import_iso).setOnClickListener(v -> {
            Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
            i.addCategory(Intent.CATEGORY_OPENABLE);
            i.setType("*/*");
            i.putExtra(Intent.EXTRA_ALLOW_MULTIPLE, true);
            i.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION
                    | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
            startActivityForResult(i, REQ_IMPORT_ISOS);
        });
        setAllfiles.setOnClickListener(v -> {
            if (!SafUtils.hasAllFilesAccess()) {
                try {
                    startActivity(SafUtils.allFilesAccessSettingsIntent());
                } catch (Exception e) {
                    Toast.makeText(this, "Open Settings → All files access for Any_Ps3",
                            Toast.LENGTH_LONG).show();
                }
            } else {
                Toast.makeText(this, "Already granted", Toast.LENGTH_SHORT).show();
            }
        });
        p.findViewById(R.id.set_wizard).setOnClickListener(v -> {
            startActivity(new Intent(this, WizardActivity.class));
            overridePendingTransition(android.R.anim.fade_in, android.R.anim.fade_out);
        });
        p.findViewById(R.id.set_legal).setOnClickListener(v ->
                new AlertDialog.Builder(this)
                        .setTitle(R.string.settings_legal)
                        .setMessage(com.anyps3.app.AnyPs3App.LEGAL_TEXT)
                        .setPositiveButton(R.string.ok, null).show());
    }

    private void refreshSettings() {
        View p = pages[1];
        try {
            JSONObject cfg = new JSONObject(NativeBridge.nativeConfigJson(h()));
            String root = cfg.optString("cache_root", "");
            boolean direct = cfg.optBoolean("cache_is_direct", false);
            long free = NativeBridge.nativeFreeSpaceBytes(h());
            setFreeSpace.setText(getString(R.string.settings_space_fmt, SafUtils.humanBytes(free))
                    + "\n" + (direct ? "Writing directly to: " : "Cache location: ") + root);
            boolean all = SafUtils.hasAllFilesAccess();
            setAllfilesState.setText(all ? R.string.settings_allfiles_on
                    : R.string.settings_allfiles_off);
            setAbout.setText(String.format(getString(R.string.version_fmt),
                    NativeBridge.nativeVersion(), "App Shell"));
        } catch (Exception ignored) {
        }
        TextView ex = p.findViewById(R.id.set_exec_strategy);
        ex.setText("Executable-code strategy (Android)\n\n"
                + "Compiled artifacts are stored as plain data files — Android's shared "
                + "storage never allows executable mappings. At launch they would be "
                + "copied into the private app directory where execution is permitted. "
                + "The artifact loader is not implemented yet; the UI reports this honestly "
                + "when reached. Cache manifest format v1, statuses: not_compiled / "
                + "compiling / ready / stale / failed.");
    }

    // ==================== LOGS ====================

    private void setupLogsPage() {
        View p = pages[2];
        ListView list = p.findViewById(R.id.log_list);
        logsAdapter = new ArrayAdapterLogs(this);
        list.setAdapter(logsAdapter);
        p.findViewById(R.id.log_clear).setOnClickListener(v -> logsAdapter.clear());
    }

    private void refreshLogs() {
        if (logsAdapter == null) return;
        try {
            String raw = NativeBridge.nativeUiLog(h());
            JSONArray arr = new JSONArray(raw);
            logsAdapter.set(arr);
        } catch (Exception ignored) {
        }
    }

    private static class ArrayAdapterLogs extends android.widget.BaseAdapter {
        private final Context ctx;
        private final List<String> lines = new ArrayList<>();

        ArrayAdapterLogs(Context ctx) { this.ctx = ctx; }

        void set(JSONArray arr) {
            lines.clear();
            for (int i = 0; i < arr.length(); i++) lines.add(arr.optString(i));
            notifyDataSetChanged();
        }

        void clear() {
            lines.clear();
            notifyDataSetChanged();
        }

        @Override public int getCount() { return lines.size(); }
        @Override public Object getItem(int i) { return lines.get(i); }
        @Override public long getItemId(int i) { return i; }
        @Override public View getView(int i, View cv, ViewGroup parent) {
            TextView tv = (TextView) cv;
            if (tv == null) {
                tv = new TextView(ctx);
                tv.setTypeface(android.graphics.Typeface.MONOSPACE);
                tv.setTextSize(11);
                tv.setTextColor(0xFFA8A29A);
            }
            tv.setText(lines.get(i));
            return tv;
        }
    }
}
