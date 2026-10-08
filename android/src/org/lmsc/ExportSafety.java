package org.lmsc;

import java.io.IOException;

/** Records partial creation without deleting or retrying remote content. */
public final class ExportSafety {
    private ExportSafety() {}

    @FunctionalInterface
    public interface Validation { void run() throws Exception; }

    private static String reason(Exception failure) {
        String message = failure.getMessage();
        return message == null || message.length() == 0 ? "存储服务未提供错误详情。" : message;
    }

    // Providers may revoke access or fail even after createDocument succeeds.
    // Attach the returned location before propagating *any* validation failure.
    public static void verifyCreated(String createdUri, Validation validation) throws IOException {
        try { validation.run(); }
        catch (Exception failure) {
            throw new IOException(reason(failure)
                + "\n所选位置可能保留本次新建的条目，请自行核对：" + createdUri, failure);
        }
    }

    public static String failureMessage(Exception failure, String outputUri, String createdCategoryUri) {
        String partialDirectory = outputUri != null ? outputUri : createdCategoryUri;
        return "导出失败，本地完整副本已保留：" + reason(failure)
            + (partialDirectory == null ? "" : "\n所选位置可能保留不完整的新目录，请自行检查：" + partialDirectory);
    }
}
