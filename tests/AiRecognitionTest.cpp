#include "core/AiRecognitionService.h"
#include "gui/AiRecognitionPage.h"
#include <QtTest>
#include <QFile>
#include <QLabel>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QTemporaryDir>

namespace {
class FakeRecognitionService final : public lmsc::AiRecognitionService {
public:
    bool available = true;
    QVector<lmsc::AiRecognitionRequest> requests;
    QStringList cancelledIds;

    bool isAvailable() const override { return available; }
    void analyze(const lmsc::AiRecognitionRequest &request) override { requests.append(request); }
    void cancel(const QString &contextId) override {
        cancelledIds.append(contextId);
        emit cancelled(contextId);
    }
    void finish(const QString &contextId, const QString &summary) {
        emit recognitionFinished({contextId, summary, {{1.25, QStringLiteral("强拍"), 0.9}}});
    }
};
}

class AiRecognitionTest : public QObject {
    Q_OBJECT
private slots:
    void initTestCase();
    void unavailableServiceFailsWithoutFabricatedResult();
    void unavailableServiceCancellationSuppressesFailure();
    void pageRequiresAvailableServiceAndLocalAudio();
    void requestAndResultFlow();
    void cancelledAndStaleResultsAreIgnored();
    void replacementAndDestroyedBackendAreSafe();
    void busyContextAndAvailabilityCancelPendingWork();
private:
    QTemporaryDir m_temp;
    QString m_audio;
};

void AiRecognitionTest::initTestCase() {
    QVERIFY(m_temp.isValid());
    m_audio = m_temp.filePath(QStringLiteral("fixture.ogg"));
    QFile fixture(m_audio);
    QVERIFY(fixture.open(QIODevice::WriteOnly));
    QCOMPARE(fixture.write("test-only-local-audio"), qint64(21));
}

void AiRecognitionTest::unavailableServiceFailsWithoutFabricatedResult() {
    lmsc::UnavailableAiRecognitionService service;
    QVERIFY(!service.isAvailable());
    QSignalSpy failed(&service, &lmsc::AiRecognitionService::requestFailed);
    QSignalSpy finished(&service, &lmsc::AiRecognitionService::recognitionFinished);
    lmsc::AiRecognitionRequest request;
    request.contextId = QStringLiteral("unavailable-request");
    request.audioFile = m_audio;
    service.analyze(request);
    QTRY_COMPARE(failed.count(), 1);
    QCOMPARE(failed.at(0).at(0).toString(), request.contextId);
    QVERIFY(failed.at(0).at(1).toString().contains(QStringLiteral("尚未接入")));
    QCOMPARE(finished.count(), 0);
}

void AiRecognitionTest::unavailableServiceCancellationSuppressesFailure() {
    lmsc::UnavailableAiRecognitionService service;
    QSignalSpy failed(&service, &lmsc::AiRecognitionService::requestFailed);
    QSignalSpy cancelled(&service, &lmsc::AiRecognitionService::cancelled);
    lmsc::AiRecognitionRequest request;
    request.contextId = QStringLiteral("cancel-before-dispatch");
    service.analyze(request);
    service.cancel(request.contextId);
    QCoreApplication::processEvents();
    QCOMPARE(cancelled.count(), 1);
    QCOMPARE(cancelled.at(0).at(0).toString(), request.contextId);
    QCOMPARE(failed.count(), 0);
}

void AiRecognitionTest::pageRequiresAvailableServiceAndLocalAudio() {
    lmsc::AiRecognitionPage page;
    auto start = page.findChild<QPushButton *>(QStringLiteral("aiRecognizeButton"));
    auto configure = page.findChild<QPushButton *>(QStringLiteral("aiConfigureConnection"));
    auto result = page.findChild<QPlainTextEdit *>(QStringLiteral("aiRecognitionResult"));
    QVERIFY(start && configure && result);
    QVERIFY(!start->isEnabled());
    QVERIFY(result->isReadOnly());
    QSignalSpy configuration(&page, &lmsc::AiRecognitionPage::configureConnectionRequested);
    configure->click();
    QCOMPARE(configuration.count(), 1);
    page.setContext(m_audio, QStringLiteral("识别测试歌曲"), 120.0, 0.0, 12.0, false);
    QVERIFY(!start->isEnabled());
    FakeRecognitionService service;
    page.setService(&service);
    QVERIFY(start->isEnabled());
    page.setContext(m_temp.filePath(QStringLiteral("missing.ogg")), {}, 120.0, 0.0, 12.0, false);
    QVERIFY(!start->isEnabled());
    page.setContext(m_temp.path(), {}, 120.0, 0.0, 12.0, false);
    QVERIFY(!start->isEnabled());
    page.setContext(m_audio, {}, 0.0, 0.0, 12.0, false);
    QVERIFY(!start->isEnabled());
    QCOMPARE(service.requests.count(), 0);
}

void AiRecognitionTest::requestAndResultFlow() {
    FakeRecognitionService service;
    lmsc::AiRecognitionPage page;
    page.setService(&service);
    page.setContext(m_audio, QStringLiteral("识别测试歌曲"), 142.5, 0.125, 12.0, false);
    auto start = page.findChild<QPushButton *>(QStringLiteral("aiRecognizeButton"));
    auto cancel = page.findChild<QPushButton *>(QStringLiteral("aiCancelRecognition"));
    auto progress = page.findChild<QProgressBar *>(QStringLiteral("aiRecognitionProgress"));
    auto result = page.findChild<QPlainTextEdit *>(QStringLiteral("aiRecognitionResult"));
    auto status = page.findChild<QLabel *>(QStringLiteral("aiRecognitionStatus"));
    QVERIFY(start && cancel && progress && result && status);
    start->click();
    QCOMPARE(service.requests.count(), 1);
    const auto request = service.requests.first();
    QVERIFY(!request.contextId.isEmpty());
    QVERIFY(request.sourceRevision > 0);
    QCOMPARE(request.audioFile, m_audio);
    QCOMPARE(request.bpm, 142.5);
    QCOMPARE(request.offsetSeconds, 0.125);
    QCOMPARE(request.durationSeconds, 12.0);
    QVERIFY(page.isRecognizing());
    QVERIFY(!start->isEnabled());
    QVERIFY(cancel->isEnabled());
    emit service.progress(QStringLiteral("other-request"), 77);
    QCOMPARE(progress->maximum(), 0);
    emit service.progress(request.contextId, 42);
    QCOMPARE(progress->value(), 42);
    service.finish(request.contextId, QStringLiteral("第一段建议"));
    QVERIFY(!page.isRecognizing());
    QVERIFY(start->isEnabled());
    QVERIFY(result->toPlainText().contains(QStringLiteral("第一段建议")));
    QVERIFY(result->toPlainText().contains(QStringLiteral("1.250")));
    QVERIFY(result->toPlainText().contains(QStringLiteral("强拍")));
    QVERIFY(result->toPlainText().contains(QStringLiteral("90%")));
    QCOMPARE(status->property("role").toString(), QStringLiteral("success"));
    emit service.requestFailed(request.contextId, QStringLiteral("迟到的失败"));
    QCOMPARE(status->property("role").toString(), QStringLiteral("success"));
}

void AiRecognitionTest::cancelledAndStaleResultsAreIgnored() {
    FakeRecognitionService service;
    lmsc::AiRecognitionPage page;
    page.setService(&service);
    page.setContext(m_audio, QStringLiteral("歌曲 A"), 120.0, 0.0, 12.0, false);
    auto start = page.findChild<QPushButton *>(QStringLiteral("aiRecognizeButton"));
    auto cancel = page.findChild<QPushButton *>(QStringLiteral("aiCancelRecognition"));
    auto result = page.findChild<QPlainTextEdit *>(QStringLiteral("aiRecognitionResult"));
    auto status = page.findChild<QLabel *>(QStringLiteral("aiRecognitionStatus"));
    QVERIFY(start && cancel && result && status);
    start->click();
    const auto first = service.requests.last();
    cancel->click();
    QVERIFY(!page.isRecognizing());
    QCOMPARE(service.cancelledIds, QStringList{first.contextId});
    service.finish(first.contextId, QStringLiteral("已取消的建议"));
    emit service.requestFailed(first.contextId, QStringLiteral("已取消的错误"));
    QVERIFY(result->toPlainText().isEmpty());
    QVERIFY(status->text().contains(QStringLiteral("取消")));

    start->click();
    const auto second = service.requests.last();
    QVERIFY(second.contextId != first.contextId);
    page.setContext(m_audio, QStringLiteral("歌曲 B"), 150.0, 0.25, 12.0, false);
    QVERIFY(!page.isRecognizing());
    QCOMPARE(service.cancelledIds.last(), second.contextId);
    service.finish(second.contextId, QStringLiteral("旧歌曲的建议"));
    QVERIFY(result->toPlainText().isEmpty());
    start->click();
    const auto current = service.requests.last();
    QVERIFY(current.sourceRevision > second.sourceRevision);
    QCOMPARE(current.bpm, 150.0);
    service.finish(second.contextId, QStringLiteral("另一条旧结果"));
    QVERIFY(page.isRecognizing());
    QVERIFY(result->toPlainText().isEmpty());
    service.finish(current.contextId, QStringLiteral("当前歌曲的建议"));
    QVERIFY(result->toPlainText().contains(QStringLiteral("当前歌曲的建议")));
}

void AiRecognitionTest::replacementAndDestroyedBackendAreSafe() {
    FakeRecognitionService original;
    lmsc::AiRecognitionPage page;
    page.setService(&original);
    page.setContext(m_audio, QStringLiteral("歌曲"), 120.0, 0.0, 12.0, false);
    auto start = page.findChild<QPushButton *>(QStringLiteral("aiRecognizeButton"));
    auto result = page.findChild<QPlainTextEdit *>(QStringLiteral("aiRecognitionResult"));
    auto status = page.findChild<QLabel *>(QStringLiteral("aiRecognitionStatus"));
    QVERIFY(start && result && status);
    start->click();
    const auto oldRequest = original.requests.last();
    auto replacement = new FakeRecognitionService;
    page.setService(replacement);
    QCOMPARE(original.cancelledIds.last(), oldRequest.contextId);
    QVERIFY(!page.isRecognizing());
    original.finish(oldRequest.contextId, QStringLiteral("旧服务结果"));
    QVERIFY(result->toPlainText().isEmpty());
    start->click();
    QVERIFY(page.isRecognizing());
    QVERIFY(!replacement->parent());
    delete replacement;
    QVERIFY(!page.isRecognizing());
    QVERIFY(!start->isEnabled());
    QVERIFY(status->text().contains(QStringLiteral("断开")));
    QVERIFY(result->toPlainText().isEmpty());
    page.setService(&original);
    QVERIFY(start->isEnabled());
}

void AiRecognitionTest::busyContextAndAvailabilityCancelPendingWork() {
    FakeRecognitionService service;
    lmsc::AiRecognitionPage page;
    page.setService(&service);
    page.setContext(m_audio, QStringLiteral("歌曲"), 120.0, 0.0, 12.0, false);
    auto start = page.findChild<QPushButton *>(QStringLiteral("aiRecognizeButton"));
    auto result = page.findChild<QPlainTextEdit *>(QStringLiteral("aiRecognitionResult"));
    QVERIFY(start && result);
    start->click();
    const auto first = service.requests.last();
    page.setContext(m_audio, QStringLiteral("歌曲"), 120.0, 0.0, 12.0, true);
    QCOMPARE(service.cancelledIds.last(), first.contextId);
    QVERIFY(!start->isEnabled());
    service.finish(first.contextId, QStringLiteral("忙碌期间的旧结果"));
    QVERIFY(result->toPlainText().isEmpty());
    page.setContext(m_audio, QStringLiteral("歌曲"), 120.0, 0.0, 12.0, false);
    start->click();
    const auto second = service.requests.last();
    service.available = false;
    emit service.availabilityChanged();
    QVERIFY(!page.isRecognizing());
    QVERIFY(!start->isEnabled());
    QCOMPARE(service.cancelledIds.last(), second.contextId);
    service.finish(second.contextId, QStringLiteral("断开后的旧结果"));
    QVERIFY(result->toPlainText().isEmpty());
}

QTEST_MAIN(AiRecognitionTest)
#include "AiRecognitionTest.moc"
