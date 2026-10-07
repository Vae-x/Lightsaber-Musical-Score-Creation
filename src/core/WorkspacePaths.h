#pragma once

#include <QString>

namespace lmsc {

// 开发构建集中保存到仓库 projects；便携版保存到软件旁；安装版保存到用户文档。
class WorkspacePaths {
public:
    // documentsDirectory 为空时使用系统文档目录；显式目录便于隔离路径验证。
    static QString projectsDirectory(const QString &executableDirectory = {},
                                     const QString &documentsDirectory = {});
    static QString suggestedProjectFile(const QString &title, const QString &projectsRoot = {});
    // 建立分类目录并建议未占用的歌曲目录；实际歌曲由 SongExporter 原子写入。
    static QString suggestedSongExportFolder(const QString &title, const QString &parent,
                                             QString *error = nullptr);
};

} // namespace lmsc
