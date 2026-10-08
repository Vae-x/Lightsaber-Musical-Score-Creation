#include <QApplication>
#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QTimer>

int runAndroidAudioTests(const QString &reportPath);
int runAndroidStorageTests(const QString &reportPath);

int main(int argc, char **argv) {
    qputenv("ANDROID_OPENSSL_SUFFIX", "_1_1");
    QApplication application(argc, argv);
    application.setApplicationName(QStringLiteral("Android平台合成验证"));
    const QString root = QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation))
        .filePath(QStringLiteral("platform-tests"));
    if (!QDir().mkpath(root)) return 100;
    QTimer::singleShot(0, &application, [&] {
        const int audio = runAndroidAudioTests(QDir(root).filePath("audio.txt"));
        const int storage = runAndroidStorageTests(QDir(root).filePath("storage.txt"));
        QFile report(QDir(root).filePath("result.txt"));
        if (!report.open(QIODevice::WriteOnly)) { application.exit(101); return; }
        report.write(QStringLiteral("audio=%1\nstorage=%2\n").arg(audio).arg(storage).toUtf8());
        report.close();
        application.exit(audio + storage);
    });
    return application.exec();
}
