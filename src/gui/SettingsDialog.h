#pragma once

#include "core/AppSettings.h"
#include <QDialog>

namespace lmsc {
class SettingsPanel;

// Optional dialog host for the reusable settings panel.
class SettingsDialog : public QDialog {
    Q_OBJECT
public:
    explicit SettingsDialog(QWidget *parent = nullptr, const QString &settingsFile = {});
    ~SettingsDialog() override;

signals:
    void preferencesChanged(const lmsc::AppPreferences &preferences);

protected:
    void reject() override;

private:
    SettingsPanel *m_panel;
};
} // namespace lmsc
