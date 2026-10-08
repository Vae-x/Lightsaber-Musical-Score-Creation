package org.lmsc;

import android.content.Context;
import android.content.ContextWrapper;
import org.json.JSONObject;
import java.io.File;
import java.io.IOException;

/** Packaged only in the PlatformTests APK; retains the real DocumentsProvider. */
public final class PlatformStorageHarness {
    private PlatformStorageHarness() {}

    public static String importTree(Context context, String treeUri, String temporaryRoot) {
        try {
            final File root = new File(temporaryRoot);
            String privatePrefix = context.getFilesDir().getCanonicalPath() + File.separator;
            if (!root.isDirectory() || !root.getCanonicalPath().startsWith(privatePrefix)
                || !root.getName().startsWith("saf-synthetic-"))
                throw new IOException("Synthetic SAF imports require the new private test directory.");
            return StorageBridge.importUri(new ContextWrapper(context) {
                @Override public File getFilesDir() { return root; }
            }, treeUri, true);
        } catch (Exception failure) {
            try {
                JSONObject json = new JSONObject();
                json.put("value", "");
                json.put("error", failure.getMessage());
                return json.toString();
            } catch (Exception ignored) { return "{\"error\":\"Synthetic SAF harness failure\"}"; }
        }
    }
}
