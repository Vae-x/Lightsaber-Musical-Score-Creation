#pragma once

#include "core/AppSettings.h"
#include <QWidget>

class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QSpinBox;
class QStackedWidget;
class QCheckBox;

namespace lmsc {
class ApiModelClient;
class CodexAccountClient;

// Shared settings content; embedded mode uses the main window's navigation.
class SettingsPanel : public QWidget {
    Q_OBJECT
public:
    explicit SettingsPanel(QWidget *parent = nullptr, const QString &settingsFile = {}, bool embedded = false);
    ~SettingsPanel() override;

    void selectPage(int index);
    bool applyPreferences();
    void discardChanges();

signals:
    void preferencesChanged(const lmsc::AppPreferences &preferences);
    void navigationRequested(int index);
    void done();
    void canceled();

private:
    QWidget *buildAppearancePage();
    QWidget *buildModelPage();
    QWidget *buildAccountPage();
    QWidget *buildNetworkPage();
    QWidget *buildAboutPage();
    NetworkProxyConfig proxyConfig() const;
    void captureProvider();
    void selectProvider(const QString &id);
    void fetchApiModels(bool force = true);
    void replaceModelList(QComboBox *box, const QStringList &models, const QString &selected);
    void setApiStatus(const QString &text, const char *role = "muted");
    void setAccountStatus(const QString &text, const char *role = "muted");
    bool configureCodex();

    AppSettings m_store;
    AppPreferences m_preferences;
    AppPreferences m_savedPreferences;
    QString m_savedTheme;
    QString m_currentProvider;
    QString m_requestProvider;
    QByteArray m_lastRequest;
    ApiModelClient *m_api;
    CodexAccountClient *m_codex;
    QListWidget *m_navigation;
    QStackedWidget *m_pages;
    QStackedWidget *m_connectionPages;
    QComboBox *m_theme;
    QComboBox *m_proxyMode;
    QWidget *m_manualProxy;
    QLineEdit *m_proxyHost;
    QSpinBox *m_proxyPort;
    QSpinBox *m_requestTimeout;
    QCheckBox *m_logEnabled;
    QComboBox *m_connection;
    QComboBox *m_provider;
    QComboBox *m_models;
    QLineEdit *m_baseUrl;
    QLineEdit *m_apiKey;
    QLabel *m_apiStatus;
    QLabel *m_providerLink;
    QPushButton *m_fetchModels;
    QPushButton *m_cancelFetch;
    QLineEdit *m_codexPath;
    QComboBox *m_codexModels;
    QLabel *m_accountStatus;
    QLabel *m_loginLink;
    QLabel *m_saveStatus;
    QPushButton *m_login;
    QPushButton *m_cancelLogin;
    bool m_loadingProvider = false;
    bool m_accountChecked = false;
};
} // namespace lmsc
