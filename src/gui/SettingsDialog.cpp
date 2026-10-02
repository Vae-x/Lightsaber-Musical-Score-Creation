#include "SettingsDialog.h"
#include "SettingsPanel.h"
#include <QApplication>
#include <QScreen>
#include <QVBoxLayout>

namespace lmsc {
namespace {
// QWidgetItem normally treats a child layout's preferred height-for-width as
// its minimum. The panel contains a flexible scroll viewport, so preserve the
// layout's real minimum (header/footer and a scrollable body) at this boundary.
class SettingsPanelItem final : public QWidgetItem {
public:
    explicit SettingsPanelItem(QWidget *panel) : QWidgetItem(panel) {}
    int heightForWidth(int width) const override {
        const auto panelLayout = wid->layout();
        if (panelLayout && panelLayout->hasHeightForWidth())
            return qMax(minimumSize().height(), panelLayout->minimumHeightForWidth(width));
        return QWidgetItem::heightForWidth(width);
    }
};
}

SettingsDialog::SettingsDialog(QWidget *parent, const QString &settingsFile)
    : QDialog(parent), m_panel(new SettingsPanel(this, settingsFile)) {
    setObjectName(QStringLiteral("settingsDialog"));
    setWindowTitle(tr("设置 · 光剑曲谱制作"));
    const auto screen = QApplication::primaryScreen();
    const QSize available = screen ? screen->availableGeometry().size() - QSize(50, 80) : QSize(1080, 740);
    resize(qMin(1080, available.width()), qMin(740, available.height()));
    setMinimumSize(qMin(760, available.width()), qMin(500, available.height()));
    auto layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addItem(new SettingsPanelItem(m_panel));
    connect(m_panel, &SettingsPanel::preferencesChanged, this, &SettingsDialog::preferencesChanged);
    connect(m_panel, &SettingsPanel::done, this, &QDialog::accept);
    connect(m_panel, &SettingsPanel::canceled, this, [this] { QDialog::reject(); });
}

SettingsDialog::~SettingsDialog() = default;

void SettingsDialog::reject() {
    m_panel->discardChanges();
    QDialog::reject();
}

} // namespace lmsc
