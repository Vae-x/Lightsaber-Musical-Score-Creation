#include "SettingsPanel.h"
#include "NavigationSidebar.h"
#include "ThemeManager.h"
#include "DiagnosticLogDialog.h"
#include "core/DiagnosticLog.h"
#include "core/ApiModelClient.h"
#include "core/AppInfo.h"
#include "core/CodexAccountClient.h"
#include <QCheckBox>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QFile>
#include <QDir>
#include <QTcpSocket>
#include <QTimer>
#include <QFileDialog>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QSpinBox>
#include <QStyle>
#include <QTextBrowser>
#include <QUrl>
#include <QVBoxLayout>

namespace lmsc {
namespace {
QLabel *description(const QString &text, QWidget *parent, const char *role = "muted") {
    auto label = new QLabel(text, parent);
    label->setWordWrap(true);
    label->setProperty("role", role);
    return label;
}
void statusText(QLabel *label, const QString &text, const char *role) {
    label->setText(text);
    label->setProperty("role", role);
    label->style()->unpolish(label);
    label->style()->polish(label);
    label->update();
}
QWidget *scrollPage(QWidget *content) {
    auto scroll = new QScrollArea;
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setWidget(content);
    return scroll;
}
QVBoxLayout *pageLayout(QWidget *page, const QString &title, const QString &summary) {
    page->setProperty("role", "settingsPage");
    auto layout = new QVBoxLayout(page);
    layout->setContentsMargins(28, 28, 28, 24);
    layout->setSpacing(16);
    layout->addWidget(description(title, page, "pageTitle"));
    if (!summary.isEmpty()) layout->addWidget(description(summary, page));
    return layout;
}
QFrame *settingsCard(QWidget *parent, const QString &title = {}, const QString &summary = {}) {
    auto card = new QFrame(parent);
    card->setProperty("role", "settingsCard");
    auto layout = new QVBoxLayout(card);
    layout->setContentsMargins(20, 18, 20, 18);
    layout->setSpacing(12);
    if (!title.isEmpty()) layout->addWidget(description(title, card, "cardTitle"));
    if (!summary.isEmpty()) layout->addWidget(description(summary, card));
    return card;
}
QFormLayout *settingsForm() {
    auto form = new QFormLayout;
    form->setHorizontalSpacing(16);
    form->setVerticalSpacing(12);
    form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    return form;
}
}

SettingsPanel::SettingsPanel(QWidget *parent, const QString &settingsFile, bool embedded)
    : QWidget(parent), m_store(settingsFile),
      m_api(new ApiModelClient(this)), m_codex(new CodexAccountClient(this)) {
    setObjectName(QStringLiteral("settingsPanel"));
    setProperty("workspaceSurface", true);
    setProperty("role", "settingsPanel");
    setAttribute(Qt::WA_StyledBackground, true);
    QString loadError;
    m_preferences = m_store.load(&loadError);
    m_savedPreferences = m_preferences;
    m_savedTheme = ThemeManager::mode();

    auto outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);
    auto header = new QWidget(this);
    header->setObjectName(QStringLiteral("settingsHeader"));
    auto headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(24, 16, 24, 16);
    headerLayout->setSpacing(12);
    auto appIcon = new QLabel(header);
    appIcon->setPixmap(QIcon(QStringLiteral(":/icons/app.png")).pixmap(32, 32));
    appIcon->setFixedSize(32, 32);
    headerLayout->addWidget(appIcon);
    auto appName = description(AppInfo::name(), header, "settingsAppName");
    appName->setObjectName(QStringLiteral("settingsAppName"));
    headerLayout->addWidget(appName);
    headerLayout->addStretch();
    auto caption = description(tr("偏好设置"), header, "settingsCaption");
    caption->setObjectName(QStringLiteral("settingsCaption"));
    headerLayout->addWidget(caption);
    outer->addWidget(header);
    header->setVisible(!embedded);
    auto body = new QHBoxLayout;
    body->setSpacing(0);
    auto sidebar = new NavigationSidebar(this);
    sidebar->setObjectName(QStringLiteral("settingsSidebar"));
    m_navigation = sidebar->listWidget();
    m_navigation->setObjectName(QStringLiteral("settingsNavigation"));
    sidebar->addItem(tr("外观"), NavigationIcon::Appearance);
    sidebar->addItem(tr("大语言模型"), NavigationIcon::Model);
    sidebar->addItem(tr("账号授权"), NavigationIcon::Account);
    sidebar->addItem(tr("网络"), NavigationIcon::Network);
    sidebar->addItem(tr("关于"), NavigationIcon::About);
    m_pages = new QStackedWidget(this);
    m_pages->setObjectName(QStringLiteral("settingsPages"));
    m_pages->addWidget(scrollPage(buildAppearancePage()));
    m_pages->addWidget(scrollPage(buildModelPage()));
    m_pages->addWidget(scrollPage(buildAccountPage()));
    m_pages->addWidget(scrollPage(buildNetworkPage()));
    m_pages->addWidget(scrollPage(buildAboutPage()));
    body->addWidget(sidebar);
    sidebar->setVisible(!embedded);
    body->addWidget(m_pages, 1);
    outer->addLayout(body, 1);
    auto footer = new QWidget(this);
    footer->setObjectName(QStringLiteral("settingsFooter"));
    auto footerLayout = new QHBoxLayout(footer);
    footerLayout->setContentsMargins(24, 14, 24, 14);
    footerLayout->setSpacing(16);
    m_saveStatus = description(loadError, footer, loadError.isEmpty() ? "muted" : "warning");
    m_saveStatus->setObjectName(QStringLiteral("settingsSaveStatus"));
    footerLayout->addWidget(m_saveStatus, 1);
    auto buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Apply | QDialogButtonBox::Cancel, footer);
    buttons->setObjectName(QStringLiteral("settingsButtons"));
    buttons->button(QDialogButtonBox::Save)->setText(embedded ? tr("保存并返回") : tr("保存并关闭"));
    buttons->button(QDialogButtonBox::Apply)->setText(tr("应用"));
    buttons->button(QDialogButtonBox::Cancel)->setText(embedded ? tr("取消并返回") : tr("取消"));
    buttons->button(QDialogButtonBox::Save)->setProperty("role", "primary");
    connect(buttons, &QDialogButtonBox::accepted, this, [this] { if (applyPreferences()) emit done(); });
    connect(buttons->button(QDialogButtonBox::Apply), &QPushButton::clicked, this, [this] { applyPreferences(); });
    connect(buttons, &QDialogButtonBox::rejected, this, [this] { discardChanges(); emit canceled(); });
    footerLayout->addWidget(buttons);
    outer->addWidget(footer);

    connect(m_navigation, &QListWidget::currentRowChanged, this, [this](int row) { selectPage(row); });
    connect(m_api, &ApiModelClient::modelsReady, this, [this](const QStringList &models) {
        m_fetchModels->setEnabled(true);
        m_cancelFetch->hide();
        if (m_requestProvider != m_currentProvider) return;
        const QString selected = m_models->currentText().trimmed();
        replaceModelList(m_models, models, selected);
        captureProvider();
        m_preferences.providers[m_currentProvider].models = models;
        setApiStatus(tr("已连接，读取到 %1 个模型。请选择模型后保存。列表中的模型不一定都支持对话。").arg(models.size()), "success");
    });
    connect(m_api, &ApiModelClient::requestFailed, this, [this](const QString &error) {
        m_fetchModels->setEnabled(true);
        m_cancelFetch->hide();
        m_lastRequest.clear();
        setApiStatus(error + tr("\n可以修正配置后重试，或手动填写模型名。"), "error");
    });
    connect(m_codex, &CodexAccountClient::accountStatus, this, [this](const QString &message, bool loggedIn) {
        setAccountStatus(message, loggedIn ? "success" : "muted");
        if (loggedIn && m_codexModels->count() == 0) m_codex->fetchModels();
    });
    connect(m_codex, &CodexAccountClient::authorizationRequired, this, [this](const QUrl &url) {
        if (url.scheme() != QStringLiteral("https")) {
            m_codex->cancelLogin();
            setAccountStatus(tr("授权地址无效，请检查 Codex 程序。"), "error");
            return;
        }
        m_loginLink->setText(tr("浏览器未打开时，可<a href=\"%1\">点击这里继续授权</a>。").arg(url.toString().toHtmlEscaped()));
        m_loginLink->show();
        setAccountStatus(tr("请在浏览器中完成 ChatGPT 登录，完成后会自动读取账号与模型。"));
        QDesktopServices::openUrl(url);
    });
    connect(m_codex, &CodexAccountClient::loginFinished, this, [this](bool success, const QString &message) {
        m_login->setEnabled(true);
        m_cancelLogin->hide();
        m_loginLink->hide();
        setAccountStatus(message, success ? "success" : "warning");
        if (success) { m_codex->checkAccount(); m_codex->fetchModels(); }
    });
    connect(m_codex, &CodexAccountClient::modelsReady, this, [this](const QStringList &models) {
        replaceModelList(m_codexModels, models, m_codexModels->currentText().trimmed());
        setAccountStatus(models.isEmpty() ? tr("账号未返回可用模型，请检查权限或手动填写模型名。")
                                         : tr("账号模型列表已更新，共 %1 个模型。").arg(models.size()),
                         models.isEmpty() ? "warning" : "success");
    });
    connect(m_codex, &CodexAccountClient::requestFailed, this, [this](const QString &message) {
        m_login->setEnabled(true);
        m_cancelLogin->hide();
        m_loginLink->hide();
        setAccountStatus(message, "error");
    });

    selectProvider(m_preferences.providerId);
    m_navigation->setCurrentRow(0);
}

SettingsPanel::~SettingsPanel() { m_api->cancel(); m_codex->stop(); }

void SettingsPanel::selectPage(int index) {
    if (index < 0 || index >= m_pages->count()) return;
    if (m_navigation->currentRow() != index) {
        m_navigation->setCurrentRow(index);
        return;
    }
    m_pages->setCurrentIndex(index);
    if (index == 2 && !m_accountChecked) {
        m_accountChecked = true;
        if (configureCodex()) m_codex->checkAccount();
    }
}

QWidget *SettingsPanel::buildAppearancePage() {
    auto page = new QWidget;
    auto layout = pageLayout(page, tr("外观"), tr("选择适合工作环境的配色，让编辑更舒适。"));
    auto group = settingsCard(page);
    auto row = new QHBoxLayout;
    row->setSpacing(24);
    auto copy = new QVBoxLayout;
    copy->setSpacing(6);
    copy->addWidget(description(tr("界面主题"), group, "cardTitle"));
    copy->addWidget(description(tr("网格、时间轴和轨道预览同步切换。"), group));
    row->addLayout(copy, 1);
    m_theme = new QComboBox(group);
    m_theme->setObjectName(QStringLiteral("themeMode"));
    m_theme->addItem(tr("跟随系统"), QStringLiteral("system"));
    m_theme->addItem(tr("浅色"), QStringLiteral("light"));
    m_theme->addItem(tr("深色"), QStringLiteral("dark"));
    m_theme->setCurrentIndex(qMax(0, m_theme->findData(m_preferences.themeMode)));
    m_theme->setAccessibleName(tr("界面主题"));
    m_theme->setMinimumWidth(168);
    row->addWidget(m_theme);
    qobject_cast<QVBoxLayout *>(group->layout())->addLayout(row);
    connect(m_theme, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this] {
        ThemeManager::apply(m_theme->currentData().toString());
    });
    layout->addWidget(group);
    auto editor = settingsCard(page, tr("编辑器配色"), tr("方块的红蓝颜色、切割箭头和选中标记保留一致，便于辨认左右手。"));
    auto legend = new QHBoxLayout;
    legend->setSpacing(24);
    legend->addWidget(description(tr("● 左手 · 红色"), editor, "error"));
    legend->addWidget(description(tr("● 右手 · 蓝色"), editor, "link"));
    legend->addStretch();
    qobject_cast<QVBoxLayout *>(editor->layout())->addLayout(legend);
    layout->addWidget(editor);
    layout->addWidget(description(tr("主题会立即预览。点击应用或保存后，下次启动会沿用；取消会恢复上次应用的主题。"), page));
    layout->addStretch();
    return page;
}

QWidget *SettingsPanel::buildModelPage() {
    auto page = new QWidget;
    auto layout = pageLayout(page, tr("大语言模型"), tr("选择连接方式，配置提供商与可用模型。"));
    auto connectionCard = settingsCard(page, tr("连接方式"));
    auto connectionForm = settingsForm();
    m_connection = new QComboBox(connectionCard);
    m_connection->setObjectName(QStringLiteral("aiConnection"));
    m_connection->addItem(tr("API Key · OpenAI 兼容协议"), QStringLiteral("api"));
    m_connection->addItem(tr("Codex · ChatGPT 账号授权"), QStringLiteral("codex"));
    m_connection->setCurrentIndex(qMax(0, m_connection->findData(m_preferences.aiConnection)));
    connectionForm->addRow(tr("使用方式"), m_connection);
    qobject_cast<QVBoxLayout *>(connectionCard->layout())->addLayout(connectionForm);
    layout->addWidget(connectionCard);
    m_connectionPages = new QStackedWidget(page);
    auto apiPage = new QWidget;
    auto apiLayout = new QVBoxLayout(apiPage);
    apiLayout->setContentsMargins(0, 0, 0, 0);
    apiLayout->setSpacing(16);
    auto apiCard = settingsCard(apiPage, tr("API 配置"));
    auto apiCardLayout = qobject_cast<QVBoxLayout *>(apiCard->layout());
    auto form = settingsForm();
    m_provider = new QComboBox(apiCard);
    m_provider->setObjectName(QStringLiteral("aiProvider"));
    for (const auto &preset : AppSettings::providerPresets()) m_provider->addItem(preset.name, preset.id);
    form->addRow(tr("提供商"), m_provider);
    auto urlRow = new QWidget(apiCard);
    auto urlLayout = new QHBoxLayout(urlRow);
    urlLayout->setContentsMargins(0, 0, 0, 0);
    m_baseUrl = new QLineEdit(urlRow);
    m_baseUrl->setObjectName(QStringLiteral("aiBaseUrl"));
    m_baseUrl->setPlaceholderText(QStringLiteral("https://example.com/v1"));
    auto resetUrl = new QPushButton(tr("恢复预设"), urlRow);
    resetUrl->setObjectName(QStringLiteral("resetProviderUrl"));
    urlLayout->addWidget(m_baseUrl, 1);
    urlLayout->addWidget(resetUrl);
    form->addRow(tr("API 地址"), urlRow);
    auto keyRow = new QWidget(apiCard);
    auto keyLayout = new QHBoxLayout(keyRow);
    keyLayout->setContentsMargins(0, 0, 0, 0);
    m_apiKey = new QLineEdit(keyRow);
    m_apiKey->setObjectName(QStringLiteral("aiApiKey"));
    m_apiKey->setEchoMode(QLineEdit::Password);
    m_apiKey->setPlaceholderText(tr("填入该提供商的 API Key"));
    auto showKey = new QCheckBox(tr("显示"), keyRow);
    showKey->setObjectName(QStringLiteral("showApiKey"));
    auto clearKey = new QPushButton(tr("清除"), keyRow);
    keyLayout->addWidget(m_apiKey, 1);
    keyLayout->addWidget(showKey);
    keyLayout->addWidget(clearKey);
    form->addRow(tr("API Key"), keyRow);
    m_models = new QComboBox(apiCard);
    m_models->setObjectName(QStringLiteral("aiModel"));
    m_models->setEditable(true);
    m_models->setInsertPolicy(QComboBox::NoInsert);
    m_models->lineEdit()->setPlaceholderText(tr("获取后选择，或手动输入模型名"));
    form->addRow(tr("模型"), m_models);
    apiCardLayout->addLayout(form);
    apiCardLayout->addWidget(description(tr("填入密钥后离开输入框，会自动读取模型。每个提供商的地址、密钥与模型分别保存。"), apiCard));
    auto fetchRow = new QHBoxLayout;
    m_fetchModels = new QPushButton(tr("获取模型 / 检查连接"), apiCard);
    m_fetchModels->setObjectName(QStringLiteral("fetchApiModels"));
    m_cancelFetch = new QPushButton(tr("取消读取"), apiCard);
    m_cancelFetch->setObjectName(QStringLiteral("cancelApiModels"));
    m_cancelFetch->hide();
    fetchRow->addWidget(m_fetchModels);
    fetchRow->addWidget(m_cancelFetch);
    fetchRow->addStretch();
    apiCardLayout->addLayout(fetchRow);
    m_apiStatus = description(tr("选择提供商，填入 API Key。"), apiCard);
    m_apiStatus->setObjectName(QStringLiteral("apiConnectionStatus"));
    apiCardLayout->addWidget(m_apiStatus);
    m_providerLink = description({}, apiCard);
    m_providerLink->setOpenExternalLinks(true);
    apiCardLayout->addWidget(m_providerLink);
    apiLayout->addWidget(apiCard);
    apiLayout->addStretch();
    m_connectionPages->addWidget(apiPage);
    auto accountPage = new QWidget;
    auto accountLayout = new QVBoxLayout(accountPage);
    accountLayout->setContentsMargins(0, 0, 0, 0);
    auto accountCard = settingsCard(accountPage, tr("ChatGPT 账号"), tr("使用已授权的 ChatGPT 账号及其可用 Codex 模型。"));
    auto configure = new QPushButton(tr("配置 Codex 账号授权"), accountCard);
    qobject_cast<QVBoxLayout *>(accountCard->layout())->addWidget(configure, 0, Qt::AlignLeft);
    accountLayout->addWidget(accountCard);
    connect(configure, &QPushButton::clicked, this, [this] { selectPage(2); emit navigationRequested(2); });
    accountLayout->addStretch();
    m_connectionPages->addWidget(accountPage);
    m_connectionPages->setCurrentIndex(m_connection->currentIndex());
    connect(m_connection, QOverload<int>::of(&QComboBox::currentIndexChanged), m_connectionPages, &QStackedWidget::setCurrentIndex);
    layout->addWidget(m_connectionPages);
    layout->addWidget(description(tr("当前完成连接与模型配置，自动制谱将在后续接入。Windows 会为当前用户加密保存 API Key；设置不会进入歌曲工程或导出包。"), page));
    layout->addStretch();
    connect(m_provider, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this] {
        if (!m_loadingProvider) selectProvider(m_provider->currentData().toString());
    });
    connect(resetUrl, &QPushButton::clicked, this, [this] {
        for (const auto &preset : AppSettings::providerPresets()) if (preset.id == m_currentProvider) {
            m_api->cancel(); m_lastRequest.clear();
            m_fetchModels->setEnabled(true); m_cancelFetch->hide();
            m_baseUrl->setText(preset.baseUrl);
            setApiStatus(tr("已恢复 API 地址预设。"));
            return;
        }
    });
    connect(showKey, &QCheckBox::toggled, this, [this](bool visible) {
        m_apiKey->setEchoMode(visible ? QLineEdit::Normal : QLineEdit::Password);
    });
    connect(clearKey, &QPushButton::clicked, m_apiKey, &QLineEdit::clear);
    const auto invalidate = [this] {
        if (m_loadingProvider) return;
        m_api->cancel(); m_lastRequest.clear();
        m_fetchModels->setEnabled(true); m_cancelFetch->hide();
        m_preferences.providers[m_currentProvider].models.clear();
        replaceModelList(m_models, {}, m_models->currentText());
        setApiStatus(tr("配置已修改，请重新获取模型。"));
    };
    connect(m_baseUrl, &QLineEdit::textChanged, this, invalidate);
    connect(m_apiKey, &QLineEdit::textChanged, this, invalidate);
    connect(m_apiKey, &QLineEdit::editingFinished, this, [this] { fetchApiModels(false); });
    connect(m_baseUrl, &QLineEdit::editingFinished, this, [this] { fetchApiModels(false); });
    connect(m_fetchModels, &QPushButton::clicked, this, [this] { fetchApiModels(); });
    connect(m_cancelFetch, &QPushButton::clicked, this, [this] {
        m_api->cancel(); m_lastRequest.clear();
        m_fetchModels->setEnabled(true); m_cancelFetch->hide();
        setApiStatus(tr("已取消模型读取。"));
    });
    return page;
}

QWidget *SettingsPanel::buildAccountPage() {
    auto page = new QWidget;
    auto layout = pageLayout(page, tr("账号授权"), tr("通过 Codex 的官方登录流程连接 ChatGPT 账号，读取授权状态与模型。"));
    auto account = settingsCard(page, tr("Codex 账号"));
    auto accountLayout = qobject_cast<QVBoxLayout *>(account->layout());
    auto form = settingsForm();
    auto executable = new QWidget(account);
    auto executableLayout = new QHBoxLayout(executable);
    executableLayout->setContentsMargins(0, 0, 0, 0);
    m_codexPath = new QLineEdit(executable);
    m_codexPath->setObjectName(QStringLiteral("codexExecutable"));
    m_codexPath->setPlaceholderText(tr("留空自动查找 Codex；也可选择 codex.exe"));
    m_codexPath->setText(m_preferences.codexExecutable);
    auto browse = new QPushButton(tr("浏览"), executable);
    executableLayout->addWidget(m_codexPath, 1);
    executableLayout->addWidget(browse);
    form->addRow(tr("Codex 程序"), executable);
    m_codexModels = new QComboBox(account);
    m_codexModels->setObjectName(QStringLiteral("codexModel"));
    m_codexModels->setEditable(true);
    m_codexModels->setInsertPolicy(QComboBox::NoInsert);
    m_codexModels->lineEdit()->setPlaceholderText(tr("授权后获取，或手动填写模型名"));
    m_codexModels->setEditText(m_preferences.codexModel);
    form->addRow(tr("账号模型"), m_codexModels);
    accountLayout->addLayout(form);
    auto actions = new QHBoxLayout;
    auto check = new QPushButton(tr("检查授权"), account);
    check->setObjectName(QStringLiteral("checkCodexAccount"));
    m_login = new QPushButton(tr("使用 ChatGPT 登录"), account);
    m_login->setObjectName(QStringLiteral("loginCodexAccount"));
    m_cancelLogin = new QPushButton(tr("取消授权"), account);
    m_cancelLogin->hide();
    actions->addWidget(check);
    actions->addWidget(m_login);
    actions->addWidget(m_cancelLogin);
    actions->addStretch();
    accountLayout->addLayout(actions);
    auto fetch = new QPushButton(tr("获取账号模型"), account);
    fetch->setObjectName(QStringLiteral("fetchCodexModels"));
    accountLayout->addWidget(fetch, 0, Qt::AlignLeft);
    m_accountStatus = description(tr("打开本页后自动检查本机 Codex 授权。"), account);
    m_accountStatus->setObjectName(QStringLiteral("codexAccountStatus"));
    accountLayout->addWidget(m_accountStatus);
    m_loginLink = description({}, account);
    m_loginLink->setOpenExternalLinks(true);
    m_loginLink->hide();
    accountLayout->addWidget(m_loginLink);
    layout->addWidget(account);
    layout->addWidget(description(tr("需要本机安装 Codex CLI。已有账号授权可直接使用；登录状态保存在 Codex 自己的凭据存储中。取消设置不会退出已有账号。"), page));
    auto documentation = description(tr("<a href=\"https://developers.openai.com/codex/app-server\">官方接入文档</a> · <a href=\"https://github.com/openai/codex\">Codex 开源项目</a>"), page);
    documentation->setOpenExternalLinks(true);
    layout->addWidget(documentation);
    layout->addStretch();
    connect(browse, &QPushButton::clicked, this, [this] {
        const QString path = QFileDialog::getOpenFileName(this, tr("选择 Codex 程序"), {}, tr("可执行程序 (*.exe);;所有文件 (*)"));
        if (!path.isEmpty()) m_codexPath->setText(path);
    });
    connect(check, &QPushButton::clicked, this, [this] {
        if (!configureCodex()) return;
        setAccountStatus(tr("正在检查账号授权…")); m_codex->checkAccount();
    });
    connect(m_login, &QPushButton::clicked, this, [this] {
        if (!configureCodex()) return;
        m_login->setEnabled(false); m_cancelLogin->show();
        setAccountStatus(tr("正在准备浏览器授权…"));
        m_codex->beginLogin();
    });
    connect(m_cancelLogin, &QPushButton::clicked, this, [this] { m_codex->cancelLogin(); });
    connect(fetch, &QPushButton::clicked, this, [this] {
        if (!configureCodex()) return;
        setAccountStatus(tr("正在读取账号模型…")); m_codex->fetchModels();
    });
    return page;
}

QWidget *SettingsPanel::buildNetworkPage() {
    auto page = new QWidget;
    auto layout = pageLayout(page, tr("网络"), tr("配置 AI 连接使用的网络代理。"));
    auto group = settingsCard(page, tr("代理设置"), tr("默认使用系统代理，也可指定 HTTP 代理。"));
    auto groupLayout = qobject_cast<QVBoxLayout *>(group->layout());
    auto modeForm = settingsForm();
    m_proxyMode = new QComboBox(group);
    m_proxyMode->setObjectName(QStringLiteral("networkProxyMode"));
    m_proxyMode->addItem(tr("自动（系统代理）"), QStringLiteral("system"));
    m_proxyMode->addItem(tr("手动（HTTP 代理）"), QStringLiteral("manual"));
    m_proxyMode->addItem(tr("直连（不使用代理）"), QStringLiteral("direct"));
    m_proxyMode->setCurrentIndex(qMax(0, m_proxyMode->findData(m_preferences.networkProxy.mode)));
    modeForm->addRow(tr("代理模式"), m_proxyMode);
    m_requestTimeout=new QSpinBox(group); m_requestTimeout->setObjectName("aiRequestTimeoutMinutes");
    m_requestTimeout->setRange(1,30); m_requestTimeout->setValue(m_preferences.requestTimeoutMinutes);
    modeForm->addRow(tr("单次请求超时（分钟）"),m_requestTimeout);
    groupLayout->addLayout(modeForm);
    m_manualProxy = new QWidget(group);
    auto manualForm = settingsForm();
    m_manualProxy->setLayout(manualForm);
    manualForm->setContentsMargins(0, 0, 0, 0);
    m_proxyHost = new QLineEdit(m_manualProxy);
    m_proxyHost->setObjectName(QStringLiteral("networkProxyHost"));
    m_proxyHost->setPlaceholderText(tr("例如 127.0.0.1 或 proxy.example.com"));
    m_proxyHost->setText(m_preferences.networkProxy.host);
    manualForm->addRow(tr("代理地址"), m_proxyHost);
    m_proxyPort = new QSpinBox(m_manualProxy);
    m_proxyPort->setObjectName(QStringLiteral("networkProxyPort"));
    m_proxyPort->setRange(1, 65535);
    m_proxyPort->setValue(m_preferences.networkProxy.port);
    manualForm->addRow(tr("端口"), m_proxyPort);
    manualForm->addRow(description(tr("地址只填 IP 或域名，端口单独填写。"), m_manualProxy));
    groupLayout->addWidget(m_manualProxy);
    auto probeButton=new QPushButton(tr("检查代理端口"),group); probeButton->setObjectName("checkProxyConnection");
    auto probeStatus=description({},group); probeStatus->setObjectName("proxyCheckStatus");
    groupLayout->addWidget(probeButton,0,Qt::AlignLeft); groupLayout->addWidget(probeStatus);
    probeButton->setEnabled(m_preferences.networkProxy.mode=="manual");
    connect(m_proxyMode,QOverload<int>::of(&QComboBox::currentIndexChanged),this,[this,probeButton] {
        probeButton->setEnabled(m_proxyMode->currentData().toString()=="manual");
    });
    connect(probeButton,&QPushButton::clicked,this,[this,probeButton,probeStatus] {
        const auto proxy=proxyConfig();
        const auto error=AppSettings::validateProxy(proxy);
        if (!error.isEmpty()) { statusText(probeStatus,error,"error"); return; }
        probeButton->setEnabled(false); statusText(probeStatus,tr("正在检查 HTTP 代理端口…"),"muted");
        auto socket=new QTcpSocket(this); auto timer=new QTimer(socket); timer->setSingleShot(true);
        const auto finish=[this,socket,timer,probeButton,probeStatus,proxy](const QString &text,bool ok) {
            if (socket->property("probeDone").toBool()) return;
            socket->setProperty("probeDone",true); timer->stop(); socket->abort();
            probeButton->setEnabled(m_proxyMode->currentData().toString()=="manual");
            statusText(probeStatus,text,ok ? "success" : "error");
            DiagnosticLog::instance().record("proxy.probe",{{"proxyMode",proxy.mode},{"host",proxy.host},{"port",proxy.port},
                {"category",ok ? "httpProxy" : "network"}}); socket->deleteLater();
        };
        connect(socket,&QTcpSocket::connected,this,[socket] {
            socket->write("CONNECT 127.0.0.1:1 HTTP/1.1\r\nHost: 127.0.0.1:1\r\n\r\n");
        });
        connect(socket,&QTcpSocket::readyRead,this,[socket,finish] {
            QByteArray response=socket->property("probeResponse").toByteArray()+socket->read(1024);
            socket->setProperty("probeResponse",response);
            if (!response.contains('\n') && response.size()<1024) return;
            const bool http=response.startsWith("HTTP/1.0 ") || response.startsWith("HTTP/1.1 ");
            finish(http ? QObject::tr("端口能响应 HTTP 代理请求；请继续获取模型验证实际服务连接。")
                        : QObject::tr("端口未返回 HTTP 代理响应，请核对 HTTP 或混合端口。"),http);
        });
        connect(socket,QOverload<QAbstractSocket::SocketError>::of(&QTcpSocket::error),this,[finish](QAbstractSocket::SocketError) {
            finish(QObject::tr("无法连接代理端口，请检查代理程序、地址和端口。"),false);
        });
        connect(timer,&QTimer::timeout,this,[finish] { finish(QObject::tr("代理端口检查超时。"),false); });
        timer->start(5000); socket->connectToHost(proxy.host,quint16(proxy.port));
    });
    m_manualProxy->setEnabled(m_proxyMode->currentData().toString() == QStringLiteral("manual"));
    layout->addWidget(group);
    layout->addWidget(description(tr("点击应用或保存后，下次启动会沿用。"), page));
    layout->addWidget(description(tr("自动模式下，API 使用系统代理，Codex 使用自身的默认网络配置。手动代理用于两种 AI 连接，本机服务保持直连。"), page));
    layout->addWidget(description(tr("打开项目主页或浏览器授权时，浏览器沿用自己的网络设置。"), page));
    layout->addStretch();
    const auto changed = [this] {
        m_manualProxy->setEnabled(m_proxyMode->currentData().toString() == QStringLiteral("manual"));
        m_api->cancel(); m_lastRequest.clear();
        m_fetchModels->setEnabled(true); m_cancelFetch->hide();
        m_codex->stop(); m_accountChecked = false;
        m_login->setEnabled(true); m_cancelLogin->hide(); m_loginLink->hide();
        setApiStatus(tr("代理设置已修改，请重新获取模型。"));
        setAccountStatus(tr("代理设置已修改，请重新检查授权或获取模型。"));
    };
    connect(m_proxyMode, QOverload<int>::of(&QComboBox::currentIndexChanged), this, changed);
    connect(m_proxyHost, &QLineEdit::textChanged, this, changed);
    connect(m_proxyPort, QOverload<int>::of(&QSpinBox::valueChanged), this, changed);
    return page;
}

QWidget *SettingsPanel::buildAboutPage() {
    auto page = new QWidget;
    auto layout = pageLayout(page, tr("关于"), tr("为音乐创作属于你的光剑曲谱。"));
    auto group = settingsCard(page);
    auto groupLayout = qobject_cast<QVBoxLayout *>(group->layout());
    auto heading = new QHBoxLayout;
    heading->setSpacing(16);
    auto icon = new QLabel(group);
    icon->setPixmap(QIcon(QStringLiteral(":/icons/app.png")).pixmap(64, 64));
    icon->setFixedSize(64, 64);
    heading->addWidget(icon);
    auto identity = new QVBoxLayout;
    identity->setSpacing(6);
    identity->addWidget(description(AppInfo::name(), group, "cardTitle"));
    identity->addWidget(description(tr("Qt5 / C++ 曲谱编辑器"), group));
    heading->addLayout(identity, 1);
    groupLayout->addLayout(heading);
    auto form = settingsForm();
    auto version = new QLabel(AppInfo::version(), group);
    version->setObjectName(QStringLiteral("aboutVersion"));
    version->setTextInteractionFlags(Qt::TextSelectableByMouse);
    form->addRow(tr("版本"), version);
    auto author = new QLabel(AppInfo::author(), group);
    author->setObjectName(QStringLiteral("aboutAuthor"));
    author->setTextInteractionFlags(Qt::TextSelectableByMouse);
    form->addRow(tr("作者"), author);
    form->addRow(tr("许可"), new QLabel(tr("GNU GPL 第 3 版"), group));
    groupLayout->addLayout(form);
    layout->addWidget(group);
    auto homepage = new QPushButton(tr("打开项目主页"), page);
    auto logs=settingsCard(page,tr("诊断日志"),tr("保留最近七天的请求阶段与错误记录，最多 25 MiB。"));
    auto logsLayout=qobject_cast<QVBoxLayout *>(logs->layout());
    m_logEnabled=new QCheckBox(tr("启用诊断日志"),logs); m_logEnabled->setObjectName("diagnosticLogEnabled");
    m_logEnabled->setChecked(m_preferences.diagnosticLogEnabled); logsLayout->addWidget(m_logEnabled);
    auto logActions=new QHBoxLayout;
    auto viewLog=new QPushButton(tr("查看 / 导出日志"),logs); viewLog->setObjectName("viewDiagnosticLogs");
    auto openLogs=new QPushButton(tr("打开日志目录"),logs); openLogs->setObjectName("openDiagnosticLogDirectory");
    logActions->addWidget(viewLog); logActions->addWidget(openLogs); logActions->addStretch(); logsLayout->addLayout(logActions);
    connect(viewLog,&QPushButton::clicked,this,[this] { showDiagnosticLog(this); });
    connect(openLogs,&QPushButton::clicked,this,[] {
        const auto directory=DiagnosticLog::instance().directory(); QDir().mkpath(directory);
        QDesktopServices::openUrl(QUrl::fromLocalFile(directory));
    });
    layout->addWidget(logs);
    homepage->setObjectName(QStringLiteral("openProjectHomepage"));
    homepage->setToolTip(AppInfo::homepageUrl());
    layout->addWidget(homepage, 0, Qt::AlignLeft);
    auto status = description({}, page);
    status->setObjectName(QStringLiteral("aboutHomepageStatus"));
    status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(status);
    connect(homepage, &QPushButton::clicked, this, [status] {
        if (QDesktopServices::openUrl(QUrl(AppInfo::homepageUrl()))) {
            statusText(status, tr("已在默认浏览器打开项目主页。"), "success");
        } else {
            statusText(status, tr("无法打开浏览器，请复制地址后访问：\n%1").arg(AppInfo::homepageUrl()), "error");
        }
    });
    layout->addWidget(description(tr("基于 Qt5 / C++ 的曲谱编辑器。第三方组件分别遵循各自许可。"), page));
    auto licenseButton = new QPushButton(tr("查看 GPLv3 许可"), page);
    licenseButton->setObjectName(QStringLiteral("showGplLicense"));
    layout->addWidget(licenseButton, 0, Qt::AlignLeft);
    auto licenseText = new QTextBrowser(page);
    licenseText->setObjectName(QStringLiteral("gplLicenseText"));
    licenseText->setOpenExternalLinks(false);
    licenseText->setFixedHeight(280);
    licenseText->hide();
    layout->addWidget(licenseText);
    connect(licenseButton, &QPushButton::clicked, this, [licenseButton, licenseText] {
        if (licenseText->isHidden()) {
            QFile file(QStringLiteral(":/licenses/GPL-3.0.txt"));
            if (file.open(QIODevice::ReadOnly)) licenseText->setPlainText(QString::fromUtf8(file.readAll()));
            else licenseText->setPlainText(tr("无法读取 GPLv3 许可全文。"));
            licenseText->show();
            licenseButton->setText(tr("收起 GPLv3 许可"));
        } else {
            licenseText->hide();
            licenseButton->setText(tr("查看 GPLv3 许可"));
        }
    });
    layout->addStretch();
    return page;
}

NetworkProxyConfig SettingsPanel::proxyConfig() const {
    NetworkProxyConfig proxy;
    proxy.mode = m_proxyMode->currentData().toString();
    proxy.host = m_proxyHost->text().trimmed();
    proxy.port = m_proxyPort->value();
    return proxy;
}

bool SettingsPanel::configureCodex() {
    const auto proxy = proxyConfig();
    const QString error = AppSettings::validateProxy(proxy);
    if (!error.isEmpty()) { setAccountStatus(error, "error"); return false; }
    m_codex->setProxyConfig(proxy);
    m_codex->setExecutablePath(m_codexPath->text().trimmed());
    return true;
}

void SettingsPanel::captureProvider() {
    if (m_currentProvider.isEmpty()) return;
    auto &config = m_preferences.providers[m_currentProvider];
    config.baseUrl = m_baseUrl->text().trimmed();
    config.apiKey = m_apiKey->text().trimmed();
    config.model = m_models->currentText().trimmed();
}

void SettingsPanel::selectProvider(const QString &id) {
    captureProvider();
    m_api->cancel(); m_lastRequest.clear();
    m_fetchModels->setEnabled(true); m_cancelFetch->hide();
    m_loadingProvider = true;
    int index = m_provider->findData(id);
    if (index < 0) index = 0;
    m_provider->setCurrentIndex(index);
    m_currentProvider = m_provider->currentData().toString();
    auto config = m_preferences.providers.value(m_currentProvider);
    for (const auto &preset : AppSettings::providerPresets()) if (preset.id == m_currentProvider) {
        if (!m_preferences.providers.contains(m_currentProvider)) config.baseUrl = preset.baseUrl;
        m_providerLink->setText(preset.documentationUrl.isEmpty() ? QString() : tr("<a href=\"%1\">查看提供商文档与密钥说明</a>").arg(preset.documentationUrl.toHtmlEscaped()));
        break;
    }
    m_baseUrl->setText(config.baseUrl);
    m_apiKey->setText(config.apiKey);
    m_apiKey->setEchoMode(QLineEdit::Password);
    if (auto *show = findChild<QCheckBox *>(QStringLiteral("showApiKey"))) {
        const QSignalBlocker blocker(show);
        show->setChecked(false);
    }
    replaceModelList(m_models, config.models, config.model);
    m_preferences.providers[m_currentProvider] = config;
    m_loadingProvider = false;
    setApiStatus(config.models.isEmpty() ? tr("填入 API Key 后可自动获取模型，也支持手动填写。")
                                       : tr("显示上次获取的 %1 个模型；可重新读取以检查连接。").arg(config.models.size()));
}

void SettingsPanel::replaceModelList(QComboBox *box, const QStringList &models, const QString &selected) {
    const QSignalBlocker blocker(box);
    box->clear();
    box->addItems(models);
    if (!selected.isEmpty()) box->setEditText(selected);
    else if (!models.isEmpty()) box->setCurrentIndex(0);
    else box->setEditText({});
}

void SettingsPanel::fetchApiModels(bool force) {
    if (m_loadingProvider || m_connection->currentData().toString() != QStringLiteral("api")) return;
    const QString base = m_baseUrl->text().trimmed();
    const QString key = m_apiKey->text().trimmed();
    if (!force && (key.isEmpty() || base.isEmpty())) return;
    const auto proxy = proxyConfig();
    const QString proxyError = AppSettings::validateProxy(proxy);
    if (!proxyError.isEmpty()) { setApiStatus(proxyError, "error"); return; }
    const QByteArray fingerprint = QCryptographicHash::hash((m_currentProvider + QChar(0) + base + QChar(0) + key).toUtf8(), QCryptographicHash::Sha256);
    if (!force && fingerprint == m_lastRequest) return;
    captureProvider();
    m_requestProvider = m_currentProvider;
    m_lastRequest = fingerprint;
    m_fetchModels->setEnabled(false); m_cancelFetch->show();
    setApiStatus(tr("正在连接并读取模型列表…"));
    m_api->setProxyConfig(proxy);
    m_api->fetchModels(base, key, m_currentProvider);
}

bool SettingsPanel::applyPreferences() {
    captureProvider();
    m_preferences.themeMode = m_theme->currentData().toString();
    m_preferences.aiConnection = m_connection->currentData().toString();
    m_preferences.providerId = m_currentProvider;
    m_preferences.codexExecutable = m_codexPath->text().trimmed();
    m_preferences.codexModel = m_codexModels->currentText().trimmed();
    m_preferences.networkProxy = proxyConfig();
    m_preferences.requestTimeoutMinutes=m_requestTimeout->value();
    m_preferences.diagnosticLogEnabled=m_logEnabled->isChecked();
    QString error;
    if (!m_store.save(m_preferences, &error)) {
        statusText(m_saveStatus, tr("保存失败：") + error, "error");
        return false;
    }
    m_savedPreferences = m_preferences;
    DiagnosticLog::instance().setEnabled(m_preferences.diagnosticLogEnabled);
    m_savedTheme = m_preferences.themeMode;
    ThemeManager::apply(m_savedTheme);
    statusText(m_saveStatus, tr("设置已保存。"), "success");
    emit preferencesChanged(m_preferences);
    return true;
}

void SettingsPanel::discardChanges() {
    m_api->cancel();
    m_codex->stop();
    m_lastRequest.clear();
    m_requestProvider.clear();
    m_accountChecked = false;
    m_preferences = m_savedPreferences;

    const QSignalBlocker themeBlocker(m_theme);
    const QSignalBlocker connectionBlocker(m_connection);
    const QSignalBlocker proxyModeBlocker(m_proxyMode);
    const QSignalBlocker proxyHostBlocker(m_proxyHost);
    const QSignalBlocker proxyPortBlocker(m_proxyPort);
    m_theme->setCurrentIndex(qMax(0, m_theme->findData(m_preferences.themeMode)));
    m_connection->setCurrentIndex(qMax(0, m_connection->findData(m_preferences.aiConnection)));
    m_connectionPages->setCurrentIndex(m_connection->currentIndex());
    m_proxyMode->setCurrentIndex(qMax(0, m_proxyMode->findData(m_preferences.networkProxy.mode)));
    m_proxyHost->setText(m_preferences.networkProxy.host);
    m_proxyPort->setValue(m_preferences.networkProxy.port);
    m_requestTimeout->setValue(m_preferences.requestTimeoutMinutes);
    m_logEnabled->setChecked(m_preferences.diagnosticLogEnabled);
    m_manualProxy->setEnabled(m_proxyMode->currentData().toString() == QStringLiteral("manual"));
    m_codexPath->setText(m_preferences.codexExecutable);
    replaceModelList(m_codexModels, {}, m_preferences.codexModel);
    // Prevent selectProvider from capturing the canceled fields into the restored snapshot.
    m_currentProvider.clear();
    selectProvider(m_preferences.providerId);
    m_login->setEnabled(true);
    m_cancelLogin->hide();
    m_loginLink->clear();
    m_loginLink->hide();
    setAccountStatus(tr("打开本页后自动检查本机 Codex 授权。"));
    statusText(m_saveStatus, {}, "muted");
    ThemeManager::apply(m_savedTheme);
}
void SettingsPanel::setApiStatus(const QString &text, const char *role) { statusText(m_apiStatus, text, role); }
void SettingsPanel::setAccountStatus(const QString &text, const char *role) { statusText(m_accountStatus, text, role); }
} // namespace lmsc
