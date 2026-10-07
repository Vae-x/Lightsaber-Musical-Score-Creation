#pragma once

#include <QString>

namespace lmsc {
namespace AppInfo {

inline QString name() { return QStringLiteral("光剑曲谱制作"); }
inline QString version() { return QStringLiteral("0.6.0"); }
inline QString author() { return QStringLiteral("Vae-x"); }
inline QString homepageUrl() {
    return QStringLiteral("https://github.com/Vae-x/Lightsaber-Musical-Score-Creation");
}

} // namespace AppInfo
} // namespace lmsc
