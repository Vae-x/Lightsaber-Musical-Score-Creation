#include "core/DiagnosticLog.h"
#include "core/AppSettings.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>
class DiagnosticLogTest : public QObject {
    Q_OBJECT
private slots:
    void motifQualityFieldsSurviveExportWithoutCandidateText() {
        QTemporaryDir directory; lmsc::DiagnosticLog log(directory.path());
        log.record("generation.motifQuality",{{"jobId","quality-job"},{"segment",7},{"referenceSegment",2},
            {"referenceNotes",14},{"currentNotes",8},{"matchedActions",8},{"actionDifference",0.0},
            {"rhythmCoverage",8.0/14},{"positionDifference",0.25},{"countDifference",6.0/14},
            {"repeatConfidence",0.96},{"targetVariation",0.1},{"decision","acceptedWithWarning"},
            {"previousCandidate","private-candidate"},{"qualityFeedback","private-feedback"},{"prompt","private-prompt"}});
        const auto content=log.read("quality-job"); const auto row=QJsonDocument::fromJson(content.trimmed()).object();
        QCOMPARE(row.value("segment").toInt(),7); QCOMPARE(row.value("referenceSegment").toInt(),2);
        QCOMPARE(row.value("referenceNotes").toInt(),14); QCOMPARE(row.value("currentNotes").toInt(),8);
        QCOMPARE(row.value("matchedActions").toInt(),8); QCOMPARE(row.value("decision").toString(),QString("acceptedWithWarning"));
        QCOMPARE(row.value("targetVariation").toDouble(),0.1); QCOMPARE(row.value("repeatConfidence").toDouble(),0.96);
        QCOMPARE(row.value("rhythmCoverage").toDouble(),8.0/14); QCOMPARE(row.value("countDifference").toDouble(),6.0/14);
        QCOMPARE(row.value("actionDifference").toDouble(),0.0); QCOMPARE(row.value("positionDifference").toDouble(),0.25);
        QVERIFY(!content.contains("private"));
    }
    void controlledFieldsAndFiltering() {
        QTemporaryDir directory; lmsc::DiagnosticLog log(directory.path());
        log.addSecret("real-test-api-key");
        log.record("request.failed",{{"jobId","job-1"},{"requestId","request-1"},{"category","authentication"},
            {"model","real-test-api-key"},{"code","person@example.com"},{"httpStatus",401},
            {"authorization","Bearer secret"},{"prompt","private song text"},{"stderr","OAuth login URL"},{"path","C:/Users/private/song.mp3"}});
        log.record("request.started",{{"jobId","job-2"},{"model","model-2"}});
        const auto content=log.read("job-1");
        QVERIFY(!content.contains("real-test-api-key")); QVERIFY(!content.contains("person@example"));
        QVERIFY(!content.contains("Bearer")); QVERIFY(!content.contains("private")); QVERIFY(!content.contains("OAuth"));
        QVERIFY(!content.contains("job-2")); QVERIFY(content.contains("authentication"));
        const auto row=QJsonDocument::fromJson(content.trimmed()).object(); QCOMPARE(row.value("httpStatus").toInt(),401);
        QVERIFY(row.contains("build"));
    }
    void rotationAndExpiryPreserveUnrelatedFiles() {
        QTemporaryDir directory; lmsc::DiagnosticLog log(directory.path(),1024,3);
        QFile unrelated(directory.filePath("keep.txt")); QVERIFY(unrelated.open(QIODevice::WriteOnly)); unrelated.write("keep"); unrelated.close();
        for (int i=0;i<60;++i) log.record("generation.progress",{{"jobId","job-1"},{"segment",i}});
        auto files=QDir(directory.path()).entryInfoList({"diagnostic-*.jsonl"},QDir::Files);
        QCOMPARE(files.size(),3);
        for (const auto &file:files) QVERIFY(file.size()<=1024);
        QFile old(directory.filePath("diagnostic-2.jsonl")); QVERIFY(old.open(QIODevice::ReadWrite));
        QVERIFY(old.setFileTime(QDateTime::currentDateTimeUtc().addDays(-8),QFileDevice::FileModificationTime)); old.close();
        log.record("expiry.checked"); QVERIFY(!QFile::exists(old.fileName())); QVERIFY(QFile::exists(unrelated.fileName()));
    }
    void exportNeverOverwritesAndResanitizes() {
        QTemporaryDir directory; lmsc::DiagnosticLog log(directory.path()); log.record("request.started",{{"jobId","job-1"}});
        QFile edited(directory.filePath("diagnostic-0.jsonl")); QVERIFY(edited.open(QIODevice::WriteOnly|QIODevice::Append));
        edited.write("{\"event\":\"external\",\"jobId\":\"job-1\",\"apiKey\":\"private-key\",\"code\":\"user@example.com\"}\n"); edited.close();
        const auto target=directory.filePath("export.jsonl"); QString error;
        QVERIFY(log.exportTo(target,"job-1",&error)); QFile out(target); QVERIFY(out.open(QIODevice::ReadOnly)); const auto before=out.readAll(); out.close();
        QVERIFY(!before.contains("private-key")); QVERIFY(!before.contains("user@example"));
        QVERIFY(!log.exportTo(target,"job-1",&error)); QVERIFY(!error.isEmpty()); QVERIFY(out.open(QIODevice::ReadOnly)); QCOMPARE(out.readAll(),before);
    }
    void disabledAndUnwritableAreNonFatal() {
        QTemporaryDir directory; lmsc::DiagnosticLog log(directory.filePath("logs")); log.setEnabled(false); log.record("disabled"); QVERIFY(log.read().isEmpty());
        QFile blocker(directory.filePath("blocker")); QVERIFY(blocker.open(QIODevice::WriteOnly)); blocker.close();
        lmsc::DiagnosticLog blocked(blocker.fileName()); blocked.record("cannot.write"); QVERIFY(!blocked.lastError().isEmpty());
        log.setEnabled(true); log.record("enabled"); QVERIFY(log.read().contains("enabled"));
    }
    void concurrentEventsAndPreferenceCompatibility() {
        QTemporaryDir directory; lmsc::DiagnosticLog log(directory.filePath("logs"));
        QVector<QThread *> workers;
        for (int i=0;i<3;++i) { auto thread=QThread::create([&log] { for (int j=0;j<20;++j) log.record("concurrent",{{"segment",j}}); }); workers.append(thread); thread->start(); }
        for (auto thread:workers) { QVERIFY(thread->wait(5000)); delete thread; }
        QCOMPARE(log.read().count('\n'),60);
        lmsc::AppSettings settings(directory.filePath("settings.json")); auto preferences=settings.load();
        QCOMPARE(preferences.requestTimeoutMinutes,10); QVERIFY(preferences.diagnosticLogEnabled);
        preferences.requestTimeoutMinutes=20; preferences.diagnosticLogEnabled=false; preferences.networkProxy.mode="direct";
        QString error; QVERIFY2(settings.save(preferences,&error),qPrintable(error));
        const auto restored=settings.load(&error); QVERIFY(error.isEmpty()); QCOMPARE(restored.requestTimeoutMinutes,20);
        QVERIFY(!restored.diagnosticLogEnabled); QCOMPARE(restored.networkProxy.mode,QString("direct"));
        preferences.requestTimeoutMinutes=31; QVERIFY(!settings.save(preferences,&error));
    }
};
QTEST_GUILESS_MAIN(DiagnosticLogTest)
#include "DiagnosticLogTest.moc"
