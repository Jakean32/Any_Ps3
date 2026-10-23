package com.anyps3.app;

import android.app.Activity;
import android.content.Context;
import android.content.Intent;
import android.net.Uri;
import android.os.Bundle;
import android.view.View;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.TextView;
import android.widget.Toast;
import android.widget.ViewFlipper;

/**
 * First-run wizard (runs only until config marks it done; re-runnable from
 * Settings): welcome -> cache folder (ACTION_OPEN_DOCUMENT_TREE + persistent
 * read/write grant + free space check with warning) -> legal notice.
 */
public class WizardActivity extends Activity {

    private static final int REQ_PICK_TREE = 41;

    private ViewFlipper flipper;
    private Button back, next;
    private TextView freeSpace, spaceWarning, cacheNote, cacheRestricted;
    private CheckBox accept;

    private Uri pendingTreeUri;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_wizard);

        flipper = findViewById(R.id.wizard_flipper);
        back = findViewById(R.id.wizard_back);
        next = findViewById(R.id.wizard_next);
        freeSpace = findViewById(R.id.wizard_free_space);
        spaceWarning = findViewById(R.id.wizard_space_warning);
        cacheNote = findViewById(R.id.wizard_cache_note);
        cacheRestricted = findViewById(R.id.wizard_cache_restricted);
        accept = findViewById(R.id.wizard_accept);

        findViewById(R.id.wizard_pick_folder).setOnClickListener(v -> pickFolder());
        back.setOnClickListener(v -> {
            if (flipper.getDisplayedChild() > 0) flipper.showPrevious();
            updateButtons();
        });
        next.setOnClickListener(v -> {
            int cur = flipper.getDisplayedChild();
            if (cur == 1 && pendingTreeUri == null) {
                Toast.makeText(this, R.string.wizard_pick_folder, Toast.LENGTH_SHORT).show();
                return;
            }
            if (cur == 2) {
                if (!accept.isChecked()) {
                    Toast.makeText(this, R.string.wizard_accept, Toast.LENGTH_SHORT).show();
                    return;
                }
                finishWizard();
                return;
            }
            flipper.showNext();
            updateButtons();
        });
        updateButtons();
    }

    private void updateButtons() {
        int cur = flipper.getDisplayedChild();
        back.setVisibility(cur == 0 ? View.INVISIBLE : View.VISIBLE);
        next.setText(cur == 2 ? R.string.wizard_finish : R.string.wizard_next);
    }

    private void pickFolder() {
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
        i.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION
                | Intent.FLAG_GRANT_WRITE_URI_PERMISSION
                | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
        startActivityForResult(i, REQ_PICK_TREE);
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode != REQ_PICK_TREE) return;
        if (resultCode != RESULT_OK || data == null || data.getData() == null) return;

        pendingTreeUri = data.getData();
        // persistent read/write access across reboots (requirement)
        SafUtils.takePersistable(this, pendingTreeUri, true);

        String rawPath = SafUtils.treeUriToPath(pendingTreeUri);
        boolean writable = SafUtils.pathWritable(rawPath);
        long free = SafUtils.freeSpace(writable && !rawPath.isEmpty() ? rawPath
                : AnyPs3App.get(this).defaultCacheRoot());
        freeSpace.setVisibility(View.VISIBLE);
        freeSpace.setText(getString(R.string.wizard_free_space_fmt, SafUtils.humanBytes(free)));
        boolean low = free < 2L * 1024 * 1024 * 1024;  // < 2 GiB recommendation
        spaceWarning.setVisibility(low ? View.VISIBLE : View.GONE);
        cacheNote.setVisibility(View.VISIBLE);
        cacheNote.setText(getString(R.string.wizard_cache_note_fmt,
                rawPath.isEmpty() ? pendingTreeUri.toString() : rawPath));
        cacheRestricted.setVisibility(writable ? View.GONE : View.VISIBLE);
    }

    private void finishWizard() {
        AnyPs3App app = AnyPs3App.get(this);
        // decide real cache root: user folder when directly writable,
        // otherwise the app's own storage (honest fallback, shown in UI)
        String raw = SafUtils.treeUriToPath(pendingTreeUri);
        String cacheRoot = (SafUtils.pathWritable(raw) && !raw.isEmpty())
                ? raw + "/AnyPs3_Cache" : app.defaultCacheRoot();

        app.ensureLibrary(cacheRoot);
        long h = app.library();
        NativeBridge.nativeConfigSet(h, "cache_root", cacheRoot);
        NativeBridge.nativeConfigSet(h, "cache_tree_uri", pendingTreeUri.toString());
        NativeBridge.nativeConfigSet(h, "cache_raw_path", raw);
        NativeBridge.nativeConfigSet(h, "cache_is_direct",
                String.valueOf(!raw.isEmpty() && cacheRoot.startsWith(raw)));
        NativeBridge.nativeConfigSet(h, "wizard_done", "true");
        NativeBridge.nativeConfigSet(h, "legal_version_accepted", AnyPs3App.LEGAL_VERSION);

        Toast.makeText(this, R.string.wizard_finish, Toast.LENGTH_SHORT).show();
        startActivity(new Intent(this, MainActivity.class));
        overridePendingTransition(android.R.anim.fade_in, android.R.anim.fade_out);
        finish();
    }
}
