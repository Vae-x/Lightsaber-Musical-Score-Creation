#pragma once

#include <QString>

namespace lmsc {

// 开发构建集中保存到仓库 projects；便携版保存到软件旁的 projects。
class WorkspacePaths {
public:
    static QString projectsDirectory(const QString &executableDirectory = {});
    static QString suggestedProjectFile(const QString &title, const QString &projectsRoot = {});
};

} // namespace lmsc
