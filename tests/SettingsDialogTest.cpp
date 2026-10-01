#include "gui/SettingsDialog.h"
#include "gui/ThemeManager.h"
#include "gui/EditorViews.h"
#include "core/AppSettings.h"
#include <QtTest>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFont>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>

class SettingsDialogTest : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() { qApp->setFont(QFont(QStringLiteral("Microsoft YaHei UI"), 9)); }
    void themePreviewCancelAndApply();
    void providerIsolationAndEncryptedSave();
    void fetchModelsThroughUi();
    void painterViewsFollowTheme();
    void captureSettings();
};

void SettingsDialogTest::themePreviewCancelAndApply() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString file = directory.filePath(QStringLiteral("preferences.json"));
    lmsc::ThemeManager::apply(QStringLiteral("dark"));
    {
        lmsc::SettingsDialog dialog(nullptr, file);
        dialog.show();
        auto theme = dialog.findChild<QComboBox *>(QStringLiteral("themeMode"));
        auto buttons = dialog.findChild<QDialogButtonBox *>(QStringLiteral("settingsButtons"));
        QVERIFY(theme && buttons);
        theme->setCurrentIndex(theme->findData(QStringLiteral("light")));
        QVERIFY(!lmsc::ThemeManager::isDark());
        QTest::mouseClick(buttons->button(QDialogButtonBox::Cancel), Qt::LeftButton);
        QVERIFY(lmsc::ThemeManager::isDark());
        QVERIFY(!QFile::exists(file));
    }
    {
        lmsc::SettingsDialog dialog(nullptr, file);
        dialog.show();
        auto theme = dialog.findChild<QComboBox *>(QStringLiteral("themeMode"));
        auto buttons = dialog.findChild<QDialogButtonBox *>(QStringLiteral("settingsButtons"));
        theme->setCurrentIndex(theme->findData(QStringLiteral("light")));
        QTest::mouseClick(buttons->button(QDialogButtonBox::Apply), Qt::LeftButton);
        QCOMPARE(lmsc::AppSettings(file).load().themeMode, QStringLiteral("light"));
        theme->setCurrentIndex(theme->findData(QStringLiteral("dark")));
        QTest::mouseClick(buttons->button(QDialogButtonBox::Cancel), Qt::LeftButton);
        QVERIFY(!lmsc::ThemeManager::isDark());
        QCOMPARE(lmsc::AppSettings(file).load().themeMode, QStringLiteral("light"));
    }
}

void SettingsDialogTest::providerIsolationAndEncryptedSave() {
    QTemporaryDir directory;
    const QString file = directory.filePath(QStringLiteral("preferences.json"));
    lmsc::SettingsDialog dialog(nullptr, file);
    dialog.show();
    auto nav = dialog.findChild<QListWidget *>(QStringLiteral("settingsNavigation"));
    auto provider = dialog.findChild<QComboBox *>(QStringLiteral("aiProvider"));
    auto key = dialog.findChild<QLineEdit *>(QStringLiteral("aiApiKey"));
    auto model = dialog.findChild<QComboBox *>(QStringLiteral("aiModel"));
    auto url = dialog.findChild<QLineEdit *>(QStringLiteral("aiBaseUrl"));
    auto show = dialog.findChild<QCheckBox *>(QStringLiteral("showApiKey"));
    auto buttons = dialog.findChild<QDialogButtonBox *>(QStringLiteral("settingsButtons"));
    QVERIFY(nav && provider && key && model && url && show && buttons);
    nav->setCurrentRow(1);
    QCOMPARE(provider->currentData().toString(), QStringLiteral("deepseek"));
    QCOMPARE(key->echoMode(), QLineEdit::Password);
    key->setText(QStringLiteral("test-only-deepseek-key"));
    model->setEditText(QStringLiteral("manual-deepseek-model"));
    const QString originalUrl = url->text();
    show->setChecked(true);
    QCOMPARE(key->echoMode(), QLineEdit::Normal);
    provider->setCurrentIndex(provider->findData(QStringLiteral("kimi")));
    QVERIFY(key->text().isEmpty());
    QCOMPARE(key->echoMode(), QLineEdit::Password);
    key->setText(QStringLiteral("test-only-kimi-key"));
    model->setEditText(QStringLiteral("manual-kimi-model"));
    provider->setCurrentIndex(provider->findData(QStringLiteral("deepseek")));
    QCOMPARE(key->text(), QStringLiteral("test-only-deepseek-key"));
    QCOMPARE(model->currentText(), QStringLiteral("manual-deepseek-model"));
    QCOMPARE(url->text(), originalUrl);
    QTest::mouseClick(buttons->button(QDialogButtonBox::Save), Qt::LeftButton);
    QCOMPARE(dialog.result(), int(QDialog::Accepted));
    const auto saved = lmsc::AppSettings(file).load();
    QCOMPARE(saved.providers.value(QStringLiteral("kimi")).model, QStringLiteral("manual-kimi-model"));
    QCOMPARE(saved.providers.value(QStringLiteral("deepseek")).apiKey, QStringLiteral("test-only-deepseek-key"));
    QFile content(file);
    QVERIFY(content.open(QIODevice::ReadOnly));
    const QByteArray bytes = content.readAll();
    QVERIFY(!bytes.contains("test-only-deepseek-key"));
    QVERIFY(!bytes.contains("test-only-kimi-key"));
    lmsc::SettingsDialog reopened(nullptr, file);
    QCOMPARE(reopened.findChild<QComboBox *>(QStringLiteral("aiModel"))->currentText(), QStringLiteral("manual-deepseek-model"));
}

void SettingsDialogTest::fetchModelsThroughUi() {
    QTemporaryDir directory;
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost, 0));
    QByteArray received;
    connect(&server, &QTcpServer::newConnection, &server, [&] {
        auto socket = server.nextPendingConnection();
        connect(socket, &QTcpSocket::readyRead, socket, [&, socket] {
            const QByteArray request = socket->readAll();
            received.append(request);
            if (!received.contains("\r\n\r\n")) return;
            const QByteArray body = R"({"data":[{"id":"flow-model"},{"id":"rhythm-model"}]})";
            socket->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
            socket->disconnectFromHost();
        });
        connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
    });
    lmsc::SettingsDialog dialog(nullptr, directory.filePath(QStringLiteral("preferences.json")));
    dialog.show();
    dialog.findChild<QListWidget *>(QStringLiteral("settingsNavigation"))->setCurrentRow(1);
    auto provider = dialog.findChild<QComboBox *>(QStringLiteral("aiProvider"));
    provider->setCurrentIndex(provider->findData(QStringLiteral("custom")));
    dialog.findChild<QLineEdit *>(QStringLiteral("aiBaseUrl"))->setText(QStringLiteral("http://127.0.0.1:%1/v1").arg(server.serverPort()));
    auto key = dialog.findChild<QLineEdit *>(QStringLiteral("aiApiKey"));
    key->setText(QStringLiteral("test-local-key"));
    auto model = dialog.findChild<QComboBox *>(QStringLiteral("aiModel"));
    model->setEditText(QStringLiteral("rhythm-model"));
    // Filling a key and leaving the field triggers automatic discovery.
    QVERIFY(QMetaObject::invokeMethod(key, "editingFinished"));
    QTRY_COMPARE_WITH_TIMEOUT(model->count(), 2, 5000);
    QCOMPARE(model->currentText(), QStringLiteral("rhythm-model"));
    QVERIFY(received.startsWith("GET /v1/models HTTP/1.1"));
    QVERIFY(received.contains("Authorization: Bearer test-local-key"));
    const auto status = dialog.findChild<QLabel *>(QStringLiteral("apiConnectionStatus"));
    QVERIFY(status->text().contains(QStringLiteral("2 个模型")));
    auto buttons = dialog.findChild<QDialogButtonBox *>(QStringLiteral("settingsButtons"));
    QTest::mouseClick(buttons->button(QDialogButtonBox::Apply), Qt::LeftButton);
    const auto saved = lmsc::AppSettings(directory.filePath(QStringLiteral("preferences.json"))).load();
    QCOMPARE(saved.providerId, QStringLiteral("custom"));
    QCOMPARE(saved.providers.value(QStringLiteral("custom")).models.size(), 2);
}

void SettingsDialogTest::painterViewsFollowTheme() {
    GridEditor grid;
    grid.resize(350, 300);
    grid.show();
    lmsc::ThemeManager::apply(QStringLiteral("dark"));
    QTest::qWait(30);
    const QImage dark = grid.grab().toImage();
    lmsc::ThemeManager::apply(QStringLiteral("light"));
    QTest::qWait(30);
    const QImage light = grid.grab().toImage();
    QVERIFY(dark.pixelColor(2, 2).lightness() < 100);
    QVERIFY(light.pixelColor(2, 2).lightness() > 180);
}

void SettingsDialogTest::captureSettings() {
    const QString output = qEnvironmentVariable("LMSC_SETTINGS_CAPTURE_DIRECTORY");
    if (output.isEmpty()) QSKIP("截图仅在指定核验目录时生成");
    QVERIFY(QDir().mkpath(output));
    QTemporaryDir directory;
    lmsc::SettingsDialog dialog(nullptr, directory.filePath(QStringLiteral("preferences.json")));
    dialog.show();
    auto nav = dialog.findChild<QListWidget *>(QStringLiteral("settingsNavigation"));
    auto theme = dialog.findChild<QComboBox *>(QStringLiteral("themeMode"));
    theme->setCurrentIndex(theme->findData(QStringLiteral("dark")));
    nav->setCurrentRow(1);
    QTest::qWait(100);
    QVERIFY(dialog.grab().save(QDir(output).filePath(QStringLiteral("settings-ai-dark.png"))));
    theme->setCurrentIndex(theme->findData(QStringLiteral("light")));
    QTest::qWait(100);
    QVERIFY(dialog.grab().save(QDir(output).filePath(QStringLiteral("settings-ai-light.png"))));
    nav->setCurrentRow(0);
    QTest::qWait(50);
    QVERIFY(dialog.grab().save(QDir(output).filePath(QStringLiteral("settings-appearance-light.png"))));
}

QTEST_MAIN(SettingsDialogTest)
#include "SettingsDialogTest.moc"
