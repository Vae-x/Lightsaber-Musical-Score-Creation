package org.lmsc;

import android.content.ContentResolver;
import android.content.Context;
import android.content.Intent;
import android.database.Cursor;
import android.net.Uri;
import android.provider.DocumentsContract;
import android.provider.DocumentsContract.Document;
import android.provider.OpenableColumns;
import android.security.keystore.KeyGenParameterSpec;
import android.security.keystore.KeyProperties;
import android.util.Base64;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;
import java.security.KeyStore;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashSet;
import java.util.List;
import java.util.Locale;
import java.util.Set;
import java.util.UUID;

import javax.crypto.Cipher;
import javax.crypto.KeyGenerator;
import javax.crypto.SecretKey;
import javax.crypto.spec.GCMParameterSpec;

/** SAF accesses only explicitly selected URIs. Original documents are read-only. */
public final class StorageBridge {
    private static final String KEY_ALIAS = "lmsc-api-key-v1";
    private StorageBridge() {}

    private static String result(String value, String error) {
        try {
            JSONObject json = new JSONObject();
            json.put("value", value == null ? "" : value);
            json.put("error", error == null ? "" : error);
            return json.toString();
        } catch (Exception e) { return "{\"error\":\"Android 文件服务内部错误。\"}"; }
    }

    public static Intent pickerIntent(Context context, boolean directory, String mimeTypesJson) throws Exception {
        Intent intent = new Intent(directory ? Intent.ACTION_OPEN_DOCUMENT_TREE : Intent.ACTION_OPEN_DOCUMENT);
        intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
        if (directory) intent.addFlags(Intent.FLAG_GRANT_WRITE_URI_PERMISSION);
        else {
            intent.addCategory(Intent.CATEGORY_OPENABLE);
            JSONArray types = new JSONArray(mimeTypesJson);
            intent.setType(types.length() == 1 ? types.getString(0) : "*/*");
            if (types.length() > 1) {
                String[] list = new String[types.length()];
                for (int i = 0; i < list.length; ++i) list[i] = types.getString(i);
                intent.putExtra(Intent.EXTRA_MIME_TYPES, list);
            }
        }
        if (intent.resolveActivity(context.getPackageManager()) == null)
            throw new IOException("设备没有可用的系统文件选择器。");
        return intent;
    }

    private static void validateName(String name) throws IOException {
        if (!SafeZip.safeRelativePath(name) || name.contains("/") || name.contains("\\"))
            throw new IOException("文件包含不安全的名称。");
    }

    private static File newImportDirectory(Context context) throws IOException {
        File imports = new File(context.getFilesDir(), "imports");
        if (!imports.isDirectory() && !imports.mkdirs()) throw new IOException("无法建立应用导入目录。");
        File directory = new File(imports, "import-" + UUID.randomUUID().toString());
        if (!directory.mkdir()) throw new IOException("无法建立独立导入副本。");
        return directory;
    }

    private static final class Budget {
        int entries;
        long bytes;
        void entry() throws IOException {
            if (++entries > SafeZip.MAX_ENTRIES) throw new IOException("目录超过 10000 个条目导入上限。");
        }
    }

    private static long copy(InputStream input, OutputStream output, Budget budget) throws IOException {
        if (input == null || output == null) throw new IOException("所选存储位置无法打开数据流。");
        byte[] buffer = new byte[65536];
        long size = 0;
        int read;
        while ((read = input.read(buffer)) != -1) {
            size += read;
            budget.bytes += read;
            if (size > SafeZip.MAX_FILE_BYTES || budget.bytes > SafeZip.MAX_TOTAL_BYTES)
                throw new IOException("文件超过 1 GiB 或目录超过 2 GiB 上限。");
            output.write(buffer, 0, read);
        }
        output.flush();
        return size;
    }

    private static final class Child {
        final Uri uri;
        final String name;
        final boolean directory;
        final long size;
        Child(Uri uri, String name, boolean directory, long size) {
            this.uri = uri; this.name = name; this.directory = directory; this.size = size;
        }
    }

    private static List<Child> children(ContentResolver resolver, Uri parent) throws IOException {
        Uri query = DocumentsContract.buildChildDocumentsUriUsingTree(parent, DocumentsContract.getDocumentId(parent));
        List<Child> result = new ArrayList<>();
        Set<String> names = new HashSet<>();
        try (Cursor cursor = resolver.query(query, new String[] {Document.COLUMN_DOCUMENT_ID,
                Document.COLUMN_DISPLAY_NAME, Document.COLUMN_MIME_TYPE, Document.COLUMN_SIZE}, null, null, null)) {
            if (cursor == null) throw new IOException("无法读取所选目录。");
            while (cursor.moveToNext()) {
                String name = cursor.getString(1);
                validateName(name);
                if (!names.add(name.toLowerCase(Locale.ROOT))) throw new IOException("所选目录含重名条目。");
                if (result.size() >= SafeZip.MAX_ENTRIES) throw new IOException("所选目录条目过多。");
                result.add(new Child(DocumentsContract.buildDocumentUriUsingTree(parent, cursor.getString(0)),
                        name, Document.MIME_TYPE_DIR.equals(cursor.getString(2)), cursor.isNull(3) ? -1 : cursor.getLong(3)));
            }
        }
        return result;
    }

    private static Uri treeDocument(String value) throws IOException {
        Uri tree = Uri.parse(value);
        if (!"content".equals(tree.getScheme()) || !DocumentsContract.isTreeUri(tree))
            throw new IOException("所选目录地址无效。");
        return DocumentsContract.buildDocumentUriUsingTree(tree, DocumentsContract.getTreeDocumentId(tree));
    }

    private static void importTree(ContentResolver resolver, Uri source, File target, Budget budget,
                                   Set<String> ancestors, int depth) throws IOException {
        if (depth > 32 || !ancestors.add(source.toString())) throw new IOException("目录过深或包含循环。");
        try {
            for (Child child : children(resolver, source)) {
                budget.entry();
                File local = new File(target, child.name);
                if (!local.getCanonicalPath().startsWith(target.getCanonicalPath() + File.separator))
                    throw new IOException("导入路径越出本地副本。");
                if (child.directory) {
                    if (!local.mkdir()) throw new IOException("无法建立导入子目录。");
                    importTree(resolver, child.uri, local, budget, ancestors, depth + 1);
                } else {
                    if (child.size > SafeZip.MAX_FILE_BYTES) throw new IOException("所选文件超过 1 GiB 上限。");
                    if (!local.createNewFile()) throw new IOException("导入副本已有文件，未覆盖。");
                    try (InputStream input = resolver.openInputStream(child.uri); OutputStream output = new FileOutputStream(local)) {
                        long copied = copy(input, output, budget);
                        if (child.size >= 0 && child.size != copied) throw new IOException("来源文件在导入期间发生变化。");
                    }
                }
            }
        } finally { ancestors.remove(source.toString()); }
    }

    public static String importUri(Context context, String value, boolean directory) {
        File local = null;
        try {
            Uri uri = Uri.parse(value);
            if (!"content".equals(uri.getScheme())) throw new IOException("所选文件地址无效。");
            ContentResolver resolver = context.getContentResolver();
            local = newImportDirectory(context);
            if (directory) {
                importTree(resolver, treeDocument(value), local, new Budget(), new HashSet<String>(), 0);
                return result(local.getAbsolutePath(), "");
            }
            String name = null;
            long expected = -1;
            try (Cursor cursor = resolver.query(uri, new String[] {OpenableColumns.DISPLAY_NAME, OpenableColumns.SIZE}, null, null, null)) {
                if (cursor != null && cursor.moveToFirst()) {
                    name = cursor.getString(0);
                    if (!cursor.isNull(1)) expected = cursor.getLong(1);
                }
            }
            if (name == null || name.length() == 0) name = "imported-file";
            validateName(name);
            if (expected > SafeZip.MAX_FILE_BYTES) throw new IOException("所选文件超过 1 GiB 导入上限。");
            File target = new File(local, name);
            if (!target.createNewFile()) throw new IOException("无法建立独立导入文件。");
            try (InputStream input = resolver.openInputStream(uri); OutputStream output = new FileOutputStream(target)) {
                long copied = copy(input, output, new Budget());
                if (expected >= 0 && expected != copied) throw new IOException("来源文件在导入期间发生变化。");
            }
            return result(target.getAbsolutePath(), "");
        } catch (Exception e) {
            return result("", "导入失败：" + e.getMessage()
                + (local == null ? "" : "\n可能保留不完整本地副本：" + local.getAbsolutePath()));
        }
    }

    private static void validateLocalTree(File root, File current, Budget budget, int depth) throws IOException {
        if (depth > 32 || !current.getAbsoluteFile().equals(current.getCanonicalFile()))
            throw new IOException("本地导出含链接或过深目录。");
        if (!current.getCanonicalPath().equals(root.getCanonicalPath())
            && !current.getCanonicalPath().startsWith(root.getCanonicalPath() + File.separator))
            throw new IOException("本地导出路径越出根目录。");
        File[] children = current.listFiles();
        if (children == null) throw new IOException("本地导出目录无法读取。");
        Set<String> names = new HashSet<>();
        for (File child : children) {
            budget.entry();
            validateName(child.getName());
            if (!names.add(child.getName().toLowerCase(Locale.ROOT))) throw new IOException("本地导出含重名条目。");
            if (!child.getAbsoluteFile().equals(child.getCanonicalFile())) throw new IOException("本地导出含符号链接。");
            if (child.isDirectory()) validateLocalTree(root, child, budget, depth + 1);
            else if (child.isFile()) {
                budget.bytes += child.length();
                if (child.length() > SafeZip.MAX_FILE_BYTES || budget.bytes > SafeZip.MAX_TOTAL_BYTES)
                    throw new IOException("导出文件或目录超过大小上限。");
            } else throw new IOException("本地导出含特殊文件。");
        }
    }

    private static Uri newDocument(ContentResolver resolver, Uri parent, String mime, String name) throws IOException {
        Set<String> existing = new HashSet<>();
        for (Child child : children(resolver, parent)) {
            existing.add(DocumentsContract.getDocumentId(child.uri));
            if (child.name.equalsIgnoreCase(name)) throw new IOException("目标已有同名条目，未覆盖：" + name);
        }
        Uri created = DocumentsContract.createDocument(resolver, parent, mime, name);
        if (created == null) throw new IOException("存储位置无法建立新条目：" + name);
        ExportSafety.verifyCreated(created.toString(), () -> {
            String parentId = DocumentsContract.getDocumentId(parent);
            if (DocumentsContract.getDocumentId(created).equals(parentId)
                || existing.contains(DocumentsContract.getDocumentId(created)))
                throw new IOException("存储位置返回已有条目，已停止复制。");
            try (Cursor cursor = resolver.query(created, new String[] {Document.COLUMN_DISPLAY_NAME,
                    Document.COLUMN_MIME_TYPE}, null, null, null)) {
                if (cursor == null || !cursor.moveToFirst() || !name.equals(cursor.getString(0))
                    || Document.MIME_TYPE_DIR.equals(mime) != Document.MIME_TYPE_DIR.equals(cursor.getString(1)))
                    throw new IOException("存储位置改变了新条目的名称或类型，已停止复制。");
            }
        });
        return created;
    }

    private static void exportTree(ContentResolver resolver, File source, Uri target, Budget budget) throws IOException {
        File[] files = source.listFiles();
        if (files == null) throw new IOException("本地导出目录无法读取。");
        for (File file : files) {
            budget.entry();
            validateName(file.getName());
            if (!file.getAbsoluteFile().equals(file.getCanonicalFile())) throw new IOException("导出期间出现链接，已停止复制。");
            Uri document = newDocument(resolver, target, file.isDirectory() ? Document.MIME_TYPE_DIR : "application/octet-stream", file.getName());
            if (file.isDirectory()) exportTree(resolver, file, document, budget);
            else {
                long expected = file.length();
                try (InputStream input = new FileInputStream(file); OutputStream output = resolver.openOutputStream(document, "w")) {
                    if (copy(input, output, budget) != expected) throw new IOException("本地文件在导出期间发生变化。");
                }
            }
        }
    }

    public static String exportDirectory(Context context, String sourcePath, String treeUri, String category) {
        Uri output = null;
        Uri createdCategory = null;
        try {
            if (!category.equals("光剑曲谱制作") && !category.equals("光剑曲谱制作工程")) throw new IOException("导出分类无效。");
            File source = new File(sourcePath);
            if (!source.isDirectory()) throw new IOException("本地导出目录不存在。");
            validateName(source.getName());
            validateLocalTree(source, source, new Budget(), 0);
            ContentResolver resolver = context.getContentResolver();
            Uri parent = treeDocument(treeUri);
            Uri categoryUri = null;
            for (Child child : children(resolver, parent)) {
                if (child.name.equalsIgnoreCase(category)) {
                    if (!child.directory) throw new IOException("导出分类已被文件占用，未覆盖。");
                    categoryUri = child.uri;
                }
            }
            if (categoryUri == null) {
                categoryUri = newDocument(resolver, parent, Document.MIME_TYPE_DIR, category);
                createdCategory = categoryUri;
            }
            Set<String> used = new HashSet<>();
            for (Child child : children(resolver, categoryUri)) used.add(child.name.toLowerCase(Locale.ROOT));
            String basename = source.getName(), name = basename;
            for (int suffix = 2; used.contains(name.toLowerCase(Locale.ROOT)); ++suffix) {
                if (suffix > 10000) throw new IOException("同名导出目录过多，请选择其他位置。");
                name = basename + "-" + suffix;
            }
            output = newDocument(resolver, categoryUri, Document.MIME_TYPE_DIR, name);
            exportTree(resolver, source, output, new Budget());
            return result(output.toString(), "");
        } catch (Exception e) {
            return result("", ExportSafety.failureMessage(e,
                output == null ? null : output.toString(),
                createdCategory == null ? null : createdCategory.toString()));
        }
    }

    public static String extractZip(String source, String destination) {
        try { SafeZip.extract(new File(source), new File(destination)); return result("ok", ""); }
        catch (Exception e) { return result("", "ZIP 导入被拒绝：" + e.getMessage()); }
    }

    private static synchronized SecretKey secretKey(boolean create) throws Exception {
        KeyStore store = KeyStore.getInstance("AndroidKeyStore");
        store.load(null);
        if (store.containsAlias(KEY_ALIAS)) return (SecretKey) store.getKey(KEY_ALIAS, null);
        if (!create) throw new IOException("API Key 的设备密钥已丢失，请重新填写。");
        KeyGenerator generator = KeyGenerator.getInstance(KeyProperties.KEY_ALGORITHM_AES, "AndroidKeyStore");
        generator.init(new KeyGenParameterSpec.Builder(KEY_ALIAS, KeyProperties.PURPOSE_ENCRYPT | KeyProperties.PURPOSE_DECRYPT)
            .setBlockModes(KeyProperties.BLOCK_MODE_GCM).setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE)
            .setRandomizedEncryptionRequired(true).setKeySize(256).build());
        return generator.generateKey();
    }

    public static String protectKey(String key) {
        byte[] plain = key.getBytes(StandardCharsets.UTF_8);
        try {
            Cipher cipher = Cipher.getInstance("AES/GCM/NoPadding");
            cipher.init(Cipher.ENCRYPT_MODE, secretKey(true));
            byte[] iv = cipher.getIV(), encrypted = cipher.doFinal(plain);
            ByteBuffer result = ByteBuffer.allocate(1 + iv.length + encrypted.length);
            result.put((byte) iv.length).put(iv).put(encrypted);
            return result(Base64.encodeToString(result.array(), Base64.NO_WRAP), "");
        } catch (Exception e) { return result("", "无法使用 Android Keystore 保护 API Key。"); }
        finally { Arrays.fill(plain, (byte) 0); }
    }

    public static String unprotectKey(String value) {
        byte[] plain = null;
        try {
            byte[] encoded = Base64.decode(value, Base64.NO_WRAP);
            if (!Base64.encodeToString(encoded, Base64.NO_WRAP).equals(value) || encoded.length < 30)
                throw new IOException("密文无效。");
            int ivLength = encoded[0] & 255;
            if (ivLength != 12 || encoded.length <= 1 + ivLength + 16) throw new IOException("密文无效。");
            Cipher cipher = Cipher.getInstance("AES/GCM/NoPadding");
            cipher.init(Cipher.DECRYPT_MODE, secretKey(false), new GCMParameterSpec(128, Arrays.copyOfRange(encoded, 1, 1 + ivLength)));
            plain = cipher.doFinal(encoded, 1 + ivLength, encoded.length - 1 - ivLength);
            String key = new String(plain, StandardCharsets.UTF_8);
            if (!Arrays.equals(key.getBytes(StandardCharsets.UTF_8), plain)) throw new IOException("密文内容无效。");
            return result(key, "");
        } catch (Exception e) { return result("", "无法解密 API Key，请在此设备重新填写。"); }
        finally { if (plain != null) Arrays.fill(plain, (byte) 0); }
    }
}
