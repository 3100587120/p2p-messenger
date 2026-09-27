package io.p2pmessenger.app;

import android.content.Context;
import android.database.Cursor;
import android.net.Uri;
import android.provider.OpenableColumns;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.UUID;

/** Bridges Android document URIs to the daemon's ordinary-file transfer API. */
public final class ContentFiles {
    private ContentFiles() {}

    private static void copy(InputStream input, OutputStream output) throws Exception {
        byte[] buffer = new byte[64 * 1024];
        int count;
        while ((count = input.read(buffer)) != -1) output.write(buffer, 0, count);
    }

    public static String copyToCache(Context context, String uriText) {
        try {
            Uri uri = Uri.parse(uriText);
            String name = "attachment";
            try (Cursor cursor = context.getContentResolver().query(uri,
                    new String[]{OpenableColumns.DISPLAY_NAME}, null, null, null)) {
                if (cursor != null && cursor.moveToFirst()) {
                    String display = cursor.getString(0);
                    if (display != null && !display.isEmpty()) name = display;
                }
            }
            name = new File(name).getName().replace('/', '_').replace('\\', '_');
            File directory = new File(context.getCacheDir(),
                    "outgoing-files/" + UUID.randomUUID());
            if (!directory.exists() && !directory.mkdirs()) return "";
            File target = new File(directory, name);
            try (InputStream input = context.getContentResolver().openInputStream(uri);
                 OutputStream output = new FileOutputStream(target)) {
                if (input == null) return "";
                copy(input, output);
            }
            return target.getAbsolutePath();
        } catch (Exception ignored) {
            return "";
        }
    }

    public static boolean copyFromCache(Context context, String source, String uriText) {
        try (InputStream input = new java.io.FileInputStream(source);
             OutputStream output = context.getContentResolver().openOutputStream(Uri.parse(uriText), "w")) {
            if (output == null) return false;
            copy(input, output);
            return true;
        } catch (Exception ignored) {
            return false;
        }
    }
}
