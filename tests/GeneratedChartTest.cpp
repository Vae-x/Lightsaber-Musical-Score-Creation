#include "core/BeatmapDocument.h"
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>
#include <limits>

using namespace lmsc;

namespace {
QString audioFixture(QTemporaryDir &root) {
    const QString path = root.filePath("source.ogg");
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write("OggS synthetic metadata-only fixture") < 0) return {};
    return path;
}
QString chartSignature(const BeatmapDocument &document) {
    QString result = document.title() + '|' + document.difficulties().first().name
        + '|' + QString::number(document.difficulties().first().rank);
    for (const auto &o : document.objects())
        result += QStringLiteral("|%1,%2,%3,%4,%5,%6,%7,%8,%9,%10,%11")
            .arg(o.id).arg(int(o.kind)).arg(o.beat, 0, 'g', 17).arg(o.x).arg(o.y)
            .arg(o.color).arg(o.direction).arg(o.duration, 0, 'g', 17).arg(o.width).arg(o.height).arg(o.protectedReason);
    return result;
}
BeatObject note(double beat, int lane, int hand = 0) {
    BeatObject object;
    object.beat = beat; object.x = lane; object.y = 1;
    object.color = hand; object.direction = 1;
    return object;
}
}

class GeneratedChartTest : public QObject {
    Q_OBJECT
private slots:
    void wholeChartAndDifficultyUndoTogether();
    void invalidAndStaleApplicationsAreAtomic();
    void allDifficultyExportsAndProjectRecovery();
    void importedChartsCannotBeReplaced();
    void revisionsNeverReturnToSavedValues();
};

void GeneratedChartTest::wholeChartAndDifficultyUndoTogether() {
    QTemporaryDir root;
    BeatmapDocument doc;
    QString error;
    QVERIFY2(doc.createNew(audioFixture(root), "原歌名", 120, 0.25, {}, &error), qPrintable(error));
    QVERIFY(doc.addObject(note(1, 0), &error));
    QVERIFY(doc.addObject(note(2, 3, 1), &error));
    const QString before = chartSignature(doc);
    const auto originalIds = doc.copyObjects({doc.objects()[0].id, doc.objects()[1].id});
    const quint64 revision = doc.revision();
    QVector<BeatObject> candidate{note(4, 1), note(6, 2, 1)};
    // A supplied ID must never alias an old entry.
    candidate[0].id = doc.objects()[0].id;
    QVERIFY2(doc.applyGeneratedChart(candidate, "Hard", 5, revision, &error), qPrintable(error));
    QCOMPARE(doc.objects().size(), 2);
    QCOMPARE(doc.difficulties().first().name, QString("Hard"));
    QVERIFY(doc.objects()[0].id != candidate[0].id);
    QVERIFY(doc.revision() > revision);
    const QString generated = chartSignature(doc);
    QVERIFY(doc.undo());
    QCOMPARE(chartSignature(doc), before);
    QVERIFY(doc.redo());
    QCOMPARE(chartSignature(doc), generated);
    QVERIFY(doc.setNewSongMetadata("之后改的歌名", "作者", "编谱", &error));
    QVERIFY(doc.undo());
    QCOMPARE(doc.title(), QString("之后改的歌名"));
    QCOMPARE(doc.difficulties().first().name, QString("Expert"));
    QCOMPARE(doc.objects()[0].id, originalIds[0].id);
    QCOMPARE(doc.objects()[1].id, originalIds[1].id);
    QVERIFY(doc.isModified());
}

void GeneratedChartTest::invalidAndStaleApplicationsAreAtomic() {
    QTemporaryDir root;
    BeatmapDocument doc;
    QString error;
    QVERIFY(doc.createNew(audioFixture(root), "校验", 120, 0, {}, &error));
    QVERIFY(doc.addObject(note(1, 0), &error));
    QVERIFY(doc.saveProject(root.filePath("saved/project.lmsc"), &error));
    const QString before = chartSignature(doc);
    const quint64 revision = doc.revision();
    auto reject = [&](QVector<BeatObject> objects, const QString &name, int rank, quint64 expected) {
        QVERIFY(!doc.applyGeneratedChart(objects, name, rank, expected, &error));
        QCOMPARE(chartSignature(doc), before);
        QCOMPARE(doc.revision(), revision);
        QVERIFY(!doc.isModified());
        QVERIFY(doc.canUndo());
        QVERIFY(!doc.canRedo());
    };
    reject({note(4, 1), note(4, 1, 1)}, "Easy", 1, revision);
    auto invalid = note(4, 1); invalid.direction = 9;
    reject({invalid}, "Easy", 1, revision);
    invalid = note(4, 1); invalid.beat = std::numeric_limits<double>::quiet_NaN();
    reject({invalid}, "Easy", 1, revision);
    invalid = note(4, 1); invalid.protectedReason = "受保护";
    reject({invalid}, "Easy", 1, revision);
    reject({note(4, 1)}, "Easy", 9, revision);
    reject({note(4, 1)}, "Easy", 1, revision - 1);
    QVERIFY(doc.applyGeneratedChart({note(4, 1)}, "Easy", 1, revision, &error));
    QVERIFY(doc.undo());
    QCOMPARE(chartSignature(doc), before);
    QVERIFY(!doc.isModified());
}

void GeneratedChartTest::allDifficultyExportsAndProjectRecovery() {
    QTemporaryDir root;
    BeatmapDocument doc;
    QString error;
    QVERIFY(doc.createNew(audioFixture(root), "保存", 123, 0.37, {}, &error));
    const QStringList names{"Easy", "Normal", "Hard", "Expert", "ExpertPlus"};
    const QVector<int> ranks{1, 3, 5, 7, 9};
    for (int i = 0; i < names.size(); ++i) {
        QVERIFY2(doc.applyGeneratedChart({note(4 + i, 1)}, names[i], ranks[i], doc.revision(), &error), qPrintable(error));
        const QString project = root.filePath(names[i] + "/project.lmsc");
        QVERIFY2(doc.saveProject(project, &error), qPrintable(error));
        QVERIFY(!doc.isModified());
        const QString exported = root.filePath("export-" + names[i]);
        QVERIFY2(doc.exportSong(exported, &error), qPrintable(error));
        QVERIFY(QFileInfo::exists(QDir(exported).filePath(names[i] + ".dat")));
        BeatmapDocument reopened;
        QVERIFY2(reopened.loadProject(project, &error), qPrintable(error));
        QVERIFY(reopened.isNewSong());
        QCOMPARE(reopened.difficulties().first().name, names[i]);
        QCOMPARE(reopened.difficulties().first().rank, ranks[i]);
        QCOMPARE(reopened.objects().size(), 1);
        QCOMPARE(reopened.timeMap().beatToSeconds(reopened.objects().first().beat), doc.timeMap().beatToSeconds(doc.objects().first().beat));
        QVERIFY(doc.undo());
        QVERIFY(doc.isModified());
        QVERIFY(doc.redo());
        QVERIFY(!doc.isModified());
    }
}

void GeneratedChartTest::importedChartsCannotBeReplaced() {
    QTemporaryDir root;
    BeatmapDocument original;
    QString error;
    QVERIFY(original.createNew(audioFixture(root), "导入", 120, 0, {}, &error));
    QVERIFY(original.addObject(note(3, 1), &error));
    const QString exported = root.filePath("song");
    QVERIFY(original.exportSong(exported, &error));
    BeatmapDocument imported;
    QVERIFY(imported.loadSong(exported, &error));
    const QString before = chartSignature(imported);
    const auto revision = imported.revision();
    QVERIFY(!imported.applyGeneratedChart({note(4, 0)}, "Easy", 1, revision, &error));
    QCOMPARE(chartSignature(imported), before);
    QCOMPARE(imported.revision(), revision);
}

void GeneratedChartTest::revisionsNeverReturnToSavedValues() {
    QTemporaryDir root;
    BeatmapDocument doc;
    QString error;
    QVERIFY(doc.createNew(audioFixture(root), "修订", 120, 0, {}, &error));
    quint64 previous = doc.revision();
    QVERIFY(doc.addObject(note(4, 0), &error)); QVERIFY(doc.revision() > previous); previous = doc.revision();
    QVERIFY(doc.undo()); QVERIFY(doc.revision() > previous); previous = doc.revision();
    QVERIFY(doc.redo()); QVERIFY(doc.revision() > previous); previous = doc.revision();
    QVERIFY(doc.setNewSongDifficulty("Normal", 3, &error)); QVERIFY(doc.revision() > previous); previous = doc.revision();
    QVERIFY(doc.undo()); QVERIFY(doc.revision() > previous); QCOMPARE(doc.difficulties().first().name, QString("Expert")); previous = doc.revision();
    QVERIFY(doc.setNewSongTempo(150, 0.5, &error)); QVERIFY(doc.revision() > previous); previous = doc.revision();
    QVERIFY(doc.createNew(root.filePath("source.ogg"), "另首", 120, 0, {}, &error));
    QVERIFY(doc.revision() > previous);
}

QTEST_GUILESS_MAIN(GeneratedChartTest)
#include "GeneratedChartTest.moc"
