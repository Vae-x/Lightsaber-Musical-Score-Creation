#pragma once

#include <QString>

namespace lmsc {
class BeatmapDocument;

// The exported copy may have a silent lead-in; the project and its assets stay unchanged.
class SongExporter {
public:
    static bool exportSong(const BeatmapDocument &document, const QString &destinationFolder,
                           double leadInSeconds, const QString &toolsDirectory,
                           QString *error = nullptr);
};
}
