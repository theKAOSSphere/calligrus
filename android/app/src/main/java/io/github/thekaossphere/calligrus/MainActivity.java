package io.github.thekaossphere.calligrus;

import android.content.ActivityNotFoundException;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.UriPermission;
import android.database.Cursor;
import android.net.Uri;
import android.os.Bundle;
import android.provider.OpenableColumns;
import android.util.Log;
import android.widget.Toast;

import androidx.activity.OnBackPressedCallback;
import androidx.core.view.WindowCompat;
import androidx.core.view.WindowInsetsControllerCompat;

import com.google.androidgamesdk.GameActivity;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.util.List;

/**
 * The window comes from GameActivity and everything on it is drawn by C++ (android_main). This class
 * does what only Java can: the document picker, reading files other apps hand over ("Open with",
 * share), opening links, and the colour of the status bar icons. A document goes to native code as its
 * display name and its bytes. It also remembers where each private copy came from, so that a restored tab can read its
 * original again when Android still lets the app (a persistable permission).
 */
public class MainActivity extends GameActivity {
    static {
        System.loadLibrary("calligrus");
    }

    private static final int REQUEST_OPEN = 1;
    private static final long MAX_BYTES = 64L * 1024 * 1024;

    /**
     * Hands a document to the app: the file name (its extension picks Markdown or Org) and the bytes. `replaces` is
     * the private copy this document was last read into (same source), if any: a tab showing it takes the new text.
     * `refresh`: a re-read at start-up, nothing happens when the text is the same. Returns the private copy's path.
     */
    private static native String nativeOpenDocument(String name, byte[] data, String replaces, boolean refresh);

    /** The Back key: true if the app takes it (the tab list is open and closes), false to leave as usual. */
    private static native boolean nativeBack();

    /** A restored tab's original: `stale` = it could not be read again (the tab shows a note); else it was picked again. */
    private static native void nativeSourceState(String copy, boolean stale);

    private static final String TAG = "calligrus";

    /** The private copy of the tab whose original the picker is now asked for ("Reopen"), else empty. */
    private String reopenCopy = "";

    /** private copy path -> source URI ("p:...") and source URI -> latest private copy ("u:..."). */
    private SharedPreferences sources() {
        return getSharedPreferences("sources", MODE_PRIVATE);
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        // Back closes the tab list while it is open; otherwise it does what it always did (leave).
        getOnBackPressedDispatcher().addCallback(this, new OnBackPressedCallback(true) {
            @Override
            public void handleOnBackPressed() {
                if (nativeBack()) return;
                setEnabled(false);
                getOnBackPressedDispatcher().onBackPressed();
                setEnabled(true);
            }
        });
        handleIntent(getIntent());
    }

    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        setIntent(intent);
        handleIntent(intent);
    }

    /** "Open with" (VIEW) or "Share" (SEND) from another app. */
    @SuppressWarnings("deprecation")
    private void handleIntent(Intent intent) {
        if (intent == null) return;
        final String action = intent.getAction();
        if (Intent.ACTION_VIEW.equals(action) && intent.getData() != null) {
            openUri(intent.getData());
        } else if (Intent.ACTION_SEND.equals(action)) {
            Uri stream = intent.getParcelableExtra(Intent.EXTRA_STREAM);
            String text = intent.getStringExtra(Intent.EXTRA_TEXT);
            if (stream != null) {
                openUri(stream);
            } else if (text != null) {
                nativeOpenDocument("shared.md", text.getBytes(StandardCharsets.UTF_8), "", false);
            }
        }
    }

    /** Reads the document on a thread of its own (a big file must not block the window). */
    private void openUri(final Uri uri) {
        openUri(uri, "");
    }

    /** `reopen`: the private copy of the tab this document is to replace (picked again for a stale tab), or empty. */
    private void openUri(final Uri uri, final String reopen) {
        // Ask to keep read access across restarts (the picker grants it; "Open with" only when the sender offers it).
        try {
            getContentResolver().takePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION);
        } catch (SecurityException | UnsupportedOperationException e) {
            // not offered: the tab will be restored from the private copy
        }
        new Thread(() -> {
            try {
                byte[] bytes = readAll(uri);
                String key = uri.toString();
                SharedPreferences prefs = sources();
                String old = reopen.isEmpty() ? prefs.getString("u:" + key, "") : reopen;
                String previous = reopen.isEmpty() ? null : prefs.getString("p:" + reopen, null);
                String stored = nativeOpenDocument(displayName(uri), bytes, old, false);
                if (stored != null && !stored.isEmpty()) {
                    remember(prefs, key, stored, old);
                    if (!reopen.isEmpty()) {
                        // another file than the tab had: the old source is let go (unless another tab shows it)
                        if (previous != null && !previous.equals(key) && prefs.getString("u:" + previous, "").equals(reopen)) {
                            prefs.edit().remove("u:" + previous).apply();
                            releasePermission(Uri.parse(previous));
                        }
                        nativeSourceState(stored, false);
                    }
                }
            } catch (IOException | RuntimeException e) {
                runOnUiThread(() -> Toast.makeText(this, "Cannot open the file: " + e.getMessage(), Toast.LENGTH_LONG).show());
            }
        }, "calligrus-open").start();
    }

    /** The whole stream (up to MAX_BYTES). */
    private byte[] readAll(Uri uri) throws IOException {
        try (InputStream in = getContentResolver().openInputStream(uri)) {
            if (in == null) throw new IOException("no stream");
            ByteArrayOutputStream out = new ByteArrayOutputStream();
            byte[] buf = new byte[64 * 1024];
            long total = 0;
            for (int n; (n = in.read(buf)) > 0; ) {
                total += n;
                if (total > MAX_BYTES) throw new IOException("file too large");
                out.write(buf, 0, n);
            }
            return out.toByteArray();
        }
    }

    private static void remember(SharedPreferences prefs, String uri, String stored, String old) {
        SharedPreferences.Editor e = prefs.edit();
        if (!old.isEmpty() && !old.equals(stored)) e.remove("p:" + old);
        e.putString("u:" + uri, stored).putString("p:" + stored, uri).apply();
    }

    /** Lets go of the persisted read permission for `uri` (if the app holds one). */
    private void releasePermission(Uri uri) {
        if (!canReadAgain(uri)) return;
        try {
            getContentResolver().releasePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION);
            Log.i(TAG, "released the read permission of " + uri);
        } catch (SecurityException | UnsupportedOperationException e) {
            Log.i(TAG, "could not release " + uri + ": " + e.getMessage());
        }
    }

    /**
     * A tab was closed (spec 35). `list`: first line the closed tab's private copy, then the copies of the tabs still open.
     * Its source is forgotten and its permission released, unless another open tab shows the same source.
     */
    public void releaseSource(final String list) {
        String[] lines = list.split("\n");
        if (lines.length == 0 || lines[0].isEmpty()) return;
        SharedPreferences prefs = sources();
        String closed = lines[0];
        String src = prefs.getString("p:" + closed, null);
        if (src == null) return;
        SharedPreferences.Editor e = prefs.edit().remove("p:" + closed);
        for (int i = 1; i < lines.length; i++) {
            if (lines[i].isEmpty() || !src.equals(prefs.getString("p:" + lines[i], null))) continue;
            if (closed.equals(prefs.getString("u:" + src, ""))) e.putString("u:" + src, lines[i]);  // (the source's latest copy is still open)
            e.apply();
            Log.i(TAG, "kept the read permission of " + src + " (another tab shows it)");
            return;
        }
        if (closed.equals(prefs.getString("u:" + src, ""))) e.remove("u:" + src);
        e.apply();
        releasePermission(Uri.parse(src));
    }

    /** Private copies the app deleted (the retention limit): their sources are forgotten; one no newer copy replaced is released. */
    public void forgetCopies(final String paths) {
        SharedPreferences prefs = sources();
        for (String path : paths.split("\n")) {
            if (path.isEmpty()) continue;
            String src = prefs.getString("p:" + path, null);
            if (src == null) continue;
            prefs.edit().remove("p:" + path).apply();
            if (path.equals(prefs.getString("u:" + src, ""))) {
                prefs.edit().remove("u:" + src).apply();
                releasePermission(Uri.parse(src));
            }
        }
    }

    /** Does the app still hold a persisted read permission for this source? */
    private boolean canReadAgain(Uri uri) {
        if (!"content".equals(uri.getScheme())) return false;
        List<UriPermission> held = getContentResolver().getPersistedUriPermissions();
        for (UriPermission p : held) if (p.getUri().equals(uri) && p.isReadPermission()) return true;
        return false;
    }

    /**
     * Start-up, after the tabs were restored from their private copies: for each copy whose source this app may still
     * read, read the source again; native code puts a changed text into the tab. Other tabs stay as they were copied.
     */
    public void refreshSources(final String paths) {
        new Thread(() -> {
            SharedPreferences prefs = sources();
            for (String path : paths.split("\n")) {
                if (path.isEmpty()) continue;
                String src = prefs.getString("p:" + path, null);
                if (src == null) continue;
                try {
                    Uri uri = Uri.parse(src);
                    if (!canReadAgain(uri)) {
                        nativeSourceState(path, true);  // no permission left: the tab says its copy may be old
                        continue;
                    }
                    String stored = nativeOpenDocument(displayName(uri), readAll(uri), path, true);
                    if (stored != null && !stored.isEmpty() && !stored.equals(path)) remember(prefs, src, stored, path);
                } catch (IOException | RuntimeException e) {
                    // the source is gone or unreadable: the tab keeps its copy, and says so
                    nativeSourceState(path, true);
                }
            }
        }, "calligrus-refresh").start();
    }

    /** The file's name as its provider shows it, else the last part of the URI. */
    private String displayName(Uri uri) {
        if ("content".equals(uri.getScheme())) {
            try (Cursor c = getContentResolver().query(uri, new String[] {OpenableColumns.DISPLAY_NAME}, null, null, null)) {
                if (c != null && c.moveToFirst()) {
                    String name = c.getString(0);
                    if (name != null && !name.isEmpty()) return name;
                }
            } catch (RuntimeException ignored) {
                // fall through to the path
            }
        }
        String last = uri.getLastPathSegment();
        return last == null ? "document.md" : last;
    }

    /** The Open button: the system's document picker. The answer comes in onActivityResult. */
    public void showOpenPicker(final String reopen) {
        runOnUiThread(() -> {
            reopenCopy = reopen == null ? "" : reopen;
            Intent pick = new Intent(Intent.ACTION_OPEN_DOCUMENT);
            pick.addCategory(Intent.CATEGORY_OPENABLE);
            pick.setType("*/*");
            // Markdown and Org files are often "application/octet-stream" or have no type at all.
            pick.putExtra(Intent.EXTRA_MIME_TYPES, new String[] {"text/*", "application/octet-stream", "application/x-org"});
            try {
                startActivityForResult(pick, REQUEST_OPEN);
            } catch (ActivityNotFoundException e) {
                Toast.makeText(this, "No document picker available", Toast.LENGTH_LONG).show();
            }
        });
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode != REQUEST_OPEN) return;
        final String reopen = reopenCopy;
        reopenCopy = "";
        if (resultCode == RESULT_OK && data != null && data.getData() != null) {
            openUri(data.getData(), reopen);
        }
    }

    /** A link in a document: the system opens it (browser, mail). */
    public void openUrl(final String url) {
        runOnUiThread(() -> {
            try {
                startActivity(new Intent(Intent.ACTION_VIEW, Uri.parse(url)));
            } catch (ActivityNotFoundException e) {
                Toast.makeText(this, "No app can open this link", Toast.LENGTH_SHORT).show();
            }
        });
    }

    /** Dark icons in the status and navigation bars for a light theme, light icons for a dark one. */
    public void setLightBars(final boolean light) {
        runOnUiThread(() -> {
            WindowInsetsControllerCompat c = WindowCompat.getInsetsController(getWindow(), getWindow().getDecorView());
            c.setAppearanceLightStatusBars(light);
            c.setAppearanceLightNavigationBars(light);
        });
    }
}
