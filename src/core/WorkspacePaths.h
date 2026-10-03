#pragma once

#include <QString>

namespace lmsc {

// 开发构建集中保存到仓库 projects；便携版保存到软件旁的 projects。
class WorkspacePaths {
public:
    static QString projectsDirectory(const QString &executableDirectory = {});
    static QString suggestedProjectFile(const QString &title, const QString &projectsRoot = {});
    // 建立分类目录并建议未占用的歌曲目录；实际歌曲由 SongExporter 原子写入。
    static QString suggestedSongExportFolder(const QString &title, const QString &parent,
                                             QString *error = nullptr);
};

} // namespace lmsc
