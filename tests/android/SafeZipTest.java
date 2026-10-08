import org.lmsc.SafeZip;

import java.io.File;
import java.io.IOException;
import java.io.RandomAccessFile;
import java.nio.charset.Charset;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.Arrays;
import java.util.zip.ZipEntry;
import java.util.zip.ZipOutputStream;

// Host-side tests use only synthetic archives in the dedicated build directory.
public final class SafeZipTest {
    private static int assertions;
    private static File root;
    private static void check(boolean value, String message) {
        ++assertions;
        if (!value) throw new AssertionError(message);
    }
    private static File zip(String id, String... names) throws IOException {
        return zipCharset(id, StandardCharsets.UTF_8, names);
    }
    private static File zipCharset(String id, Charset charset, String... names) throws IOException {
        File file = new File(root, id + ".zip");
        try (ZipOutputStream output = new ZipOutputStream(Files.newOutputStream(file.toPath()), charset)) {
            for (String name : names) {
                output.putNextEntry(new ZipEntry(name));
                if (!name.endsWith("/")) output.write(("synthetic:" + name).getBytes(StandardCharsets.UTF_8));
                output.closeEntry();
            }
        }
        return file;
    }
    private static long centralOffset(File file) throws IOException {
        try (RandomAccessFile input = new RandomAccessFile(file, "r")) {
            for (long pos = 0; pos < input.length() - 4; ++pos) {
                input.seek(pos);
                if (input.readInt() == 0x504b0102) return pos;
            }
        }
        throw new IOException("missing central directory");
    }
    private static void patchCentral(File file, int offset, long value) throws IOException {
        try (RandomAccessFile output = new RandomAccessFile(file, "rw")) {
            output.seek(centralOffset(file) + offset);
            for (int i = 0; i < 4; ++i) output.write((int) (value >>> (i * 8)) & 255);
        }
    }
    private static void reject(File file, String id, boolean beforeWrite) throws IOException {
        byte[] source = Files.readAllBytes(file.toPath());
        File target = new File(root, id);
        boolean rejected = false;
        try { SafeZip.extract(file, target); } catch (IOException expected) { rejected = true; }
        check(rejected, "reject " + id);
        check(Arrays.equals(source, Files.readAllBytes(file.toPath())), "source preserved " + id);
        if (beforeWrite) check(!target.exists(), "validate before write " + id);
    }
    public static void main(String[] args) throws Exception {
        root = Files.createTempDirectory(new File(args[0]).toPath(), "safe-zip-").toFile();
        for (String name : new String[] {"../outside", "safe/../outside", "/absolute", "C:/absolute",
                "safe//empty", "NUL.dat", "safe/trailing.", "safe/control\n", "safe\\..\\outside"}) {
            check(!SafeZip.safeRelativePath(name), "unsafe path " + name);
            reject(zip("bad-" + assertions, name), "rejected-" + assertions, true);
        }
        check(SafeZip.safeRelativePath("中文歌曲/nested/song.ogg"), "unicode path accepted");
        File good = zip("good", "目录/", "目录/中文.dat", "Info.dat");
        File target = new File(root, "good-output");
        SafeZip.extract(good, target);
        check(new File(target, "目录/中文.dat").isFile(), "nested unicode extraction");
        check(new String(Files.readAllBytes(new File(target, "Info.dat").toPath()), StandardCharsets.UTF_8)
            .equals("synthetic:Info.dat"), "content preserved");
        reject(zip("case", "Info.dat", "info.dat"), "case-output", true);
        reject(zip("conflict", "folder", "folder/child.dat"), "conflict-output", true);
        File symlink = zip("symlink", "link");
        patchCentral(symlink, 38, 0xa1ff0000L);
        reject(symlink, "symlink-output", true);
        File oversized = zip("oversized", "large");
        patchCentral(oversized, 24, 1073741825L);
        reject(oversized, "oversized-output", true);
        File wrongCrc = zip("crc", "data");
        patchCentral(wrongCrc, 16, 0);
        reject(wrongCrc, "crc-output", false);
        File legacy = zipCharset("legacy", Charset.forName("CP437"), "café.dat");
        File legacyTarget = new File(root, "legacy-output");
        SafeZip.extract(legacy, legacyTarget);
        check(new File(legacyTarget, "café.dat").isFile(), "legacy CP437 extraction");
        byte[] original = Files.readAllBytes(new File(target, "Info.dat").toPath());
        boolean overwriteRejected = false;
        try { SafeZip.extract(good, target); } catch (IOException expected) { overwriteRejected = true; }
        check(overwriteRejected, "nonempty destination rejected");
        check(Arrays.equals(original, Files.readAllBytes(new File(target, "Info.dat").toPath())), "existing content unchanged");
        check(!new File(root, "outside").exists(), "no escaped files");
        System.out.println("SafeZip: " + assertions + " assertions passed; synthetic artifacts: " + root.getAbsolutePath());
    }
}
