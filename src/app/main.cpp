#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QDebug>
#include <QFile>
#include <QFont>
#include <QIcon>
#include <QTemporaryDir>
#include <QTimer>
#include <memory>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include "core/AppInfo.h"
#include "core/AppSettings.h"
#include "gui/MainWindow.h"
#include "gui/EditorViews.h"

int main(int argc, char *argv[]) {
#ifdef Q_OS_ANDROID
    qputenv("ANDROID_OPENSSL_SUFFIX", "_1_1");
#endif
    QCoreApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
    QCoreApplication::setAttribute(Qt::AA_UseHighDpiPixmaps);
    QApplication application(argc, argv);
#ifdef Q_OS_WIN
    // 安装和卸载等待使用者关闭程序，避免更新正在使用的文件或丢失未保存工程。
    // 只保持命名对象，不取得互斥所有权；正常多开仍然可用。
    std::unique_ptr<void, decltype(&CloseHandle)> runningMutex(
        CreateMutexW(nullptr, FALSE, L"Local\\LmscLightsaberScoreRunning"), &CloseHandle);
#endif
    application.setApplicationName(lmsc::AppInfo::name());
    application.setApplicationDisplayName(lmsc::AppInfo::name());
    application.setApplicationVersion(lmsc::AppInfo::version());
    application.setOrganizationName("LMSC");
    application.setWindowIcon(QIcon(":/icons/app.png"));
#ifndef Q_OS_ANDROID
    application.setFont(QFont("Microsoft YaHei UI", 9));
#endif
    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addOption({QStringList{"open"}, QStringLiteral("打开歌曲文件夹、ZIP 或编辑工程。"), "path"});
    parser.addOption({QStringList{"smoke-check"}, QStringLiteral("执行编辑与工程往返验证，并保存截图和报告。"), "directory"});
    parser.addOption({QStringList{"capture"}, QStringLiteral("加载完成后保存界面截图。"), "path"});
    parser.process(application);
    const QString check = parser.value("smoke-check");
    const QString capture = parser.value("capture");
    const bool automated = !check.isEmpty() || !capture.isEmpty();
#ifdef Q_OS_ANDROID
    if (automated) qInfo() << "Android editor validation:" << parser.value("open") << check << capture;
#endif
    std::unique_ptr<QTemporaryDir> isolatedSettings;
    QString settingsFile;
    if (automated) {
        isolatedSettings.reset(new QTemporaryDir);
        if (!isolatedSettings->isValid()) {
            qWarning() << "Cannot create isolated validation settings";
            return 1;
        }
        settingsFile = isolatedSettings->filePath(QStringLiteral("settings.ini"));
        auto preferences = lmsc::AppSettings(settingsFile).load();
        preferences.diagnosticLogEnabled = false;
        QString settingsError;
        if (!lmsc::AppSettings(settingsFile).save(preferences, &settingsError)) {
            qWarning().noquote() << "Cannot save validation settings:" << settingsError;
            return 1;
        }
    }
    MainWindow window(nullptr, settingsFile);
    window.setTestMode(automated);
    auto report = [&](const QString &message, int code) {
        if (!check.isEmpty()) {
            QDir().mkpath(check);
            QFile file(QDir(check).filePath("result.txt"));
            if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) file.write(message.toUtf8());
        }
        application.exit(code);
    };
    if (automated) {
        QObject::connect(&window, &MainWindow::loadFailed, &application, [&, report](const QString &error) {
            report(QStringLiteral("失败：") + error, 2);
        });
        QObject::connect(&window, &MainWindow::documentReady, &application, [&] {
            auto timer = new QTimer(&window);
            int *ticks = new int(0);
            QObject::connect(timer, &QTimer::timeout, &window, [&, timer, ticks, report] {
                if (!window.isAudioReady() && ++*ticks < 120) return;
                timer->stop(); delete ticks;
                if (!window.isAudioReady()) { report(QStringLiteral("失败：音频解码超时"), 3); return; }
                QString error;
                if (!check.isEmpty() && !window.runEditorCheck(check, &error)) { report(QStringLiteral("失败：") + error, 4); return; }
                // Let queued widget updates reach the OpenGL framebuffer.
                QTimer::singleShot(150, &window, [&, report] {
                    const QString path = capture.isEmpty() ? QDir(check).filePath("editor.png") : capture;
                    if (auto *track = window.findChild<TrackView *>()) track->grabFramebuffer();
                    if (!window.grab().save(path)) { report(QStringLiteral("失败：截图保存失败"), 5); return; }
                    report(QStringLiteral("通过：导入、音频、编辑、撤销、重做、保存、导出、重开与渲染\n"), 0);
                });
            });
            timer->start(250);
        });
    }
#ifdef Q_OS_ANDROID
    window.showFullScreen();
#else
    window.show();
#endif
    if (parser.isSet("open")) QTimer::singleShot(0, &window, [&] { window.openPath(parser.value("open")); });
    return application.exec();
}
