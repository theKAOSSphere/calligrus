package io.github.thekaossphere.calligrus;

import android.content.ActivityNotFoundException;
import android.content.Intent;
import android.database.Cursor;
import android.net.Uri;
import android.os.Bundle;
import android.provider.OpenableColumns;
import android.widget.Toast;

import androidx.core.view.WindowCompat;
import androidx.core.view.WindowInsetsControllerCompat;

import com.google.androidgamesdk.GameActivity;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;

/**
 * The window comes from GameActivity and everything on it is drawn by C++ (android_main). This class
 * does what only Java can: the document picker, reading files other apps hand over ("Open with",
 * share), opening links, and the colour of the status bar icons. A document goes to native code as its
 * display name and its bytes.
 */
public class MainActivity extends GameActivity {
    static {
        System.loadLibrary("calligrus");
    }

    private static final int REQUEST_OPEN = 1;
    private static final long MAX_BYTES = 64L * 1024 * 1024;

    /** Hands a document to the app: the file name (its extension picks Markdown or Org) and the bytes. */
    private static native void nativeOpenDocument(String name, byte[] data);

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
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
                nativeOpenDocument("shared.md", text.getBytes(StandardCharsets.UTF_8));
            }
        }
    }

    /** Reads the document on a thread of its own (a big file must not block the window). */
    private void openUri(final Uri uri) {
        new Thread(() -> {
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
                nativeOpenDocument(displayName(uri), out.toByteArray());
            } catch (IOException | RuntimeException e) {
                runOnUiThread(() -> Toast.makeText(this, "Cannot open the file: " + e.getMessage(), Toast.LENGTH_LONG).show());
            }
        }, "calligrus-open").start();
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
    public void showOpenPicker() {
        runOnUiThread(() -> {
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
        if (requestCode == REQUEST_OPEN && resultCode == RESULT_OK && data != null && data.getData() != null) {
            openUri(data.getData());
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
