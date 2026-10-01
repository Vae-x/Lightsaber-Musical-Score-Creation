#include "gui/MainWindow.h"
#include "gui/EditorViews.h"
#include "core/AudioService.h"
#include "core/ProjectStore.h"
#include <QtTest>
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMouseEvent>
#include <QProcess>
#include <QPushButton>
#include <QRegularExpression>
#include <QSpinBox>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QTimer>
#include <cmath>

namespace {
template<typename T> T *field(QWidget &root, const QString &labelText) {
    for (auto *form : root.findChildren<QFormLayout *>())
        for (int row = 0; row < form->rowCount(); ++row) {
            auto *labelItem = form->itemAt(row, QFormLayout::LabelRole);
            auto *fieldItem = form->itemAt(row, QFormLayout::FieldRole);
            const auto *label = labelItem ? qobject_cast<QLabel *>(labelItem->widget()) : nullptr;
            if (label && label->text() == labelText && fieldItem)
                return qobject_cast<T *>(fieldItem->widget());
        }
    return nullptr;
}
QPushButton *button(QWidget &root, const QString &text) {
    for (auto *candidate : root.findChildren<QPushButton *>())
        if (candidate->text() == text)
            return candidate;
    return nullptr;
}
bool hasText(QWidget &root, const QString &text) {
    for (auto *label : root.findChildren<QLabel *>())
        if (label->text().contains(text))
            return true;
    return false;
}
int objectCount(QWidget &root) {
    static const QRegularExpression expression(QStringLiteral("^(\\d+) 个物件"));
    for (auto *label : root.findChildren<QLabel *>()) {
        const auto match = expression.match(label->text());
        if (match.hasMatch())
            return match.captured(1).toInt();
    }
    return -1;
}
QPoint gridCell(GridEditor &grid, int x, int y) {
    const double unit = std::min((grid.width() - 38) / 4.0, (grid.height() - 78) / 3.0);
    const double left = (grid.width() - unit * 4) / 2;
    const double top = 39 + (grid.height() - 78 - unit * 3) / 2;
    return QPoint(qRound(left + (x + 0.5) * unit), qRound(top + (2.5 - y) * unit));
}
}

class MainWindowTest : public QObject {
    Q_OBJECT
private slots:
    void initTestCase();
    void clickPlaceApplyUndoAndDifficulty();
    void protectedSelectionRejectsEntireDrag();
    void importMp3AndCropThroughDialogs_data();
    void importMp3AndCropThroughDialogs();
    void importDryHands();
    void trackFramebufferUsesLoadedObjects();
private:
    QTemporaryDir m_temp;
    QString m_song;
};

void MainWindowTest::initTestCase() {
    QVERIFY(m_temp.isValid());
    m_song = QDir(m_temp.path()).filePath("SyntheticSong");
    QVERIFY(QDir().mkpath(m_song));
    QProcess converter;
    converter.start(QString::fromUtf8(LMSC_AUDIO_TOOLS_DIR) + "/bin/ffmpeg.exe",
                    {"-v", "error", "-y", "-i",
                     QString::fromUtf8(LMSC_AUDIO_FIXTURES_DIR) + "/clicks.mp3",
                     "-c:a", "libvorbis", QDir(m_song).filePath("song.ogg")});
    QVERIFY(converter.waitForFinished(30000));
    QCOMPARE(converter.exitCode(), 0);
    QJsonArray difficulties{
        QJsonObject{{"_difficulty", "Expert"}, {"_difficultyRank", 7}, {"_beatmapFilename", "Expert.dat"}},
        QJsonObject{{"_difficulty", "ExpertPlus"}, {"_difficultyRank", 9}, {"_beatmapFilename", "ExpertPlus.dat"}}};
    QJsonObject info{{"_version", "2.0.0"}, {"_songName", "GUI Fixture"},
                     {"_beatsPerMinute", 120}, {"_songTimeOffset", 0},
                     {"_songFilename", "song.ogg"}, {"_coverImageFilename", ""},
                     {"_difficultyBeatmapSets", QJsonArray{QJsonObject{
                        {"_beatmapCharacteristicName", "Standard"}, {"_difficultyBeatmaps", difficulties}}}}};
    QJsonObject expert{{"version", "3.3.0"},
        {"colorNotes", QJsonArray{
            QJsonObject{{"b", 4}, {"x", 0}, {"y", 0}, {"c", 0}, {"d", 1}},
            QJsonObject{{"b", 6}, {"x", 2}, {"y", 2}, {"c", 1}, {"d", 2}}}},
        {"bombNotes", QJsonArray{QJsonObject{{"b", 8}, {"x", 3}, {"y", 0}}}},
        {"sliders", QJsonArray{QJsonObject{
            {"b", 4}, {"c", 0}, {"x", 0}, {"y", 0}, {"d", 1}, {"mu", 1},
            {"tb", 5}, {"tx", 1}, {"ty", 1}, {"tc", 1}, {"tmu", 1}, {"m", 0}}}},
        {"obstacles", QJsonArray{}}};
    QJsonObject expertPlus{{"version", "3.3.0"},
        {"colorNotes", QJsonArray{QJsonObject{{"b", 10}, {"x", 1}, {"y", 1}, {"c", 1}, {"d", 0}}}},
        {"bombNotes", QJsonArray{}}, {"obstacles", QJsonArray{}}};
    QString error;
    QVERIFY2(lmsc::ProjectStore::writeJson(QDir(m_song).filePath("Info.dat"), info, &error), qPrintable(error));
    QVERIFY2(lmsc::ProjectStore::writeJson(QDir(m_song).filePath("Expert.dat"), expert, &error), qPrintable(error));
    QVERIFY2(lmsc::ProjectStore::writeJson(QDir(m_song).filePath("ExpertPlus.dat"), expertPlus, &error), qPrintable(error));
}

void MainWindowTest::clickPlaceApplyUndoAndDifficulty() {
    MainWindow window; window.setTestMode(true); window.show();
    QSignalSpy ready(&window, &MainWindow::documentReady);
    QSignalSpy failed(&window, &MainWindow::loadFailed);
    window.openPath(m_song);
    QTRY_VERIFY_WITH_TIMEOUT(ready.count() == 1 || failed.count() > 0, 20000);
    QCOMPARE(failed.count(), 0);
    QTRY_VERIFY_WITH_TIMEOUT(window.isAudioReady(), 20000);
    QCOMPARE(objectCount(window), 3);
    auto *grid = window.findChild<GridEditor *>();
    QVERIFY(grid);
    QApplication::setActiveWindow(&window);
    QTest::mouseClick(grid, Qt::LeftButton, Qt::NoModifier, gridCell(*grid, 1, 1));
    QCOMPARE(objectCount(window), 4);
    QTest::mouseClick(grid, Qt::LeftButton, Qt::NoModifier, gridCell(*grid, 1, 1));
    auto *beat = field<QDoubleSpinBox>(window, QStringLiteral("拍位置"));
    auto *x = field<QSpinBox>(window, QStringLiteral("列 (0–3)"));
    auto *color = field<QComboBox>(window, QStringLiteral("颜色"));
    // The first color combo belongs to placement, so locate the properties group.
    auto *apply = button(window, QStringLiteral("应用属性"));
    QVERIFY(beat && x && apply);
    color = field<QComboBox>(*apply->parentWidget(), QStringLiteral("颜色"));
    auto *direction = field<QComboBox>(*apply->parentWidget(), QStringLiteral("方向"));
    QVERIFY(color && direction);
    QVERIFY(apply->isEnabled());
    beat->setValue(0.25); x->setValue(2); color->setCurrentIndex(1); direction->setCurrentIndex(3);
    QTest::mouseClick(apply, Qt::LeftButton);
    QCOMPARE(objectCount(window), 4);
    QCOMPARE(beat->value(), 0.25);
    QCOMPARE(x->value(), 2);
    QCOMPARE(color->currentIndex(), 1);
    window.findChild<AudioService *>()->seek(0.125);
    QTest::mouseClick(grid, Qt::LeftButton, Qt::NoModifier, gridCell(*grid, 2, 1));
    QVERIFY(apply->isEnabled());
    grid->setFocus();
    QTest::keyClick(grid, Qt::Key_Delete);
    QCOMPARE(objectCount(window), 3);
    QTest::keyClick(grid, Qt::Key_Z, Qt::ControlModifier);
    QCOMPARE(objectCount(window), 4);
    QTest::keyClick(grid, Qt::Key_Y, Qt::ControlModifier);
    QCOMPARE(objectCount(window), 3);
    QTest::keyClick(grid, Qt::Key_Z, Qt::ControlModifier);
    QCOMPARE(objectCount(window), 4);
    auto *difficulties = window.findChild<QListWidget *>();
    QVERIFY(difficulties);
    QCOMPARE(difficulties->count(), 2);
    difficulties->setCurrentRow(1);
    QCOMPARE(objectCount(window), 1);
    QVERIFY(!apply->isEnabled());
    difficulties->setCurrentRow(0);
    QCOMPARE(objectCount(window), 4);
    window.close();
}

void MainWindowTest::protectedSelectionRejectsEntireDrag() {
    MainWindow window; window.setTestMode(true); window.show();
    QSignalSpy ready(&window, &MainWindow::documentReady);
    window.openPath(m_song);
    QTRY_VERIFY_WITH_TIMEOUT(ready.count() == 1, 20000);
    QTRY_VERIFY_WITH_TIMEOUT(window.isAudioReady(), 20000);
    auto *grid = window.findChild<GridEditor *>();
    auto *timeline = window.findChild<TimelineView *>();
    auto *apply = button(window, QStringLiteral("应用属性"));
    QVERIFY(grid && timeline && apply);
    auto *audio = window.findChild<AudioService *>();
    audio->seek(2.0);
    QTest::mouseClick(grid, Qt::LeftButton, Qt::NoModifier, gridCell(*grid, 0, 0));
    QVERIFY(!apply->isEnabled());
    QVERIFY(hasText(window, QStringLiteral("已保护")));
    timeline->setFocus(); QApplication::setActiveWindow(&window);
    QTest::keyClick(timeline, Qt::Key_A, Qt::ControlModifier);
    const double row = (timeline->height() - 20 - 88) / 4.0;
    const QPoint head(232, qRound(88 + row * 0.63));
    QTest::mousePress(timeline, Qt::LeftButton, Qt::NoModifier, head);
    QMouseEvent move(QEvent::MouseMove, head + QPoint(23, 0), Qt::NoButton,
                     Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(timeline, &move);
    QTest::mouseRelease(timeline, Qt::LeftButton, Qt::NoModifier, head + QPoint(23, 0));
    QCOMPARE(objectCount(window), 3);
    QVERIFY(window.statusBar()->currentMessage().contains(QStringLiteral("保护")));
    QTest::mouseClick(grid, Qt::LeftButton, Qt::NoModifier, gridCell(*grid, 0, 0));
    QVERIFY(!apply->isEnabled());
    auto *beat = field<QDoubleSpinBox>(window, QStringLiteral("拍位置"));
    QCOMPARE(beat->value(), 4.0);
    window.close();
}

void MainWindowTest::importMp3AndCropThroughDialogs_data() {
    QTest::addColumn<QString>("mediaName");
    QTest::addColumn<int>("streamIndex");
    QTest::addColumn<bool>("previewBeforeConvert");
    QTest::newRow("MP3") << QStringLiteral("clicks.mp3") << 0 << false;
    QTest::newRow("MP4-second-audio") << QStringLiteral("two-tracks.mp4") << 2 << true;
}

void MainWindowTest::importMp3AndCropThroughDialogs() {
    QFETCH(QString, mediaName);
    QFETCH(int, streamIndex);
    QFETCH(bool, previewBeforeConvert);
    const QString inputMedia = QString::fromUtf8(LMSC_AUDIO_FIXTURES_DIR) + "/" + mediaName;
    MainWindow window; window.setTestMode(true); window.show();
    QSignalSpy ready(&window, &MainWindow::documentReady);
    int phase = 0;
    bool configuredCrop = false;
    bool convertedDuringPreview = false;
    QString lastDialog;
    QTimer dialogDriver;
    connect(&dialogDriver, &QTimer::timeout, &window, [&] {
        QWidget *modal = QApplication::activeModalWidget();
        if (!modal)
            for (auto *candidate : QApplication::topLevelWidgets())
                if (candidate->isVisible() && qobject_cast<QDialog *>(candidate)) { modal = candidate; break; }
        if (modal && modal->windowTitle() != lastDialog) {
            lastDialog = modal->windowTitle();
            qInfo() << "Dialog driver phase" << phase << lastDialog;
        }
        if (phase == 0) {
            auto *file = qobject_cast<QFileDialog *>(modal);
            if (!file) return;
            const QString media = inputMedia;
            file->setDirectory(QFileInfo(media).absolutePath());
            file->selectFile(QFileInfo(media).fileName());
            phase = 1;
            QTimer::singleShot(250, file, [file, media] {
                if (auto *name = file->findChild<QLineEdit *>(QStringLiteral("fileNameEdit"))) {
                    name->setText(media);
                }
                if (auto *buttons = file->findChild<QDialogButtonBox *>()) {
                    if (auto *open = buttons->button(QDialogButtonBox::Open)) {
                        QTest::mouseClick(open, Qt::LeftButton);
                        return;
                    }
                }
                QMetaObject::invokeMethod(file, "accept", Qt::QueuedConnection);
            });
        } else if (phase == 1) {
            auto *dialog = qobject_cast<QDialog *>(modal);
            if (!dialog || !dialog->windowTitle().contains(QStringLiteral("音轨与裁剪"))) return;
            auto *title = field<QLineEdit>(*dialog, QStringLiteral("歌曲名"));
            auto *start = field<QDoubleSpinBox>(*dialog, QStringLiteral("裁剪起点"));
            auto *end = field<QDoubleSpinBox>(*dialog, QStringLiteral("裁剪终点"));
            auto *track = field<QComboBox>(*dialog, QStringLiteral("提取音轨"));
            auto *buttons = dialog->findChild<QDialogButtonBox *>();
            if (!(title && start && end && track && buttons)) { dialog->reject(); phase = 2; return; }
            const int trackChoice = track->findData(streamIndex);
            if (trackChoice < 0) { dialog->reject(); phase = 2; return; }
            track->setCurrentIndex(trackChoice);
            title->setText(QStringLiteral("GUI Crop")); start->setValue(1.0); end->setValue(4.0);
            configuredCrop = true; phase = 2; dialogDriver.stop();
            if (previewBeforeConvert) {
                auto *preview = button(*dialog, QStringLiteral("试听所选片段"));
                if (!preview) { configuredCrop = false; dialog->reject(); return; }
                auto *audio = window.findChild<AudioService *>();
                auto *whenBusy = new QTimer(dialog);
                whenBusy->setInterval(1);
                connect(whenBusy, &QTimer::timeout, dialog, [&, whenBusy, buttons, audio] {
                    if (!audio->isBusy()) return;
                    whenBusy->stop(); convertedDuringPreview = true;
                    QTest::mouseClick(buttons->button(QDialogButtonBox::Ok), Qt::LeftButton);
                });
                QTest::mouseClick(preview, Qt::LeftButton);
                whenBusy->start();
            } else {
                QTest::mouseClick(buttons->button(QDialogButtonBox::Ok), Qt::LeftButton);
            }
        }
    });
    dialogDriver.start(20);
    // Fail safely instead of leaving a modal chooser open after a regression.
    QTimer::singleShot(15000, &window, [&] {
        if (phase < 2) {
            dialogDriver.stop();
            if (auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget())) dialog->reject();
        }
    });
    auto *newSong = button(window, QStringLiteral("新歌 · MP3 / MP4"));
    QVERIFY(newSong);
    QTest::mouseClick(newSong, Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(ready.count() == 1 || phase == 2, 20000);
    QVERIFY(configuredCrop);
    QTRY_VERIFY_WITH_TIMEOUT(ready.count() == 1, 20000);
    QTRY_VERIFY_WITH_TIMEOUT(window.isAudioReady(), 20000);
    QVERIFY(window.windowTitle().contains(QStringLiteral("GUI Crop")));
    QCOMPARE(window.findChild<QListWidget *>()->count(), 1);
    QCOMPARE(objectCount(window), 0);
    QVERIFY(std::abs(window.findChild<AudioService *>()->duration() - 3.0) < 0.06);
    QTRY_VERIFY_WITH_TIMEOUT(hasText(window, QStringLiteral("建议")), 20000);
    if (previewBeforeConvert) QVERIFY(convertedDuringPreview);
    const QString output = QDir(m_temp.path()).filePath(mediaName + "-roundtrip");
    QString error;
    QVERIFY2(window.runEditorCheck(output, &error), qPrintable(error));
    lmsc::BeatmapDocument reopened;
    QVERIFY2(reopened.loadProject(QDir(output).filePath("smoke.lmsc"), &error), qPrintable(error));
    const auto source = reopened.importSource();
    QVERIFY(source.isAvailable());
    QCOMPARE(source.streamIndex, streamIndex);
    QCOMPARE(source.startSeconds, 1.0);
    QCOMPARE(source.endSeconds, 4.0);
    QFile original(inputMedia), retained(source.path);
    QVERIFY(original.open(QIODevice::ReadOnly) && retained.open(QIODevice::ReadOnly));
    QCOMPARE(QCryptographicHash::hash(retained.readAll(), QCryptographicHash::Sha256),
             QCryptographicHash::hash(original.readAll(), QCryptographicHash::Sha256));
    const auto exported = lmsc::ProjectStore::files(QDir(output).filePath("export"), &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    for (const auto &path : exported)
        QVERIFY(!path.endsWith(".mp3", Qt::CaseInsensitive) && !path.endsWith(".mp4", Qt::CaseInsensitive));
    window.close();
}

void MainWindowTest::importDryHands() {
    const QString sample = QString::fromUtf8(LMSC_REPOSITORY_DIR) + "/samples/beatmaps/2369d (Dry Hands - Darkrealm7).zip";
    if (!QFileInfo::exists(sample)) QSKIP("Local Dry Hands reference ZIP is unavailable");
    MainWindow window; window.setTestMode(true); window.show();
    QSignalSpy ready(&window, &MainWindow::documentReady);
    QSignalSpy failed(&window, &MainWindow::loadFailed);
    window.openPath(sample);
    QTRY_VERIFY_WITH_TIMEOUT(ready.count() == 1 || failed.count() > 0, 30000);
    QCOMPARE(failed.count(), 0);
    QTRY_VERIFY_WITH_TIMEOUT(window.isAudioReady(), 20000);
    QVERIFY(window.windowTitle().contains(QStringLiteral("Dry Hands")));
    QVERIFY(window.findChild<QListWidget *>()->count() > 0);
    const int before = objectCount(window);
    QVERIFY(before > 0);
    auto *grid = window.findChild<GridEditor *>();
    QTest::mouseClick(grid, Qt::LeftButton, Qt::NoModifier, gridCell(*grid, 3, 2));
    QCOMPARE(objectCount(window), before + 1);
    window.close();
}

void MainWindowTest::trackFramebufferUsesLoadedObjects() {
    MainWindow window; window.setTestMode(true); window.show();
    QSignalSpy ready(&window, &MainWindow::documentReady);
    window.openPath(m_song);
    QTRY_VERIFY_WITH_TIMEOUT(ready.count() == 1, 20000);
    QTRY_VERIFY_WITH_TIMEOUT(window.isAudioReady(), 20000);
    auto *track = window.findChild<TrackView *>();
    QVERIFY(track);
    const QImage frame = track->grabFramebuffer().convertToFormat(QImage::Format_RGB32);
    QVERIFY(!frame.isNull());
    int redPixels = 0, bluePixels = 0;
    for (int y = 0; y < frame.height(); ++y)
        for (int x = 0; x < frame.width(); ++x) {
            const QColor pixel(frame.pixel(x, y));
            if (pixel.red() > 100 && pixel.red() > pixel.green() * 1.4 && pixel.red() > pixel.blue() * 1.2) ++redPixels;
            if (pixel.blue() > 180 && pixel.blue() > pixel.red() * 1.5) ++bluePixels;
        }
    frame.save(QCoreApplication::applicationDirPath() + "/track-mainwindow-test.png");
    QVERIFY2(redPixels > 20, "Loaded red note is absent from the OpenGL framebuffer");
    QVERIFY2(bluePixels > 20, "Loaded blue note is absent from the OpenGL framebuffer");
    window.close();
}

int main(int argc, char **argv) {
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication application(argc, argv);
    application.setFont(QFont(QStringLiteral("Microsoft YaHei UI"), 9));
    MainWindowTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "MainWindowTest.moc"
