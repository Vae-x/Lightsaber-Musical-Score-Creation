package org.lmsc;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.RandomAccessFile;
import java.nio.ByteBuffer;
import java.nio.charset.CharacterCodingException;
import java.nio.charset.Charset;
import java.nio.charset.CodingErrorAction;
import java.util.ArrayList;
import java.util.Enumeration;
import java.util.HashSet;
import java.util.List;
import java.util.Locale;
import java.util.Set;
import java.util.zip.CRC32;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;

/** ZIP extraction without processes, with validation before the first write. */
public final class SafeZip {
    static final int MAX_ENTRIES = 10000;
    static final long MAX_FILE_BYTES = 1073741824L;
    static final long MAX_TOTAL_BYTES = 2147483648L;
    private SafeZip() {}

    public static boolean safeRelativePath(String name) {
        if (name == null || name.length() == 0 || name.startsWith("/") || name.startsWith("\\")) return false;
        String path = name.replace('\\', '/');
        for (String part : path.split("/", -1)) {
            if (part.length() == 0 || part.equals(".") || part.equals("..")
                || part.endsWith(".") || part.endsWith(" ")
                || part.matches("(?i)(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(\\..*)?")) return false;
            for (int i = 0; i < part.length(); ++i) {
                char ch = part.charAt(i);
                if (ch < 32 || "<>:\"|?*".indexOf(ch) >= 0) return false;
            }
        }
        return true;
    }

    private static int u16(RandomAccessFile file) throws IOException {
        return file.readUnsignedByte() | file.readUnsignedByte() << 8;
    }
    private static long u32(RandomAccessFile file) throws IOException {
        return (long) u16(file) | (long) u16(file) << 16;
    }

    // java.util.zip does not expose Unix link attributes. Read the central
    // directory as well, rejecting links/devices/encryption and ZIP64 up front.
    private static List<String> validateCentralDirectory(File archive) throws IOException {
        try (RandomAccessFile file = new RandomAccessFile(archive, "r")) {
            long length = file.length();
            long eocd = -1;
            for (long pos = length - 22, lower = Math.max(0, length - 65557); pos >= lower; --pos) {
                file.seek(pos);
                if (u32(file) != 0x06054b50L) continue;
                file.seek(pos + 20);
                if (pos + 22 + u16(file) == length) { eocd = pos; break; }
            }
            if (eocd < 0) throw new IOException("ZIP 目录损坏。");
            file.seek(eocd + 4);
            int disk = u16(file), centralDisk = u16(file), diskCount = u16(file), count = u16(file);
            long centralSize = u32(file), offset = u32(file);
            if (disk != 0 || centralDisk != 0 || diskCount != count || count > MAX_ENTRIES
                || count == 65535 || offset == 0xffffffffL || centralSize == 0xffffffffL
                || offset + centralSize != eocd) throw new IOException("ZIP 分卷、ZIP64 或过大的目录不受支持。");
            file.seek(offset);
            List<String> names = new ArrayList<>();
            Set<String> seen = new HashSet<>();
            Set<String> files = new HashSet<>();
            long sum = 0;
            for (int i = 0; i < count; ++i) {
                long start = file.getFilePointer();
                if (u32(file) != 0x02014b50L) throw new IOException("ZIP 条目损坏。");
                u16(file); // creator platform/version
                u16(file); // required version
                int flags = u16(file), method = u16(file);
                file.seek(start + 20);
                long compressed = u32(file), size = u32(file);
                int nameLength = u16(file), extraLength = u16(file), commentLength = u16(file);
                int startDisk = u16(file);
                u16(file);
                long attributes = u32(file), localOffset = u32(file);
                if ((flags & 1) != 0 || (method != ZipEntry.STORED && method != ZipEntry.DEFLATED)
                    || startDisk != 0 || compressed == 0xffffffffL || localOffset >= offset
                    || size > MAX_FILE_BYTES || nameLength == 0
                    || start + 46L + nameLength + extraLength + commentLength > offset + centralSize)
                    throw new IOException("ZIP 含加密、过大或不支持的条目。");
                int unixType = (int) (attributes >>> 16) & 0xf000;
                if (unixType != 0 && unixType != 0x8000 && unixType != 0x4000)
                    throw new IOException("ZIP 含符号链接或特殊文件。");
                byte[] encoded = new byte[nameLength];
                file.readFully(encoded);
                Charset charset = Charset.forName((flags & 2048) != 0 ? "UTF-8" : "CP437");
                String raw;
                try {
                    raw = charset.newDecoder().onMalformedInput(CodingErrorAction.REPORT)
                        .onUnmappableCharacter(CodingErrorAction.REPORT).decode(ByteBuffer.wrap(encoded)).toString();
                } catch (CharacterCodingException e) { throw new IOException("ZIP 文件名编码无效。", e); }
                String name = raw.replace('\\', '/');
                boolean directory = name.endsWith("/");
                if (directory) name = name.substring(0, name.length() - 1);
                String key = name.toLowerCase(Locale.ROOT);
                if (!safeRelativePath(name) || !seen.add(key)) throw new IOException("ZIP 含不安全或重复的路径。");
                if (!directory) files.add(key);
                if (unixType == 0x4000 && !directory) throw new IOException("ZIP 目录类型不一致。");
                sum += size;
                if (sum > MAX_TOTAL_BYTES) throw new IOException("ZIP 超过 2 GiB 导入上限。");
                names.add(raw);
                file.seek(start + 46L + nameLength + extraLength + commentLength);
            }
            if (file.getFilePointer() != offset + centralSize) throw new IOException("ZIP 目录长度不一致。");
            for (String key : seen) {
                int slash = key.lastIndexOf('/');
                while (slash >= 0) {
                    if (files.contains(key.substring(0, slash))) throw new IOException("ZIP 文件与目录路径冲突。");
                    slash = key.lastIndexOf('/', slash - 1);
                }
            }
            return names;
        }
    }

    public static void extract(File archive, File destination) throws IOException {
        if (!archive.isFile()) throw new IOException("ZIP 文件不存在。");
        if (destination.exists() && (!destination.isDirectory() || destination.list() == null
                                     || destination.list().length != 0)) throw new IOException("ZIP 目标必须为空目录。");
        if (!destination.getAbsoluteFile().equals(destination.getCanonicalFile()))
            throw new IOException("ZIP 目标含链接或不规范路径。");
        List<String> names = validateCentralDirectory(archive);
        try (ZipFile zip = new ZipFile(archive, Charset.forName("CP437"))) {
            List<ZipEntry> entries = new ArrayList<>();
            Enumeration<? extends ZipEntry> iterator = zip.entries();
            int index = 0;
            while (iterator.hasMoreElements()) {
                ZipEntry entry = iterator.nextElement();
                if (index >= names.size() || !entry.getName().equals(names.get(index++)))
                    throw new IOException("ZIP 文件名与目录不一致。");
                entries.add(entry);
            }
            if (index != names.size()) throw new IOException("ZIP 条目数量不一致。");
            if (!destination.isDirectory() && !destination.mkdirs()) throw new IOException("无法建立 ZIP 导入目录。");
            String prefix = destination.getCanonicalPath() + File.separator;
            long total = 0;
            byte[] buffer = new byte[65536];
            for (ZipEntry entry : entries) {
                File target = new File(destination, entry.getName().replace('\\', '/'));
                if (!target.getCanonicalPath().startsWith(prefix)) throw new IOException("ZIP 路径越出目标目录。");
                if (entry.isDirectory() || entry.getName().endsWith("\\")) {
                    if (!target.isDirectory() && !target.mkdirs()) throw new IOException("无法建立 ZIP 子目录。");
                    continue;
                }
                File parent = target.getParentFile();
                if (!parent.isDirectory() && !parent.mkdirs()) throw new IOException("无法建立 ZIP 子目录。");
                if (!target.createNewFile()) throw new IOException("ZIP 目标已有文件，未覆盖。");
                long copied = 0;
                CRC32 crc = new CRC32();
                try (InputStream input = zip.getInputStream(entry); FileOutputStream output = new FileOutputStream(target)) {
                    int read;
                    while ((read = input.read(buffer)) != -1) {
                        copied += read;
                        total += read;
                        if (copied > MAX_FILE_BYTES || total > MAX_TOTAL_BYTES || copied > entry.getSize())
                            throw new IOException("ZIP 实际解压大小超过导入上限。");
                        crc.update(buffer, 0, read);
                        output.write(buffer, 0, read);
                    }
                }
                if (copied != entry.getSize() || crc.getValue() != entry.getCrc())
                    throw new IOException("ZIP 文件校验失败，导入副本可能不完整。");
            }
        }
    }

}
