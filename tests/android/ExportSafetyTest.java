import org.lmsc.ExportSafety;

import java.io.IOException;

/** Synthetic provider-validation failures; does not claim to test SAF access. */
public final class ExportSafetyTest {
    private static int assertions;
    private static void check(boolean value, String message) {
        ++assertions;
        if (!value) throw new AssertionError(message);
    }
    public static void main(String[] args) throws Exception {
        final String category = "content://synthetic/tree/test/document/new-category";
        final String song = "content://synthetic/tree/test/document/new-song";
        final String resource = "content://synthetic/tree/test/document/new-resource";
        ExportSafety.verifyCreated(song, () -> {});
        for (Exception failure : new Exception[] {new SecurityException("synthetic revoked permission"),
                new IOException("synthetic query failure"), new IllegalArgumentException("synthetic invalid metadata"),
                new SecurityException()}) {
            boolean rejected = false;
            try { ExportSafety.verifyCreated(song, () -> { throw failure; }); }
            catch (IOException expected) {
                rejected = true;
                check(expected.getMessage().contains(song), "post-create failure includes created URI");
                check(expected.getCause() == failure, "original provider failure retained");
                String message = ExportSafety.failureMessage(expected, null, null);
                check(message.contains(song), "top-level report retains unverified new directory");
                check(message.contains("本地完整副本已保留"), "local copy is retained");
            }
            check(rejected, "post-create validation rejected");
        }
        Exception enumerationFailure = new SecurityException("synthetic category enumeration failure");
        String categoryReport = ExportSafety.failureMessage(enumerationFailure, null, category);
        check(categoryReport.contains(category), "category created before enumeration failure reported");
        check(!categoryReport.contains(song), "does not invent a song directory");
        String songReport = ExportSafety.failureMessage(new IOException("synthetic stream failure"), song, category);
        check(songReport.contains(song), "created song directory reported");
        check(!songReport.contains(category), "song location used after creation completed");
        String noCreation = ExportSafety.failureMessage(new IOException("synthetic prevalidation failure"), null, null);
        check(!noCreation.contains("content://"), "no residual location invented before creation");
        try {
            ExportSafety.verifyCreated(resource, () -> { throw new SecurityException("synthetic resource metadata query"); });
        } catch (IOException failure) {
            String message = ExportSafety.failureMessage(failure, song, category);
            check(message.contains(resource) && message.contains(song), "resource and enclosing partial song both reported");
        }
        System.out.println("ExportSafety: " + assertions + " assertions passed (synthetic failures, no SAF provider access).");
    }
}
