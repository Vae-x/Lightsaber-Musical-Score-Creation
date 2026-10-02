#include "gui/SettingsDialog.h"
#include "gui/NavigationSidebar.h"
#include "gui/ThemeManager.h"
#include "gui/EditorViews.h"
#include "core/AppSettings.h"
#include "core/AppInfo.h"
#include "core/DiagnosticLog.h"
#include <QtTest>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFont>
#include <QFrame>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QPlainTextEdit>
#include <QScrollArea>
#include <QScrollBar>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QToolButton>

class SettingsHomepageReceiver : public QObject {
    Q_OBJECT
public:
    QUrl openedUrl;
public slots:
    void openUrl(const QUrl &url) { openedUrl = url; }
};

class SettingsDialogTest : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() { qApp->setFont(QFont(QStringLiteral("Microsoft YaHei UI"), 9)); }
    void themePreviewCancelAndApply();
    void navigationCollapsePreservesDraft();
    void compactNavigationMouseAndKeyboard();
    void responsiveSettingsLayout_data();
    void responsiveSettingsLayout();
    void providerIsolationAndEncryptedSave();
    void fetchModelsThroughUi();
    void fetchModelsThroughManualProxy();
    void proxySaveCancelAndValidation();
    void networkDiagnosticsAndPersistence();
    void aboutHomepage();
    void painterViewsFollowTheme();
    void captureSettings();
};

void SettingsDialogTest::navigationCollapsePreservesDraft() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    lmsc::SettingsDialog dialog(nullptr, directory.filePath(QStringLiteral("preferences.json")));
    dialog.show();
    auto sidebar = dialog.findChild<lmsc::NavigationSidebar *>(QStringLiteral("settingsSidebar"));
    auto nav = dialog.findChild<QListWidget *>(QStringLiteral("settingsNavigation"));
    auto pages = dialog.findChild<QStackedWidget *>(QStringLiteral("settingsPages"));
    auto toggle = dialog.findChild<QToolButton *>(QStringLiteral("navigationToggle"));
    auto model = dialog.findChild<QComboBox *>(QStringLiteral("aiModel"));
    auto key = dialog.findChild<QLineEdit *>(QStringLiteral("aiApiKey"));
    auto showKey = dialog.findChild<QCheckBox *>(QStringLiteral("showApiKey"));
    QVERIFY(sidebar && nav && pages && toggle && model && key && showKey);
    QCOMPARE(sidebar->listWidget(), nav);
    QVERIFY(!sidebar->isCollapsed());
    QCOMPARE(sidebar->width(), 208);
    QCOMPARE(toggle->toolTip(), QStringLiteral("收起导航"));
    QCOMPARE(toggle->accessibleName(), QStringLiteral("收起导航"));
    const QStringList pageNames = {QStringLiteral("外观"), QStringLiteral("大语言模型"),
                                  QStringLiteral("账号授权"), QStringLiteral("网络"), QStringLiteral("关于")};
    QCOMPARE(nav->count(), pageNames.size());
    for (int row = 0; row < nav->count(); ++row) {
        QCOMPARE(nav->item(row)->text(), pageNames.at(row));
        QCOMPARE(nav->item(row)->toolTip(), pageNames.at(row));
        QCOMPARE(nav->item(row)->data(Qt::AccessibleTextRole).toString(), pageNames.at(row));
        QVERIFY(!nav->item(row)->icon().isNull());
    }
    nav->setCurrentRow(1);
    QWidget *const modelPage = pages->currentWidget();
    key->setText(QStringLiteral("test-only-navigation-key"));
    model->setEditText(QStringLiteral("unsaved-navigation-model"));
    QSignalSpy rowChanges(nav, &QListWidget::currentRowChanged);
    QSignalSpy collapseChanges(sidebar, &lmsc::NavigationSidebar::collapsedChanged);
    QVERIFY(rowChanges.isValid() && collapseChanges.isValid());

    QTest::mouseClick(toggle, Qt::LeftButton);
    QTRY_VERIFY(sidebar->isCollapsed());
    QCOMPARE(sidebar->width(), 64);
    QCOMPARE(toggle->toolTip(), QStringLiteral("展开导航"));
    QCOMPARE(toggle->accessibleName(), QStringLiteral("展开导航"));
    QCOMPARE(nav->currentRow(), 1);
    QCOMPARE(pages->currentIndex(), 1);
    QCOMPARE(pages->currentWidget(), modelPage);
    QCOMPARE(rowChanges.count(), 0);
    QCOMPARE(collapseChanges.count(), 1);
    QCOMPARE(collapseChanges.at(0).at(0).toBool(), true);
    QCOMPARE(model->currentText(), QStringLiteral("unsaved-navigation-model"));
    QCOMPARE(key->text(), QStringLiteral("test-only-navigation-key"));
    QCOMPARE(key->echoMode(), QLineEdit::Password);
    QVERIFY(!showKey->isChecked());

    QTest::mouseClick(toggle, Qt::LeftButton);
    QTRY_VERIFY(!sidebar->isCollapsed());
    QCOMPARE(sidebar->width(), 208);
    QCOMPARE(model->currentText(), QStringLiteral("unsaved-navigation-model"));
    QCOMPARE(key->echoMode(), QLineEdit::Password);
    QCOMPARE(pages->currentWidget(), modelPage);
    QCOMPARE(rowChanges.count(), 0);
    QCOMPARE(collapseChanges.count(), 2);
    QCOMPARE(collapseChanges.at(1).at(0).toBool(), false);

    // Explicitly displaying a key is a draft state too; folding must preserve it.
    showKey->setChecked(true);
    sidebar->setCollapsed(true);
    sidebar->setCollapsed(true);
    QCOMPARE(collapseChanges.count(), 3);
    QCOMPARE(key->echoMode(), QLineEdit::Normal);
    QVERIFY(showKey->isChecked());
    sidebar->setCollapsed(false);
    QCOMPARE(key->echoMode(), QLineEdit::Normal);
    QVERIFY(showKey->isChecked());
    QCOMPARE(model->currentText(), QStringLiteral("unsaved-navigation-model"));
    QCOMPARE(rowChanges.count(), 0);
    const int eventsBeforeKeys = collapseChanges.count();
    toggle->setFocus();
    // Enter on the menu button must never reach QDialog's default Save button.
    for (int keyCode : {int(Qt::Key_Return), int(Qt::Key_Enter)}) {
        QTest::keyClick(toggle, static_cast<Qt::Key>(keyCode));
        QVERIFY(sidebar->isCollapsed());
        QVERIFY(dialog.isVisible());
        sidebar->setCollapsed(false);
    }
    QCOMPARE(collapseChanges.count(), eventsBeforeKeys + 4);
    QTest::keyClick(toggle, Qt::Key_Enter, Qt::KeypadModifier);
    QVERIFY(sidebar->isCollapsed());
    QVERIFY(dialog.isVisible());
    QCOMPARE(rowChanges.count(), 0);
    QVERIFY(!QFile::exists(directory.filePath(QStringLiteral("preferences.json"))));
}

void SettingsDialogTest::compactNavigationMouseAndKeyboard() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    lmsc::SettingsDialog dialog(nullptr, directory.filePath(QStringLiteral("preferences.json")));
    dialog.show();
    auto sidebar = dialog.findChild<lmsc::NavigationSidebar *>(QStringLiteral("settingsSidebar"));
    auto nav = dialog.findChild<QListWidget *>(QStringLiteral("settingsNavigation"));
    auto pages = dialog.findChild<QStackedWidget *>(QStringLiteral("settingsPages"));
    QVERIFY(sidebar && nav && pages);
    sidebar->setCollapsed(true);
    QTest::qWait(30);
    const QRect networkItem = nav->visualItemRect(nav->item(3));
    QVERIFY(nav->viewport()->rect().contains(networkItem.center()));
    QTest::mouseClick(nav->viewport(), Qt::LeftButton, Qt::NoModifier, networkItem.center());
    QCOMPARE(nav->currentRow(), 3);
    QCOMPARE(pages->currentIndex(), 3);
    QCOMPARE(nav->currentItem()->toolTip(), QStringLiteral("网络"));
    QCOMPARE(nav->currentItem()->data(Qt::AccessibleTextRole).toString(), QStringLiteral("网络"));

    nav->setFocus();
    QTest::keyClick(nav, Qt::Key_Down);
    QCOMPARE(nav->currentRow(), 4);
    QCOMPARE(pages->currentIndex(), 4);
    QTest::keyClick(nav, Qt::Key_Up);
    QCOMPARE(nav->currentRow(), 3);
    QCOMPARE(pages->currentIndex(), 3);
    sidebar->setCollapsed(false);
    QCOMPARE(nav->currentRow(), 3);
    QCOMPARE(pages->currentIndex(), 3);
    QCOMPARE(nav->currentItem()->text(), QStringLiteral("网络"));
}

void SettingsDialogTest::responsiveSettingsLayout_data() {
    QTest::addColumn<QSize>("windowSize");
    QTest::addColumn<bool>("collapsed");
    QTest::addColumn<QString>("themeMode");
    for (const QSize size : {QSize(980, 690), QSize(760, 500)}) {
        for (bool collapsed : {false, true}) {
            for (const QString &theme : {QStringLiteral("dark"), QStringLiteral("light")}) {
                const QByteArray name = QStringLiteral("%1x%2-%3-%4")
                    .arg(size.width()).arg(size.height())
                    .arg(collapsed ? QStringLiteral("compact") : QStringLiteral("expanded"), theme).toLatin1();
                QTest::newRow(name.constData()) << size << collapsed << theme;
            }
        }
    }
}

void SettingsDialogTest::responsiveSettingsLayout() {
    QFETCH(QSize, windowSize);
    QFETCH(bool, collapsed);
    QFETCH(QString, themeMode);
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    lmsc::SettingsDialog dialog(nullptr, directory.filePath(QStringLiteral("preferences.json")));
    auto sidebar = dialog.findChild<lmsc::NavigationSidebar *>(QStringLiteral("settingsSidebar"));
    auto nav = dialog.findChild<QListWidget *>(QStringLiteral("settingsNavigation"));
    auto pages = dialog.findChild<QStackedWidget *>(QStringLiteral("settingsPages"));
    auto footer = dialog.findChild<QWidget *>(QStringLiteral("settingsFooter"));
    auto buttons = dialog.findChild<QDialogButtonBox *>(QStringLiteral("settingsButtons"));
    auto theme = dialog.findChild<QComboBox *>(QStringLiteral("themeMode"));
    auto codexPath = dialog.findChild<QLineEdit *>(QStringLiteral("codexExecutable"));
    QVERIFY(sidebar && nav && pages && footer && buttons && theme && codexPath);
    // Visiting the account page must not inspect the user's installed CLI or credentials.
    codexPath->setText(directory.filePath(QStringLiteral("unavailable-test-codex.exe")));
    theme->setCurrentIndex(theme->findData(themeMode));
    lmsc::ThemeManager::apply(themeMode);
    sidebar->setCollapsed(collapsed);
    dialog.resize(windowSize);
    dialog.show();
    QTest::qWait(30);
    QCOMPARE(dialog.size(), windowSize);
    const auto inDialog = [&dialog](QWidget *widget) {
        return QRect(widget->mapTo(&dialog, QPoint(0, 0)), widget->size());
    };
    QVERIFY(dialog.rect().contains(inDialog(sidebar)));
    QVERIFY(dialog.rect().contains(inDialog(footer)));
    QVERIFY(inDialog(pages).bottom() < inDialog(footer).top());
    const auto footerButtons = buttons->buttons();
    QCOMPARE(footerButtons.size(), 3);
    for (int index = 0; index < footerButtons.size(); ++index) {
        auto button = footerButtons.at(index);
        QVERIFY(button->isVisible());
        QVERIFY(inDialog(footer).contains(inDialog(button)));
        QVERIFY(button->width() >= button->minimumSizeHint().width());
        for (int other = index + 1; other < footerButtons.size(); ++other)
            QVERIFY(!inDialog(button).intersects(inDialog(footerButtons.at(other))));
    }

    for (int row = 0; row < nav->count(); ++row) {
        nav->setCurrentRow(row);
        QTest::qWait(10);
        auto scroll = qobject_cast<QScrollArea *>(pages->currentWidget());
        QVERIFY(scroll && scroll->widget());
        auto content = scroll->widget();
        QCOMPARE(content->property("role").toString(), QStringLiteral("settingsPage"));
        QCOMPARE(content->width(), scroll->viewport()->width());
        QCOMPARE(scroll->horizontalScrollBar()->maximum(), 0);
        int cardCount = 0;
        for (auto card : content->findChildren<QFrame *>()) {
            if (card->property("role").toString() != QStringLiteral("settingsCard")) continue;
            if (!card->isVisibleTo(content)) continue;
            ++cardCount;
            const QRect cardBounds(card->mapTo(content, QPoint(0, 0)), card->size());
            QVERIFY(cardBounds.width() > 0 && cardBounds.height() > 0);
            QVERIFY(cardBounds.left() >= 0 && cardBounds.right() < content->width());
            for (auto field : card->findChildren<QWidget *>()) {
                if (!field->isVisibleTo(card)) continue;
                if (!qobject_cast<QLabel *>(field) && !qobject_cast<QComboBox *>(field)
                    && !qobject_cast<QLineEdit *>(field) && !qobject_cast<QPushButton *>(field)
                    && !qobject_cast<QSpinBox *>(field) && !qobject_cast<QCheckBox *>(field)) continue;
                const QRect fieldBounds(field->mapTo(content, QPoint(0, 0)), field->size());
                QVERIFY2(fieldBounds.width() > 0 && fieldBounds.left() >= cardBounds.left()
                         && fieldBounds.right() <= cardBounds.right(), qPrintable(field->objectName()));
            }
        }
        QVERIFY2(cardCount > 0, qPrintable(nav->currentItem()->text()));
        QCOMPARE(pages->currentIndex(), row);
        QVERIFY(inDialog(pages).bottom() < inDialog(footer).top());
    }
    nav->setCurrentRow(3);
    auto proxyMode = dialog.findChild<QComboBox *>(QStringLiteral("networkProxyMode"));
    auto proxyHost = dialog.findChild<QLineEdit *>(QStringLiteral("networkProxyHost"));
    auto saveStatus = dialog.findChild<QLabel *>(QStringLiteral("settingsSaveStatus"));
    QVERIFY(proxyMode && proxyHost && saveStatus);
    proxyMode->setCurrentIndex(proxyMode->findData(QStringLiteral("manual")));
    proxyHost->clear();
    QTest::mouseClick(buttons->button(QDialogButtonBox::Apply), Qt::LeftButton);
    QTest::qWait(10);
    QVERIFY(!saveStatus->text().isEmpty());
    QVERIFY(dialog.rect().contains(inDialog(footer)));
    QVERIFY(inDialog(footer).contains(inDialog(saveStatus)));
    QVERIFY(inDialog(pages).bottom() < inDialog(footer).top());
    for (auto button : footerButtons) QVERIFY(inDialog(footer).contains(inDialog(button)));
    QVERIFY(!QFile::exists(directory.filePath(QStringLiteral("preferences.json"))));
}

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

void SettingsDialogTest::proxySaveCancelAndValidation() {
    QTemporaryDir directory;
    const QString file = directory.filePath(QStringLiteral("preferences.json"));
    {
        lmsc::SettingsDialog dialog(nullptr, file);
        dialog.show();
        auto nav = dialog.findChild<QListWidget *>(QStringLiteral("settingsNavigation"));
        auto mode = dialog.findChild<QComboBox *>(QStringLiteral("networkProxyMode"));
        auto host = dialog.findChild<QLineEdit *>(QStringLiteral("networkProxyHost"));
        auto port = dialog.findChild<QSpinBox *>(QStringLiteral("networkProxyPort"));
        auto buttons = dialog.findChild<QDialogButtonBox *>(QStringLiteral("settingsButtons"));
        QVERIFY(nav && mode && host && port && buttons);
        nav->setCurrentRow(3);
        QCOMPARE(nav->currentItem()->text(), QStringLiteral("网络"));
        QCOMPARE(mode->currentData().toString(), QStringLiteral("system"));
        QVERIFY(!host->isEnabled());
        QVERIFY(!port->isEnabled());
        mode->setCurrentIndex(mode->findData(QStringLiteral("manual")));
        QVERIFY(host->isEnabled());
        QVERIFY(port->isEnabled());
        QTest::mouseClick(buttons->button(QDialogButtonBox::Save), Qt::LeftButton);
        QVERIFY(dialog.isVisible());
        QVERIFY(!QFile::exists(file));
        QVERIFY(dialog.findChild<QLabel *>(QStringLiteral("settingsSaveStatus"))->text().contains(QStringLiteral("代理")));
        host->setText(QStringLiteral("127.0.0.1"));
        port->setValue(7890);
        QTest::mouseClick(buttons->button(QDialogButtonBox::Apply), Qt::LeftButton);
        const auto saved = lmsc::AppSettings(file).load();
        QCOMPARE(saved.networkProxy.mode, QStringLiteral("manual"));
        QCOMPARE(saved.networkProxy.host, QStringLiteral("127.0.0.1"));
        QCOMPARE(saved.networkProxy.port, 7890);
        host->setText(QStringLiteral("unsaved.example.com"));
        QTest::mouseClick(buttons->button(QDialogButtonBox::Cancel), Qt::LeftButton);
        QCOMPARE(lmsc::AppSettings(file).load().networkProxy.host, QStringLiteral("127.0.0.1"));
    }
    lmsc::SettingsDialog reopened(nullptr, file);
    reopened.show();
    auto mode = reopened.findChild<QComboBox *>(QStringLiteral("networkProxyMode"));
    auto host = reopened.findChild<QLineEdit *>(QStringLiteral("networkProxyHost"));
    auto port = reopened.findChild<QSpinBox *>(QStringLiteral("networkProxyPort"));
    auto buttons = reopened.findChild<QDialogButtonBox *>(QStringLiteral("settingsButtons"));
    QCOMPARE(mode->currentData().toString(), QStringLiteral("manual"));
    QCOMPARE(host->text(), QStringLiteral("127.0.0.1"));
    QCOMPARE(port->value(), 7890);
    mode->setCurrentIndex(mode->findData(QStringLiteral("system")));
    QVERIFY(!host->isEnabled());
    QTest::mouseClick(buttons->button(QDialogButtonBox::Save), Qt::LeftButton);
    QCOMPARE(reopened.result(), int(QDialog::Accepted));
    QCOMPARE(lmsc::AppSettings(file).load().networkProxy.mode, QStringLiteral("system"));
}

void SettingsDialogTest::fetchModelsThroughManualProxy() {
    QTemporaryDir directory;
    QTcpServer proxyServer;
    QVERIFY(proxyServer.listen(QHostAddress::LocalHost, 0));
    QByteArray received;
    connect(&proxyServer, &QTcpServer::newConnection, &proxyServer, [&] {
        auto socket = proxyServer.nextPendingConnection();
        connect(socket, &QTcpSocket::readyRead, socket, [&, socket] {
            received.append(socket->readAll());
            if (!received.contains("\r\n\r\n")) return;
            socket->write("HTTP/1.1 502 Bad Gateway\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
            socket->disconnectFromHost();
        });
        connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
    });
    lmsc::SettingsDialog dialog(nullptr, directory.filePath(QStringLiteral("preferences.json")));
    dialog.show();
    auto nav = dialog.findChild<QListWidget *>(QStringLiteral("settingsNavigation"));
    nav->setCurrentRow(3);
    auto mode = dialog.findChild<QComboBox *>(QStringLiteral("networkProxyMode"));
    mode->setCurrentIndex(mode->findData(QStringLiteral("manual")));
    dialog.findChild<QLineEdit *>(QStringLiteral("networkProxyHost"))->setText(QStringLiteral("127.0.0.1"));
    dialog.findChild<QSpinBox *>(QStringLiteral("networkProxyPort"))->setValue(proxyServer.serverPort());
    nav->setCurrentRow(1);
    auto provider = dialog.findChild<QComboBox *>(QStringLiteral("aiProvider"));
    provider->setCurrentIndex(provider->findData(QStringLiteral("custom")));
    dialog.findChild<QLineEdit *>(QStringLiteral("aiBaseUrl"))->setText(QStringLiteral("https://proxy-target.invalid/v1"));
    dialog.findChild<QLineEdit *>(QStringLiteral("aiApiKey"))->setText(QStringLiteral("test-proxy-key"));
    QTest::mouseClick(dialog.findChild<QPushButton *>(QStringLiteral("fetchApiModels")), Qt::LeftButton);
    auto status = dialog.findChild<QLabel *>(QStringLiteral("apiConnectionStatus"));
    QTRY_VERIFY_WITH_TIMEOUT(status->text().contains(QStringLiteral("手动填写模型名")), 5000);
    QVERIFY(received.startsWith("CONNECT proxy-target.invalid:443 HTTP/1.1"));
    QVERIFY(!received.contains("test-proxy-key"));
    QVERIFY(dialog.findChild<QPushButton *>(QStringLiteral("fetchApiModels"))->isEnabled());
    QCOMPARE(dialog.findChild<QComboBox *>(QStringLiteral("aiModel"))->count(), 0);
}

void SettingsDialogTest::networkDiagnosticsAndPersistence() {
    struct RestoreLog {
        bool enabled=lmsc::DiagnosticLog::instance().isEnabled();
        ~RestoreLog() { lmsc::DiagnosticLog::instance().setEnabled(enabled); }
    } restore;
    QTemporaryDir directory;
    const QString file=directory.filePath("preferences.json");
    lmsc::SettingsDialog dialog(nullptr,file);
    dialog.show();
    auto nav=dialog.findChild<QListWidget *>("settingsNavigation");
    auto mode=dialog.findChild<QComboBox *>("networkProxyMode");
    auto timeout=dialog.findChild<QSpinBox *>("aiRequestTimeoutMinutes");
    auto enabled=dialog.findChild<QCheckBox *>("diagnosticLogEnabled");
    auto probe=dialog.findChild<QPushButton *>("checkProxyConnection");
    auto status=dialog.findChild<QLabel *>("proxyCheckStatus");
    auto buttons=dialog.findChild<QDialogButtonBox *>("settingsButtons");
    QVERIFY(nav && mode && timeout && enabled && probe && status && buttons);
    QCOMPARE(timeout->value(),10);
    QCOMPARE(timeout->minimum(),1);
    QCOMPARE(timeout->maximum(),30);
    QVERIFY(enabled->isChecked());
    QVERIFY(mode->findData("direct")>=0);
    nav->setCurrentRow(3);
    mode->setCurrentIndex(mode->findData("direct"));
    QVERIFY(!probe->isEnabled());
    timeout->setValue(20);
    enabled->setChecked(false);
    buttons->button(QDialogButtonBox::Apply)->click();
    const auto saved=lmsc::AppSettings(file).load();
    QCOMPARE(saved.networkProxy.mode,QString("direct"));
    QCOMPARE(saved.requestTimeoutMinutes,20);
    QVERIFY(!saved.diagnosticLogEnabled);
    timeout->setValue(1);
    enabled->setChecked(true);
    buttons->button(QDialogButtonBox::Cancel)->click();
    QCOMPARE(lmsc::AppSettings(file).load().requestTimeoutMinutes,20);
    QVERIFY(!lmsc::AppSettings(file).load().diagnosticLogEnabled);

    lmsc::SettingsDialog reopened(nullptr,file);
    reopened.show();
    QCOMPARE(reopened.findChild<QSpinBox *>("aiRequestTimeoutMinutes")->value(),20);
    QVERIFY(!reopened.findChild<QCheckBox *>("diagnosticLogEnabled")->isChecked());
    reopened.findChild<QListWidget *>("settingsNavigation")->setCurrentRow(3);
    auto newMode=reopened.findChild<QComboBox *>("networkProxyMode");
    newMode->setCurrentIndex(newMode->findData("manual"));
    QTcpServer proxy;
    QVERIFY(proxy.listen(QHostAddress::LocalHost,0));
    QByteArray received;
    connect(&proxy,&QTcpServer::newConnection,&proxy,[&] {
        auto socket=proxy.nextPendingConnection();
        connect(socket,&QTcpSocket::readyRead,socket,[&,socket] {
            received+=socket->readAll();
            if (received.contains("\r\n\r\n")) socket->write("HTTP/1.1 407 Proxy Authentication Required\r\n\r\n");
        });
        connect(socket,&QTcpSocket::disconnected,socket,&QObject::deleteLater);
    });
    reopened.findChild<QLineEdit *>("networkProxyHost")->setText("127.0.0.1");
    reopened.findChild<QSpinBox *>("networkProxyPort")->setValue(proxy.serverPort());
    auto newProbe=reopened.findChild<QPushButton *>("checkProxyConnection");
    newProbe->click();
    QVERIFY(!newProbe->isEnabled());
    QTRY_VERIFY_WITH_TIMEOUT(newProbe->isEnabled(),5000);
    QVERIFY(reopened.findChild<QLabel *>("proxyCheckStatus")->text().contains(QStringLiteral("实际服务连接")));
    QVERIFY(received.startsWith("CONNECT 127.0.0.1:1 HTTP/1.1"));
    QVERIFY(!received.contains("Authorization"));
    // Opening logs must work even while recording is disabled.
    reopened.findChild<QListWidget *>("settingsNavigation")->setCurrentRow(4);
    reopened.findChild<QPushButton *>("viewDiagnosticLogs")->click();
    auto viewer=reopened.findChild<QDialog *>("diagnosticLogDialog");
    QVERIFY(viewer && viewer->isVisible());
    QVERIFY(viewer->findChild<QPlainTextEdit *>("diagnosticLogText")->isReadOnly());
    QVERIFY(viewer->findChild<QPushButton *>("exportDiagnosticLog"));
    viewer->close();
}

void SettingsDialogTest::aboutHomepage() {
    QTemporaryDir directory;
    lmsc::SettingsDialog dialog(nullptr, directory.filePath(QStringLiteral("preferences.json")));
    dialog.show();
    auto nav = dialog.findChild<QListWidget *>(QStringLiteral("settingsNavigation"));
    QVERIFY(nav);
    nav->setCurrentRow(4);
    QCOMPARE(nav->currentItem()->text(), QStringLiteral("关于"));
    auto version = dialog.findChild<QLabel *>(QStringLiteral("aboutVersion"));
    auto author = dialog.findChild<QLabel *>(QStringLiteral("aboutAuthor"));
    auto homepage = dialog.findChild<QPushButton *>(QStringLiteral("openProjectHomepage"));
    QVERIFY(version && author && homepage);
    QCOMPARE(version->text(), lmsc::AppInfo::version());
    QCOMPARE(author->text(), QStringLiteral("Vae-x"));
    SettingsHomepageReceiver receiver;
    QDesktopServices::setUrlHandler(QStringLiteral("https"), &receiver, "openUrl");
    QTest::mouseClick(homepage, Qt::LeftButton);
    QDesktopServices::unsetUrlHandler(QStringLiteral("https"));
    QCOMPARE(receiver.openedUrl, QUrl(lmsc::AppInfo::homepageUrl()));
}

void SettingsDialogTest::captureSettings() {
    const QString output = qEnvironmentVariable("LMSC_SETTINGS_CAPTURE_DIRECTORY");
    if (output.isEmpty()) QSKIP("截图仅在指定核验目录时生成");
    QVERIFY(QDir().mkpath(output));
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    lmsc::SettingsDialog dialog(nullptr, directory.filePath(QStringLiteral("preferences.json")));
    dialog.resize(1080, 740);
    dialog.show();
    auto nav = dialog.findChild<QListWidget *>(QStringLiteral("settingsNavigation"));
    auto theme = dialog.findChild<QComboBox *>(QStringLiteral("themeMode"));
    auto sidebar = dialog.findChild<lmsc::NavigationSidebar *>(QStringLiteral("settingsSidebar"));
    auto key = dialog.findChild<QLineEdit *>(QStringLiteral("aiApiKey"));
    QVERIFY(nav && theme && sidebar && key);
    // This fresh temporary store never loads real credentials into exported screenshots.
    QVERIFY(key->text().isEmpty());
    QCOMPARE(key->echoMode(), QLineEdit::Password);
    const auto capture = [&dialog, &output](const QString &fileName) {
        QTest::qWait(80);
        return dialog.grab().save(QDir(output).filePath(fileName));
    };
    theme->setCurrentIndex(theme->findData(QStringLiteral("dark")));
    nav->setCurrentRow(0);
    QVERIFY(capture(QStringLiteral("settings-appearance-dark.png")));
    sidebar->setCollapsed(true);
    QVERIFY(capture(QStringLiteral("settings-appearance-dark-compact.png")));
    nav->setCurrentRow(1);
    QVERIFY(capture(QStringLiteral("settings-ai-dark-compact.png")));
    sidebar->setCollapsed(false);
    QVERIFY(capture(QStringLiteral("settings-ai-dark.png")));
    theme->setCurrentIndex(theme->findData(QStringLiteral("light")));
    QVERIFY(capture(QStringLiteral("settings-ai-light.png")));
    sidebar->setCollapsed(true);
    QVERIFY(capture(QStringLiteral("settings-ai-light-compact.png")));
    nav->setCurrentRow(0);
    QVERIFY(capture(QStringLiteral("settings-appearance-light-compact.png")));
    sidebar->setCollapsed(false);
    QVERIFY(capture(QStringLiteral("settings-appearance-light.png")));
    nav->setCurrentRow(3);
    QVERIFY(capture(QStringLiteral("settings-network-auto-light.png")));
    auto mode = dialog.findChild<QComboBox *>(QStringLiteral("networkProxyMode"));
    mode->setCurrentIndex(mode->findData(QStringLiteral("manual")));
    dialog.findChild<QLineEdit *>(QStringLiteral("networkProxyHost"))->setText(QStringLiteral("127.0.0.1"));
    dialog.findChild<QSpinBox *>(QStringLiteral("networkProxyPort"))->setValue(7890);
    QVERIFY(capture(QStringLiteral("settings-network-manual-light.png")));
    nav->setCurrentRow(4);
    QVERIFY(capture(QStringLiteral("settings-about-light.png")));
    theme->setCurrentIndex(theme->findData(QStringLiteral("dark")));
    QVERIFY(capture(QStringLiteral("settings-about-dark.png")));
    nav->setCurrentRow(3);
    QVERIFY(capture(QStringLiteral("settings-network-manual-dark.png")));

    dialog.resize(760, 500);
    for (const QString &mode : {QStringLiteral("dark"), QStringLiteral("light")}) {
        theme->setCurrentIndex(theme->findData(mode));
        for (bool collapsed : {false, true}) {
            sidebar->setCollapsed(collapsed);
            const QString suffix = collapsed ? QStringLiteral("compact") : QStringLiteral("expanded");
            nav->setCurrentRow(0);
            QVERIFY(capture(QStringLiteral("settings-appearance-%1-%2-760x500.png").arg(mode, suffix)));
            nav->setCurrentRow(1);
            QVERIFY(capture(QStringLiteral("settings-ai-%1-%2-760x500.png").arg(mode, suffix)));
        }
    }
}

QTEST_MAIN(SettingsDialogTest)
#include "SettingsDialogTest.moc"
