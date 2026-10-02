#pragma once

#include "TimeMap.h"
#include <QJsonObject>
#include <QVector>
#include <QStringList>
#include <memory>

namespace lmsc {

enum class ObjectKind { Note, Bomb, Wall };

struct BeatObject {
    QString id;
    ObjectKind kind = ObjectKind::Note;
    double beat = 0.0;
    int x = 0;
    int y = 0;
    int color = 0;
    int direction = 8;
    double duration = 1.0;
    int width = 1;
    int height = 5;
    QString protectedReason;
    bool isProtected() const { return !protectedReason.isEmpty(); }
};

struct Difficulty {
    QString id;
    QString characteristic;
    QString name;
    QString filename;
    int rank = 0;
    QString version;
};

struct ImportSource {
    QString path;
    int streamIndex = -1;
    double startSeconds = 0.0;
    double endSeconds = -1.0;
    bool isAvailable() const { return !path.isEmpty(); }
};

class BeatmapDocument {
public:
    BeatmapDocument();
    ~BeatmapDocument();
    BeatmapDocument(BeatmapDocument &&) noexcept;
    BeatmapDocument &operator=(BeatmapDocument &&) noexcept;
    BeatmapDocument(const BeatmapDocument &) = delete;
    BeatmapDocument &operator=(const BeatmapDocument &) = delete;

    bool loadSong(const QString &folder, QString *error = nullptr);
    bool loadZip(const QString &filename, QString *error = nullptr);
    // A project directory, its project.lmsc file, or an autosave.lmsc recovery file.
    bool loadProject(const QString &path, QString *error = nullptr);
    bool createNew(const QString &audioPath, const QString &title, double bpm,
                   double firstBeatSeconds = 0.0, const QString &coverPath = {},
                   QString *error = nullptr);

    bool isLoaded() const;
    QString title() const;
    QString audioPath() const;
    QString coverPath() const;
    QString projectPath() const;
    bool isNewSong() const;
    const QVector<Difficulty> &difficulties() const;
    QString currentDifficultyId() const;
    bool setDifficulty(const QString &id, QString *error = nullptr);
    const QVector<BeatObject> &objects() const;
    const TimeMap &timeMap() const;
    QString readOnlyReason() const;
    QStringList warnings() const;
    // Monotonic within this document, including undo/redo and timing changes.
    quint64 revision() const;

    bool addObject(const BeatObject &object, QString *error = nullptr);
    bool updateObject(const BeatObject &object, QString *error = nullptr);
    bool updateObjects(const QVector<BeatObject> &objects, QString *error = nullptr);
    bool removeObjects(const QStringList &ids, QString *error = nullptr);
    QVector<BeatObject> copyObjects(const QStringList &ids, QString *error = nullptr) const;
    bool pasteObjects(const QVector<BeatObject> &objects, double beatOffset,
                      int xOffset = 0, bool mirror = false, QString *error = nullptr);
    bool mirrorObjects(const QStringList &ids, QString *error = nullptr);
    // New songs only: replace the entire chart and its difficulty in one command.
    bool applyGeneratedChart(const QVector<BeatObject> &objects,
                             const QString &difficultyName, int difficultyRank,
                             quint64 expectedRevision, QString *error = nullptr);
    bool canUndo() const;
    bool canRedo() const;
    bool undo();
    bool redo();
    bool isModified() const;
    bool setNewSongTempo(double bpm, double firstBeatSeconds, QString *error = nullptr);
    bool setNewSongDifficulty(const QString &name, int rank, QString *error = nullptr);
    bool setNewSongMetadata(const QString &title, const QString &artist,
                           const QString &mapper, QString *error = nullptr);
    bool setImportSource(const QString &path, int absoluteStreamIndex,
                         double startSeconds, double endSeconds, QString *error = nullptr);
    ImportSource importSource() const;

    bool saveProject(const QString &path, QString *error = nullptr);
    bool autoSave(QString *error = nullptr);
    bool exportSong(const QString &destinationFolder, QString *error = nullptr) const;
    // Allows the caller to offer recovery before opening a saved project.
    static QString recoveryPath(const QString &projectPath);
    static bool hasRecovery(const QString &projectPath);

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace lmsc
