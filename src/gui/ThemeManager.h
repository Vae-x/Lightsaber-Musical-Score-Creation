#ifndef LMSC_THEME_MANAGER_H
#define LMSC_THEME_MANAGER_H

#include <QString>

class QObject;

namespace lmsc {

// Global application theme. Valid modes are "system", "light" and "dark".
class ThemeManager {
public:
    static void apply(const QString &mode);
    static QString mode();
    static bool isDark();
    static void watchSystemChanges(QObject *parent);
};

} // namespace lmsc

#endif // LMSC_THEME_MANAGER_H
