#include "ThemeManager.h"

#include <QApplication>
#include <QEvent>
#include <QPalette>
#include <QPointer>
#include <QSettings>
#include <QStyle>
#include <QTimer>
#include <QWidget>

namespace {
QString currentMode = QStringLiteral("system");
bool currentDark = false;
bool applyingTheme = false;
bool nativePaletteCaptured = false;
QPalette nativePalette;
QPointer<QObject> systemWatcher;

bool systemIsDark() {
#ifdef Q_OS_WIN
    // The application preference differs from the Windows shell preference.
    const QSettings personalisation(
        QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize"),
        QSettings::NativeFormat);
    return personalisation.value(QStringLiteral("AppsUseLightTheme"), 1).toInt() == 0;
#else
    return nativePalette.color(QPalette::Window).lightness() < 128;
#endif
}

QString stylesheet(bool dark) {
    QString css = QStringLiteral(R"(
        QWidget { background: @window; color: @text; font-size: 13px; }
        QLabel { background: transparent; }
        QLabel[role="title"] { color: @text; font-size: 17px; font-weight: 600; }
        QLabel[role="muted"], QLabel[role="status"] { color: @muted; }
        QLabel[role="warning"] { color: @warning; }
        QLabel[role="success"] { color: @success; }
        QLabel[role="error"] { color: @error; }
        QWidget:disabled { color: @disabled; }
        QGroupBox { border: 1px solid @border; border-radius: 5px;
                    margin-top: 11px; padding-top: 10px; }
        QGroupBox::title { subcontrol-origin: margin; left: 9px; }
        QPushButton, QToolButton, QComboBox, QSpinBox, QDoubleSpinBox, QLineEdit {
            background: @control; border: 1px solid @border; border-radius: 3px;
            padding: 5px; selection-background-color: @selected;
            selection-color: @selectedText;
        }
        QPushButton:hover, QToolButton:hover { background: @hover; }
        QPushButton:pressed, QToolButton:pressed, QToolButton:checked {
            background: @selected; color: @selectedText;
        }
        QPushButton:focus, QToolButton:focus, QLineEdit:focus, QComboBox:focus,
        QSpinBox:focus, QDoubleSpinBox:focus { border: 1px solid @accent; }
        QPushButton:disabled, QToolButton:disabled { color: @disabled; }
        QComboBox { padding-right: 21px; }
        QComboBox::drop-down { width: 19px; border-left: 1px solid @border; }
        QComboBox::down-arrow { image: url(:/icons/@chevron); width: 12px; height: 8px; }
        QListWidget, QListView, QTreeView, QTableView, QTextEdit, QPlainTextEdit,
        QTextBrowser, QComboBox QAbstractItemView {
            background: @base; alternate-background-color: @control;
            border: 1px solid @border; selection-background-color: @selected;
            selection-color: @selectedText;
        }
        QListWidget::item, QListView::item { padding: 4px; }
        QListWidget::item:selected, QListView::item:selected,
        QComboBox QAbstractItemView::item:selected { background: @selected; color: @selectedText; }
        QMenu, QMenuBar, QToolBar { background: @panel; }
        QMenu { border: 1px solid @border; padding: 3px; }
        QMenu::item { padding: 5px 26px; }
        QMenu::item:selected, QMenuBar::item:selected { background: @selected; color: @selectedText; }
        QMenu::separator { height: 1px; background: @border; margin: 4px 8px; }
        QToolBar { border: 0; spacing: 3px; }
        QToolBar::separator { background: @border; width: 1px; margin: 5px; }
        QTabWidget::pane { border: 1px solid @border; background: @window; }
        QTabBar::tab { background: @control; color: @text; border: 1px solid @border;
                       padding: 7px 14px; margin-right: 3px; }
        QTabBar::tab:selected { background: @selected; color: @selectedText; }
        QTabBar::tab:hover:!selected { background: @hover; }
        QSplitter::handle { background: @border; }
        QScrollArea { border: 0; }
        QStatusBar { background: @panel; color: @muted; }
        QStatusBar::item { border: 0; }
        QProgressBar { background: @base; border: 1px solid @border; border-radius: 3px;
                       text-align: center; }
        QProgressBar::chunk { background: @accent; border-radius: 2px; }
        QSlider::groove:horizontal { background: @border; height: 5px; border-radius: 2px; }
        QSlider::handle:horizontal { background: @accent; width: 13px;
                                     margin: -4px 0; border-radius: 6px; }
        QScrollBar:horizontal { background: @base; height: 15px; margin: 0 15px; }
        QScrollBar:vertical { background: @base; width: 15px; margin: 15px 0; }
        QScrollBar::handle { background: @border; min-width: 25px; min-height: 25px; }
        QScrollBar::handle:hover { background: @muted; }
        QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }
        QToolTip { background: @control; color: @text; border: 1px solid @border; padding: 4px; }

        /* Hallmark · genre modern-minimal · macrostructure navigation workbench
           critique P4 H4 E4 S4 R5 V4: quiet surfaces and clear form hierarchy. */
        QDialog#settingsDialog { background: @settingsWindow; }
        QDialog#settingsDialog QWidget { background: transparent; }
        QDialog#settingsDialog QLabel { font-size: 14px; }
        QDialog#settingsDialog QWidget#settingsHeader {
            background: @settingsSidebar; border-bottom: 1px solid @settingsBorder;
        }
        QDialog#settingsDialog QWidget#settingsSidebar {
            background: @settingsSidebar; border-right: 1px solid @settingsBorder;
        }
        QDialog#settingsDialog QWidget#settingsFooter {
            background: @settingsWindow; border-top: 1px solid @settingsBorder;
        }
        QDialog#settingsDialog QListWidget#settingsNavigation {
            background: transparent; border: 0; outline: 0; padding: 0;
        }
        QDialog#settingsDialog QListWidget#settingsNavigation::item {
            background: transparent; border: 0; padding: 0;
        }
        QDialog#settingsDialog QListWidget#settingsNavigation::item:selected {
            background: transparent; color: @text;
        }
        QDialog#settingsDialog QLabel[role="pageTitle"] {
            font-size: 26px; font-weight: 600;
        }
        QDialog#settingsDialog QLabel[role="cardTitle"],
        QDialog#settingsDialog QLabel[role="settingSection"] {
            font-size: 14px; font-weight: 600;
        }
        QDialog#settingsDialog QLabel#settingsAppName { font-size: 14px; font-weight: 600; }
        QDialog#settingsDialog QLabel#settingsCaption { color: @muted; }
        QDialog#settingsDialog QLabel[role="link"] { color: @noteBlue; }
        QDialog#settingsDialog QFrame[role="settingsCard"] {
            background: @settingsCard; border: 1px solid @settingsBorder; border-radius: 8px;
        }
        QDialog#settingsDialog QPushButton, QDialog#settingsDialog QComboBox,
        QDialog#settingsDialog QLineEdit, QDialog#settingsDialog QSpinBox,
        QDialog#settingsDialog QDoubleSpinBox {
            background: @settingsWindow; border: 1px solid @settingsBorder;
            border-radius: 5px; padding: 6px 9px; font-size: 14px;
        }
        QDialog#settingsDialog QPushButton:hover, QDialog#settingsDialog QComboBox:hover,
        QDialog#settingsDialog QLineEdit:hover, QDialog#settingsDialog QSpinBox:hover,
        QDialog#settingsDialog QDoubleSpinBox:hover {
            background: @settingsHover;
        }
        QDialog#settingsDialog QPushButton:pressed,
        QDialog#settingsDialog QPushButton:checked {
            background: @settingsBorder; color: @text;
        }
        QDialog#settingsDialog QPushButton:disabled, QDialog#settingsDialog QComboBox:disabled,
        QDialog#settingsDialog QLineEdit:disabled, QDialog#settingsDialog QSpinBox:disabled,
        QDialog#settingsDialog QDoubleSpinBox:disabled {
            background: @settingsSidebar; color: @disabled; border-color: @settingsBorder;
        }
        QDialog#settingsDialog QPushButton:focus, QDialog#settingsDialog QComboBox:focus,
        QDialog#settingsDialog QLineEdit:focus, QDialog#settingsDialog QSpinBox:focus,
        QDialog#settingsDialog QDoubleSpinBox:focus { border-color: @accent; }
        QDialog#settingsDialog QComboBox { padding-right: 30px; }
        QDialog#settingsDialog QComboBox::drop-down { width: 26px; border: 0; }
        QDialog#settingsDialog QComboBox QLineEdit,
        QDialog#settingsDialog QComboBox QLineEdit:focus,
        QDialog#settingsDialog QComboBox QLineEdit:hover,
        QDialog#settingsDialog QComboBox QLineEdit:disabled {
            background: transparent; border: 0; border-radius: 0; padding: 0;
        }
        QDialog#settingsDialog QComboBox QAbstractItemView {
            background: @settingsCard; border-color: @settingsBorder;
        }
        QDialog#settingsDialog QPushButton[role="primary"] {
            background: @accent; color: @primaryText; border-color: @accent; font-weight: 600;
        }
        QDialog#settingsDialog QPushButton[role="primary"]:hover {
            background: @primaryHover; border-color: @primaryHover;
        }
        QDialog#settingsDialog QPushButton[role="primary"]:pressed {
            background: @primaryPressed; border-color: @primaryPressed;
        }
        QDialog#settingsDialog QPushButton[role="primary"]:focus {
            border: 1px solid @text;
        }
        QDialog#settingsDialog QPushButton[role="primary"]:disabled {
            background: @settingsBorder; color: @disabled; border-color: @settingsBorder;
        }
        QDialog#settingsDialog QToolButton#navigationToggle {
            background: transparent; border: 1px solid transparent; border-radius: 6px; padding: 0;
        }
        QDialog#settingsDialog QToolButton#navigationToggle:hover { background: @settingsHover; }
        QDialog#settingsDialog QToolButton#navigationToggle:pressed {
            background: @settingsBorder; color: @text;
        }
        QDialog#settingsDialog QToolButton#navigationToggle:focus { border-color: @accent; }
        QDialog#settingsDialog QToolButton#navigationToggle:disabled { color: @disabled; }
        QDialog#settingsDialog QCheckBox { font-size: 14px; spacing: 8px; }
        QDialog#settingsDialog QCheckBox::indicator {
            width: 16px; height: 16px; background: @settingsWindow;
            border: 1px solid @settingsBorder; border-radius: 3px;
        }
        QDialog#settingsDialog QCheckBox::indicator:hover,
        QDialog#settingsDialog QCheckBox::indicator:focus { border-color: @accent; }
        QDialog#settingsDialog QCheckBox::indicator:checked {
            background: @accent; border-color: @accent; image: url(:/icons/check-@checkVariant.svg);
        }
        QDialog#settingsDialog QCheckBox::indicator:disabled {
            background: @settingsSidebar; border-color: @settingsBorder;
        }
        QDialog#settingsDialog QCheckBox::indicator:checked:disabled {
            background: @disabled; border-color: @disabled;
        }
        QDialog#settingsDialog QScrollBar:vertical {
            background: transparent; width: 10px; margin: 0;
        }
        QDialog#settingsDialog QScrollBar:horizontal {
            background: transparent; height: 10px; margin: 0;
        }
        QDialog#settingsDialog QScrollBar::handle {
            background: @settingsBorder; border-radius: 5px; min-width: 0; min-height: 0;
        }
        QDialog#settingsDialog QScrollBar::handle:vertical { min-height: 32px; }
        QDialog#settingsDialog QScrollBar::handle:horizontal { min-width: 32px; }
        QDialog#settingsDialog QScrollBar::handle:hover { background: @settingsScrollHover; }
        QDialog#settingsDialog QScrollBar::add-line,
        QDialog#settingsDialog QScrollBar::sub-line {
            background: transparent; border: 0; width: 0; height: 0;
        }
        QDialog#settingsDialog QScrollBar::up-arrow, QDialog#settingsDialog QScrollBar::down-arrow,
        QDialog#settingsDialog QScrollBar::left-arrow, QDialog#settingsDialog QScrollBar::right-arrow {
            image: none; width: 0; height: 0;
        }
    )");
    const auto color = [&](const char *token, const char *darkColor, const char *lightColor) {
        css.replace(QString::fromLatin1(token), QString::fromLatin1(dark ? darkColor : lightColor));
    };
    color("@window", "#171d28", "#f3f5f8");
    color("@text", "#dae2ef", "#243247");
    color("@muted", "#a0aec2", "#596b82");
    color("@warning", "#e9c881", "#93620b");
    color("@success", "#66d3b4", "#127656");
    color("@error", "#ff939e", "#b72f43");
    color("@disabled", "#69778c", "#8995a5");
    color("@border", "#3e4b61", "#cbd3df");
    color("@control", "#253044", "#ffffff");
    color("@hover", "#35455e", "#e3ebf4");
    // Longer tokens must be replaced before their prefix.
    color("@selectedText", "#ffffff", "#ffffff");
    color("@selected", "#355d88", "#3d6f9f");
    color("@accent", "#42d3bd", "#168578");
    color("@base", "#121925", "#ffffff");
    color("@panel", "#202937", "#e8edf4");
    color("@settingsWindow", "#18191c", "#f6f7f9");
    color("@settingsSidebar", "#111214", "#eef0f3");
    color("@settingsCard", "#232529", "#ffffff");
    color("@settingsBorder", "#36393f", "#dce0e6");
    color("@settingsHover", "#2d3035", "#e5e8ee");
    color("@settingsScrollHover", "#70757d", "#a3a9b3");
    color("@noteBlue", "#7db3ff", "#2469bb");
    color("@primaryText", "#102724", "#ffffff");
    color("@primaryHover", "#68dec9", "#11796d");
    color("@primaryPressed", "#2ab9a4", "#0d675d");
    css.replace(QStringLiteral("@chevron"), dark ? QStringLiteral("chevron-dark.svg") : QStringLiteral("chevron-light.svg"));
    css.replace(QStringLiteral("@checkVariant"), dark ? QStringLiteral("dark") : QStringLiteral("light"));
    return css;
}

QPalette palette(bool dark) {
    QPalette result = nativePalette;
    const auto color = [&](QPalette::ColorRole role, const char *darkColor, const char *lightColor) {
        result.setColor(role, QColor(QString::fromLatin1(dark ? darkColor : lightColor)));
    };
    color(QPalette::Window, "#171d28", "#f3f5f8");
    color(QPalette::WindowText, "#dae2ef", "#243247");
    color(QPalette::Base, "#121925", "#ffffff");
    color(QPalette::AlternateBase, "#253044", "#e8edf4");
    color(QPalette::Text, "#dae2ef", "#243247");
    color(QPalette::Button, "#253044", "#ffffff");
    color(QPalette::ButtonText, "#dae2ef", "#243247");
    color(QPalette::ToolTipBase, "#253044", "#ffffff");
    color(QPalette::ToolTipText, "#dae2ef", "#243247");
    color(QPalette::Highlight, "#355d88", "#3d6f9f");
    result.setColor(QPalette::HighlightedText, Qt::white);
    color(QPalette::Link, "#42d3bd", "#168578");
    color(QPalette::LinkVisited, "#c3abeb", "#725795");
    color(QPalette::Light, "#53637d", "#ffffff");
    color(QPalette::Midlight, "#3e4b61", "#e8edf4");
    color(QPalette::Mid, "#3e4b61", "#cbd3df");
    color(QPalette::Dark, "#101620", "#a8b4c4");
    color(QPalette::Shadow, "#090d13", "#8290a3");
    const QColor disabled(QString::fromLatin1(dark ? "#69778c" : "#8995a5"));
    result.setColor(QPalette::Disabled, QPalette::WindowText, disabled);
    result.setColor(QPalette::Disabled, QPalette::Text, disabled);
    result.setColor(QPalette::Disabled, QPalette::ButtonText, disabled);
    return result;
}

class SystemThemeWatcher final : public QObject {
public:
    explicit SystemThemeWatcher(QObject *parent) : QObject(parent) {
        qApp->installEventFilter(this);
        // Registry reads every two seconds keep this compatible with Qt 5.12,
        // whose Windows platform plugin does not expose an OS color-scheme signal.
        auto timer = new QTimer(this);
        timer->setInterval(2000);
        timer->setTimerType(Qt::VeryCoarseTimer);
        connect(timer, &QTimer::timeout, this, [] {
            if (currentMode == QStringLiteral("system") && systemIsDark() != currentDark)
                lmsc::ThemeManager::apply(currentMode);
        });
        timer->start();
    }

protected:
    bool eventFilter(QObject *object, QEvent *event) override {
        if (!applyingTheme && object == qApp && currentMode == QStringLiteral("system") &&
            (event->type() == QEvent::ApplicationPaletteChange || event->type() == QEvent::ThemeChange)) {
#ifndef Q_OS_WIN
            nativePalette = qApp->palette();
#endif
            lmsc::ThemeManager::apply(currentMode);
        }
        return QObject::eventFilter(object, event);
    }
};
}

namespace lmsc {

void ThemeManager::apply(const QString &mode) {
    if (!qApp || applyingTheme)
        return;
    if (!nativePaletteCaptured) {
        nativePalette = qApp->palette();
        nativePaletteCaptured = true;
    }
    currentMode = mode == QStringLiteral("dark") || mode == QStringLiteral("light")
                      ? mode : QStringLiteral("system");
    currentDark = currentMode == QStringLiteral("dark") ||
                  (currentMode == QStringLiteral("system") && systemIsDark());
    applyingTheme = true;
    qApp->setPalette(palette(currentDark));
    qApp->setStyleSheet(stylesheet(currentDark));
    // QPainter-based editors and QOpenGLWidget previews do not inherit their
    // custom drawn colors from QSS, so schedule a repaint for every widget.
    for (QWidget *widget : QApplication::allWidgets())
        widget->update();
    applyingTheme = false;
}

QString ThemeManager::mode() { return currentMode; }
bool ThemeManager::isDark() { return currentDark; }

void ThemeManager::watchSystemChanges(QObject *parent) {
    if (qApp && !systemWatcher)
        systemWatcher = new SystemThemeWatcher(parent ? parent : qApp);
}

} // namespace lmsc
