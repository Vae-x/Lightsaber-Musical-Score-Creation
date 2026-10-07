#include "gui/MainWindow.h"
#include "gui/AiRecognitionPage.h"
#include "gui/GenerationPreviewDialog.h"
#include "gui/EditorRefinementPanel.h"
#include "gui/EditorSessionController.h"
#include "gui/TaskProgressView.h"
#include "gui/EditorViews.h"
#include "gui/NavigationSidebar.h"
#include "gui/ThemeManager.h"
#include "gui/SettingsPanel.h"
#include "gui/SongImportDialog.h"
#include "gui/SongExportDialog.h"
#include "core/AiTextTransport.h"
#include "core/LocalAiGenerationService.h"
#include "core/InfernoSaberGenerationService.h"
#include "core/AiRefinementService.h"
#include "core/AppInfo.h"
#include "core/AppSettings.h"
#include "core/AudioService.h"
#include "core/MtpImportService.h"
#include "core/MtpDeleteService.h"
#include "core/ProjectStore.h"
#include <QtTest>
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDialogButtonBox>
#include <QDesktopServices>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFile>
#include <QFileSystemModel>
#include <QFormLayout>
#include <QIcon>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QMenu>
#include <QMouseEvent>
#include <QProcess>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QPointer>
#include <QSpinBox>
#include <QRegularExpression>
#include <QSpinBox>
#include <QStatusBar>
#include <QStackedWidget>
#include <QTemporaryDir>
#include <QTextBrowser>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QTreeWidget>
#include <QTreeView>
#include <QTreeWidgetItemIterator>
#include <QTabWidget>
#include <QMimeData>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QSaveFile>
#include <QProgressDialog>
#include <QScrollArea>
#include <cmath>

class HomepageUrlReceiver : public QObject {
    Q_OBJECT
public:
    QUrl openedUrl;
    int openedCount = 0;
public slots:
    void openUrl(const QUrl &url) {
        openedUrl = url;
        ++openedCount;
    }
};

namespace {
class MainRecognitionService final : public lmsc::AiRecognitionService {
public:
    QVector<lmsc::AiRecognitionRequest> requests;
    QStringList cancelledIds;
    bool isAvailable() const override { return true; }
    void analyze(const lmsc::AiRecognitionRequest &request) override { requests.append(request); }
    void cancel(const QString &contextId) override {
        cancelledIds.append(contextId);
        emit cancelled(contextId);
    }
};
class MainGenerationService final : public lmsc::AiGenerationService {
public:
    QVector<lmsc::GenerationRequest> requests;
    QStringList cancelledIds;
    bool isAvailable() const override { return true; }
    void generate(const lmsc::GenerationRequest &request) override { requests.append(request); }
    void cancel(const QString &jobId) override { cancelledIds.append(jobId); emit cancelled(jobId); }
};
class MainRefinementService final : public lmsc::AiRefinementService {
public:
    MainRefinementService() : AiRefinementService(nullptr) {}
    QVector<lmsc::RefinementRequest> requests;
    QStringList cancelledJobs, discardedJobs, resumedJobs;
    lmsc::AiGenerationService::Status current;
    bool publishPartialOnCancel = false;
    bool isAvailable() const override { return true; }
    lmsc::AiGenerationService::Status status() const override { return current; }
    void refine(const lmsc::RefinementRequest &request) override { requests.append(request); current={}; current.jobId=request.generation.jobId; current.state=lmsc::AiGenerationService::Status::Running; }
    void cancel(const QString &job) override {
        cancelledJobs.append(job); current.state=lmsc::AiGenerationService::Status::Paused; current.resumable=true;
        if (publishPartialOnCancel && !requests.isEmpty()) emit candidateReady(resultFor(requests.last()));
        emit cancelled(job);
    }
    void discard(const QString &job) override { discardedJobs.append(job); current={}; }
    void resume(const QString &job) override { resumedJobs.append(job); current.state=lmsc::AiGenerationService::Status::Running; current.resumable=false; }
    lmsc::RefinementResult resultFor(const lmsc::RefinementRequest &request) const {
        lmsc::RefinementResult result; result.source=request; result.candidate.source=request.generation;
        result.candidate.objects=request.baseline;
        if (!result.candidate.objects.isEmpty()) result.candidate.objects.first().y=2;
        result.patch=lmsc::refinementDifference(request.baseline,result.candidate.objects);
        result.stats=lmsc::refinementStatistics(result.patch,request.baseline);
        result.processedSegments=QStringList{QStringLiteral("s1")};
        result.remainingSegments=QStringList{QStringLiteral("s2")}; result.resumable=true;
        result.processedRanges=QStringList{QStringLiteral("p1 · 0.000–4.000 秒")};
        return result;
    }
    void finish(const lmsc::RefinementRequest &request) {
        current.state=lmsc::AiGenerationService::Status::Completed; current.resumable=true;
        emit candidateReady(resultFor(request));
    }
};

class ScopedHttpsUrlHandler {
public:
    explicit ScopedHttpsUrlHandler(QObject *receiver) {
        QDesktopServices::setUrlHandler(QStringLiteral("https"), receiver, "openUrl");
    }
    ~ScopedHttpsUrlHandler() { QDesktopServices::unsetUrlHandler(QStringLiteral("https")); }
};
class ScopedFileUrlHandler {
public:
    explicit ScopedFileUrlHandler(QObject *receiver) { QDesktopServices::setUrlHandler(QStringLiteral("file"), receiver, "openUrl"); }
    ~ScopedFileUrlHandler() { QDesktopServices::unsetUrlHandler(QStringLiteral("file")); }
};

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
QAction *namedAction(QWidget &root, const QString &name) {
    for (auto action : root.findChildren<QAction *>()) if (action->text() == name) return action;
    return nullptr;
}
lmsc::EditorSessionController *session(MainWindow &window) {
    return window.findChild<lmsc::EditorSessionController *>(QStringLiteral("editorSessionController"));
}
lmsc::GenerationDraft syntheticDraft(const lmsc::GenerationRequest &source, int count = 3) {
    lmsc::GenerationDraft draft;
    draft.source = source;
    draft.summary = QStringLiteral("编辑页内暂存的合成测试候选");
    for (int i = 0; i < count; ++i) {
        lmsc::BeatObject note;
        note.beat = 2 + i * 2; note.x = i % 2 ? 2 : 0; note.y = 1;
        note.color = i % 2; note.direction = i % 2 ? 0 : 1;
        draft.objects.append(note);
    }
    draft.metrics.directional = count;
    return draft;
}
}

class MainWindowTest : public QObject {
    Q_OBJECT
private slots:
    void initTestCase();
    void chineseBrandIconAndAboutLicense();
    void aboutHomepageOpensProject();
    void settingsEntryAndThemeCancel();
    void navigationPreservesEditorAndSettingsDraft();
    void languageModelAccountSubmenu();
    void aiRecognitionEntry();
    void aiSuggestionsPreserveLoadedDocument();
    void aiGenerationPreviewAndAtomicApply();
    void localGenerationOfflinePreviewAndApply();
    void infernoGenerationUsesEditorDraftAndPreservesOfflineTask();
    void realInfernoGenerationEditorCheck();
    void existingNewSongRefinementIsAtomicAndReusable();
    void manualEditCancelsRefinementAndRejectsLateResult();
    void savedDraftReopensForManualEditingWithoutApplying();
    void draftSynchronizationFailureDoesNotMarkSavedOrReplaceDisk();
    void difficultySwitchPreservesIndependentDrafts();
    void viewingCachedDraftPreservesRefinementPartialAndManualChanges();
    void inlinePromptCannotReplaceOtherDifficulty();
    void outputLimitSettingsReachGenerationTransport();
    void clickPlaceApplyUndoAndDifficulty();
    void protectedSelectionRejectsEntireDrag();
    void importMp3AndCropThroughDialogs_data();
    void importMp3AndCropThroughDialogs();
    void importDryHands();
    void importHeadsetThroughDialog();
    void unifiedImportAndExportDialogs();
    void deleteHeadsetSongThroughDialog_data();
    void deleteHeadsetSongThroughDialog();
    void deviceExportThroughDialog_data();
    void deviceExportThroughDialog();
    void trackFramebufferUsesLoadedObjects();
    void captureWorkspace();
private:
    QString settingsFile() const {
        return m_temp.filePath(QString::fromLatin1(QTest::currentTestFunction()) + QStringLiteral(".json"));
    }
    QTemporaryDir m_temp;
    QString m_song;
};

void MainWindowTest::settingsEntryAndThemeCancel() {
    MainWindow window(nullptr, settingsFile());
    window.setTestMode(true);
    window.show();
    auto action = window.findChild<QAction *>(QStringLiteral("openSettingsAction"));
    auto nav = window.findChild<QListWidget *>(QStringLiteral("mainNavigation"));
    auto workspace = window.findChild<QStackedWidget *>(QStringLiteral("workspacePages"));
    auto panel = window.findChild<QWidget *>(QStringLiteral("settingsPanel"));
    auto pages = window.findChild<QStackedWidget *>(QStringLiteral("settingsPages"));
    auto combo = window.findChild<QComboBox *>(QStringLiteral("themeMode"));
    auto buttons = window.findChild<QDialogButtonBox *>(QStringLiteral("settingsButtons"));
    QVERIFY(action);
    QVERIFY(nav && workspace && panel && pages && combo && buttons);
    QCOMPARE(action->shortcut(), QKeySequence(QStringLiteral("Ctrl+,")));
    const QString originalTheme = lmsc::ThemeManager::mode();
    QApplication::setActiveWindow(&window);
    QTest::keyClick(&window, Qt::Key_Comma, Qt::ControlModifier);
    QTRY_COMPARE(nav->currentRow(), 2);
    QCOMPARE(workspace->currentWidget(), panel);
    QCOMPARE(pages->currentIndex(), 0);
    QVERIFY(!panel->isWindow());
    QVERIFY(!QApplication::activeModalWidget());
    for (auto *topLevel : QApplication::topLevelWidgets())
        QVERIFY(topLevel->objectName() != QStringLiteral("settingsDialog"));
    const QString preview = originalTheme == QStringLiteral("dark") ? QStringLiteral("light") : QStringLiteral("dark");
    combo->setCurrentIndex(combo->findData(preview));
    QCOMPARE(lmsc::ThemeManager::mode(), preview);
    QCOMPARE(buttons->button(QDialogButtonBox::Save)->text(), QStringLiteral("保存并返回"));
    QCOMPARE(buttons->button(QDialogButtonBox::Cancel)->text(), QStringLiteral("取消并返回"));
    QTest::mouseClick(buttons->button(QDialogButtonBox::Cancel), Qt::LeftButton);
    QCOMPARE(nav->currentRow(), 0);
    QCOMPARE(workspace->currentIndex(), 0);
    QCOMPARE(lmsc::ThemeManager::mode(), originalTheme);
    QVERIFY(!QFile::exists(settingsFile()));

    action->trigger();
    QCOMPARE(nav->currentRow(), 2);
    combo->setCurrentIndex(combo->findData(QStringLiteral("dark")));
    QTest::mouseClick(buttons->button(QDialogButtonBox::Apply), Qt::LeftButton);
    QCOMPARE(workspace->currentWidget(), panel);
    QCOMPARE(lmsc::AppSettings(settingsFile()).load().themeMode, QStringLiteral("dark"));
    combo->setCurrentIndex(combo->findData(QStringLiteral("light")));
    QTest::mouseClick(buttons->button(QDialogButtonBox::Cancel), Qt::LeftButton);
    QCOMPARE(workspace->currentIndex(), 0);
    QCOMPARE(lmsc::ThemeManager::mode(), QStringLiteral("dark"));
    action->trigger();
    QCOMPARE(combo->currentData().toString(), QStringLiteral("dark"));
    combo->setCurrentIndex(combo->findData(QStringLiteral("light")));
    QTest::mouseClick(buttons->button(QDialogButtonBox::Save), Qt::LeftButton);
    QCOMPARE(workspace->currentIndex(), 0);
    QCOMPARE(lmsc::AppSettings(settingsFile()).load().themeMode, QStringLiteral("light"));
    window.close();
}

void MainWindowTest::navigationPreservesEditorAndSettingsDraft() {
    MainWindow window(nullptr, settingsFile());
    window.setTestMode(true);
    window.show();
    auto sidebar = window.findChild<lmsc::NavigationSidebar *>(QStringLiteral("mainSidebar"));
    auto nav = window.findChild<QListWidget *>(QStringLiteral("mainNavigation"));
    auto workspace = window.findChild<QStackedWidget *>(QStringLiteral("workspacePages"));
    auto settingsPages = window.findChild<QStackedWidget *>(QStringLiteral("settingsPages"));
    auto toggle = sidebar ? sidebar->findChild<QToolButton *>(QStringLiteral("navigationToggle")) : nullptr;
    QVERIFY(sidebar && nav && workspace && settingsPages && toggle);
    const QStringList names{QStringLiteral("曲谱编辑"), QStringLiteral("生成与精修"),
        QStringLiteral("外观"), QStringLiteral("大语言模型"), QStringLiteral("账号授权"),
        QStringLiteral("网络"), QStringLiteral("关于")};
    QCOMPARE(nav->count(), names.size());
    QCOMPARE(workspace->count(), 2);
    QCOMPARE(workspace->widget(0)->objectName(), QStringLiteral("editorPage"));
    QCOMPARE(workspace->widget(1)->objectName(), QStringLiteral("settingsPanel"));
    QCOMPARE(nav->currentRow(), 0);
    QVERIFY(sidebar->isCollapsed());
    QCOMPARE(sidebar->width(), 64);
    for (int row = 0; row < nav->count(); ++row) {
        QCOMPARE(nav->item(row)->text(), names.at(row));
        QCOMPARE(nav->item(row)->toolTip(), names.at(row));
        QCOMPARE(nav->item(row)->data(Qt::AccessibleTextRole).toString(), names.at(row));
        QVERIFY(!nav->item(row)->icon().isNull());
    }

    QSignalSpy ready(&window, &MainWindow::documentReady);
    window.openPath(m_song);
    QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, 20000);
    QTRY_VERIFY_WITH_TIMEOUT(window.isAudioReady(), 20000);
    auto difficulties = window.findChild<QListWidget *>(QStringLiteral("difficultyList"));
    auto grid = window.findChild<GridEditor *>();
    auto audio = window.findChild<AudioService *>();
    auto apply = button(window, QStringLiteral("应用属性"));
    auto beat = field<QDoubleSpinBox>(window, QStringLiteral("拍位置"));
    QVERIFY(difficulties && grid && audio && apply && beat);
    difficulties->setCurrentRow(1);
    audio->seek(1.25);
    QApplication::setActiveWindow(&window);
    QTest::mouseClick(grid, Qt::LeftButton, Qt::NoModifier, gridCell(*grid, 1, 1));
    QCOMPARE(objectCount(window), 2);
    QTest::mouseClick(grid, Qt::LeftButton, Qt::NoModifier, gridCell(*grid, 1, 1));
    QVERIFY(apply->isEnabled());
    const double selectedBeat = beat->value();
    const double playbackPosition = audio->position();
    const QString title = window.windowTitle();
    QWidget *const editor = workspace->currentWidget();

    nav->setCurrentRow(3);
    QCOMPARE(workspace->currentIndex(), 1);
    QCOMPARE(settingsPages->currentIndex(), 1);
    auto model = window.findChild<QComboBox *>(QStringLiteral("aiModel"));
    auto key = window.findChild<QLineEdit *>(QStringLiteral("aiApiKey"));
    QVERIFY(model && key);
    // Keep the synthetic key out of the focus/auto-fetch path.
    nav->setFocus();
    model->setEditText(QStringLiteral("unsaved-workspace-model"));
    key->setText(QStringLiteral("test-only-workspace-key"));
    QTest::mouseClick(toggle, Qt::LeftButton);
    QVERIFY(!sidebar->isCollapsed());
    QCOMPARE(sidebar->width(), 208);
    QCOMPARE(nav->currentRow(), 3);
    QCOMPARE(model->currentText(), QStringLiteral("unsaved-workspace-model"));
    QCOMPARE(key->text(), QStringLiteral("test-only-workspace-key"));

    nav->setFocus();
    QTest::keyClick(nav, Qt::Key_Z, Qt::ControlModifier);
    QTest::keyClick(nav, Qt::Key_Delete);
    QTest::keyClick(nav, Qt::Key_Space);
    QCOMPARE(objectCount(window), 2);
    QCOMPARE(difficulties->currentRow(), 1);
    QCOMPARE(beat->value(), selectedBeat);
    QVERIFY(!audio->isPlaying());
    QCOMPARE(audio->position(), playbackPosition);
    QCOMPARE(window.windowTitle(), title);
    QVERIFY(!QApplication::activeModalWidget());

    nav->setCurrentRow(1);
    QCOMPARE(workspace->currentWidget(), editor);
    QCOMPARE(window.findChild<QTabWidget *>(QStringLiteral("editorToolsTabs"))->currentIndex(), 1);
    // Both navigation entries now share the same editor and its command route.
    // Settings still preserve document edits and their undo history.
    QCOMPARE(objectCount(window), 2);
    QVERIFY(!audio->isPlaying());
    QCOMPARE(audio->position(), playbackPosition);
    nav->setCurrentRow(3);
    QCOMPARE(model->currentText(), QStringLiteral("unsaved-workspace-model"));
    QCOMPARE(key->text(), QStringLiteral("test-only-workspace-key"));
    QTest::mouseClick(toggle, Qt::LeftButton);
    QVERIFY(sidebar->isCollapsed());
    QCOMPARE(sidebar->width(), 64);
    nav->setCurrentRow(0);
    QCOMPARE(workspace->currentWidget(), editor);
    QCOMPARE(difficulties->currentRow(), 1);
    QCOMPARE(objectCount(window), 2);
    QCOMPARE(beat->value(), selectedBeat);
    QVERIFY(apply->isEnabled());
    QCOMPARE(audio->position(), playbackPosition);
    QCOMPARE(window.windowTitle(), title);
    grid->setFocus();
    QTest::keyClick(grid, Qt::Key_Z, Qt::ControlModifier);
    QCOMPARE(objectCount(window), 1);
    window.close();
}

void MainWindowTest::languageModelAccountSubmenu() {
    MainWindow window(nullptr, settingsFile());
    window.setTestMode(true);
    window.show();
    auto sidebar = window.findChild<lmsc::NavigationSidebar *>(QStringLiteral("mainSidebar"));
    auto nav = window.findChild<QListWidget *>(QStringLiteral("mainNavigation"));
    auto settingsPages = window.findChild<QStackedWidget *>(QStringLiteral("settingsPages"));
    auto codexPath = window.findChild<QLineEdit *>(QStringLiteral("codexExecutable"));
    auto model = window.findChild<QComboBox *>(QStringLiteral("aiModel"));
    auto key = window.findChild<QLineEdit *>(QStringLiteral("aiApiKey"));
    QVERIFY(sidebar && nav && settingsPages && codexPath && model && key);
    codexPath->setText(m_temp.filePath(QStringLiteral("unavailable-submenu-codex.exe")));
    QCOMPARE(sidebar->parentRow(4), 3);
    QVERIFY(nav->item(4)->isHidden());
    sidebar->setCollapsed(false);
    QVERIFY(nav->item(4)->isHidden());
    nav->setCurrentRow(3);
    QCOMPARE(settingsPages->currentIndex(), 1);
    QVERIFY(sidebar->isSubmenuExpanded(3));
    QVERIFY(!nav->item(4)->isHidden());
    nav->setFocus();
    model->setEditText(QStringLiteral("unsaved-submenu-model"));
    key->setText(QStringLiteral("test-only-submenu-key"));
    QTest::keyClick(nav, Qt::Key_Left);
    QVERIFY(!sidebar->isSubmenuExpanded(3));
    QVERIFY(nav->item(4)->isHidden());
    QCOMPARE(nav->currentRow(), 3);
    QTest::keyClick(nav, Qt::Key_Down);
    QCOMPARE(nav->currentRow(), 5);
    nav->setCurrentRow(3);
    QTest::keyClick(nav, Qt::Key_Right);
    QTest::keyClick(nav, Qt::Key_Down);
    QCOMPARE(nav->currentRow(), 4);
    QCOMPARE(settingsPages->currentIndex(), 2);
    QTest::keyClick(nav, Qt::Key_Left);
    QCOMPARE(nav->currentRow(), 3);
    QVERIFY(nav->item(4)->isHidden());
    QCOMPARE(settingsPages->currentIndex(), 1);
    nav->setCurrentRow(4); // A direct settings link expands its parent first.
    QVERIFY(!nav->item(4)->isHidden());
    QCOMPARE(settingsPages->currentIndex(), 2);
    sidebar->setCollapsed(true);
    QCOMPARE(nav->currentRow(), 4);
    QVERIFY(nav->item(4)->isHidden());
    nav->setFocus();
    QTest::keyClick(nav, Qt::Key_Right);
    auto menu = sidebar->findChild<QMenu *>(QStringLiteral("navigationSubmenu"));
    QVERIFY(menu && menu->isVisible());
    QVERIFY(menu->actions().first()->isChecked());
    QTest::keyClick(menu, Qt::Key_Escape);
    QCOMPARE(nav->currentRow(), 4);
    nav->setCurrentRow(3);
    nav->setFocus();
    QTest::keyClick(nav, Qt::Key_Right);
    QVERIFY(menu && menu->isVisible());
    QCOMPARE(menu->actions().size(), 1);
    QCOMPARE(menu->actions().first()->text(), QStringLiteral("账号授权"));
    QTest::keyClick(menu, Qt::Key_Return);
    QCOMPARE(nav->currentRow(), 4);
    QCOMPARE(settingsPages->currentIndex(), 2);
    QVERIFY(!menu->isVisible());
    nav->setCurrentRow(3);
    QTest::mouseClick(nav->viewport(), Qt::LeftButton, Qt::NoModifier,
                     nav->visualItemRect(nav->item(3)).center());
    QVERIFY(menu->isVisible());
    menu->close();
    sidebar->setCollapsed(false);
    nav->setCurrentRow(3);
    QCOMPARE(model->currentText(), QStringLiteral("unsaved-submenu-model"));
    QCOMPARE(key->text(), QStringLiteral("test-only-submenu-key"));
    QVERIFY(!QFile::exists(settingsFile()));
    window.close();
}

void MainWindowTest::aiRecognitionEntry() {
    MainWindow window(nullptr, settingsFile());
    window.setTestMode(true);
    window.show();
    auto nav = window.findChild<QListWidget *>(QStringLiteral("mainNavigation"));
    auto workspace = window.findChild<QStackedWidget *>(QStringLiteral("workspacePages"));
    auto recognize = window.findChild<QPushButton *>(QStringLiteral("aiRecognizeButton"));
    auto configure = window.findChild<QPushButton *>(QStringLiteral("aiConfigureConnection"));
    auto status = window.findChild<QLabel *>(QStringLiteral("aiRecognitionStatus"));
    QVERIFY(nav && workspace && recognize && configure && status);
    nav->setCurrentRow(1);
    QCOMPARE(workspace->currentIndex(), 0);
    QVERIFY(recognize->isVisible());
    QVERIFY(!recognize->isEnabled());
    QVERIFY(!status->text().isEmpty());
    QSignalSpy ready(&window, &MainWindow::documentReady);
    window.openPath(m_song);
    QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, 20000);
    QTRY_VERIFY_WITH_TIMEOUT(window.isAudioReady(), 20000);
    nav->setCurrentRow(1);
    // The default offline analyzer works without saved model credentials.
    QVERIFY(recognize->isEnabled());
    window.findChild<lmsc::AiRecognitionPage *>()->setGenerationMode(lmsc::AiRecognitionPage::LanguageModel);
    QVERIFY(!recognize->isEnabled());
    QVERIFY(hasText(*workspace->currentWidget(), QStringLiteral("GUI Fixture")));
    QCOMPARE(objectCount(window), 3);
    QTest::mouseClick(configure, Qt::LeftButton);
    QCOMPARE(nav->currentRow(), 3);
    QCOMPARE(workspace->currentIndex(), 1);
    QCOMPARE(window.findChild<QStackedWidget *>(QStringLiteral("settingsPages"))->currentIndex(), 1);
    QVERIFY(!QApplication::activeModalWidget());
    window.close();
}

void MainWindowTest::aiSuggestionsPreserveLoadedDocument() {
    MainRecognitionService service;
    MainWindow window(nullptr, settingsFile());
    window.setTestMode(true);
    window.show();
    window.setAiRecognitionService(&service);
    QVERIFY(!service.parent());
    QSignalSpy ready(&window, &MainWindow::documentReady);
    window.openPath(m_song);
    QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, 20000);
    QTRY_VERIFY_WITH_TIMEOUT(window.isAudioReady(), 20000);
    auto grid = window.findChild<GridEditor *>();
    auto audio = window.findChild<AudioService *>();
    auto nav = window.findChild<QListWidget *>(QStringLiteral("mainNavigation"));
    auto difficulties = window.findChild<QListWidget *>(QStringLiteral("difficultyList"));
    auto beat = field<QDoubleSpinBox>(window, QStringLiteral("拍位置"));
    auto apply = button(window, QStringLiteral("应用属性"));
    auto start = window.findChild<QPushButton *>(QStringLiteral("aiRecognizeButton"));
    auto result = window.findChild<QPlainTextEdit *>(QStringLiteral("aiRecognitionResult"));
    QVERIFY(grid && audio && nav && difficulties && beat && apply && start && result);
    audio->seek(3.0);
    QTest::mouseClick(grid, Qt::LeftButton, Qt::NoModifier, gridCell(*grid, 2, 2));
    QVERIFY(apply->isEnabled());
    QCOMPARE(beat->value(), 6.0);
    const QString originalTitle = window.windowTitle();
    const double originalPosition = audio->position();
    nav->setCurrentRow(1);
    QVERIFY(start->isEnabled());
    QTest::mouseClick(start, Qt::LeftButton);
    QCOMPARE(service.requests.count(), 1);
    const auto request = service.requests.first();
    QVERIFY(QFileInfo::exists(request.audioFile));
    QCOMPARE(request.bpm, 120.0);
    QCOMPARE(request.offsetSeconds, 0.0);
    QVERIFY(request.durationSeconds > 0.0);
    QFile audioFile(request.audioFile);
    QVERIFY(audioFile.open(QIODevice::ReadOnly));
    const QByteArray audioHash = QCryptographicHash::hash(audioFile.readAll(), QCryptographicHash::Sha256);
    audioFile.close();
    emit service.recognitionFinished({request.contextId, QStringLiteral("仅供参考的 AI 节拍建议"),
                                      {{0.75, QStringLiteral("建议强拍"), 0.95}}});
    QVERIFY(result->toPlainText().contains(QStringLiteral("仅供参考的 AI 节拍建议")));
    QCOMPARE(objectCount(window), 3);
    QCOMPARE(difficulties->currentRow(), 0);
    QCOMPARE(beat->value(), 6.0);
    QCOMPARE(audio->position(), originalPosition);
    QCOMPARE(window.windowTitle(), originalTitle);
    QVERIFY(audioFile.open(QIODevice::ReadOnly));
    QCOMPARE(QCryptographicHash::hash(audioFile.readAll(), QCryptographicHash::Sha256), audioHash);
    nav->setCurrentRow(0);
    QVERIFY(apply->isEnabled());
    QAction *undo = nullptr;
    for (auto *action : window.findChildren<QAction *>())
        if (action->text() == QStringLiteral("撤销")) undo = action;
    QVERIFY(undo);
    QVERIFY(!undo->isEnabled());
    QCOMPARE(objectCount(window), 3);
    nav->setCurrentRow(1);
    QTest::mouseClick(start, Qt::LeftButton);
    QCOMPARE(service.requests.count(), 2);
    const auto pending = service.requests.last();
    nav->setCurrentRow(0);
    QVERIFY(!service.cancelledIds.contains(pending.contextId));
    emit service.recognitionFinished({pending.contextId, QStringLiteral("已离开页面的迟到结果"), {}});
    QCOMPARE(result->toPlainText(), QStringLiteral("已离开页面的迟到结果"));
    QCOMPARE(objectCount(window), 3);
    QVERIFY(apply->isEnabled());
    window.close();
}

void MainWindowTest::outputLimitSettingsReachGenerationTransport() {
    MainWindow window(nullptr,m_temp.filePath("output-policy-settings.json")); window.setTestMode(true);
    auto *transport=window.findChild<lmsc::ConfiguredAiTextTransport *>();
    auto *settings=window.findChild<lmsc::SettingsPanel *>();
    auto *tokens=window.findChild<QSpinBox *>("aiMaxOutputTokens"); QVERIFY(transport && settings && tokens);
    QSignalSpy changes(transport,&lmsc::AiTextTransport::configurationChanged);
    tokens->setValue(65536); QVERIFY(settings->applyPreferences()); QCOMPARE(changes.count(),1);
    QCOMPARE(transport->outputPolicy().maximumTokens,65536);
}
void MainWindowTest::localGenerationOfflinePreviewAndApply() {
    QString error;
    const QString song = QDir(m_song).filePath(QStringLiteral("song.ogg"));
    QFile source(song); QVERIFY(source.open(QIODevice::ReadOnly));
    const auto hash = QCryptographicHash::hash(source.readAll(), QCryptographicHash::Sha256); source.close();
    lmsc::BeatmapDocument original;
    QVERIFY2(original.createNew(song, QStringLiteral("本地制谱检查"), 120, .25, {}, &error), qPrintable(error));
    lmsc::BeatObject note; note.beat = 2; note.direction = 1;
    QVERIFY2(original.addObject(note, &error), qPrintable(error));
    const QString project = m_temp.filePath(QStringLiteral("local-generation-project/project.lmsc"));
    QVERIFY2(original.saveProject(project, &error), qPrintable(error));
    MainWindow window(nullptr, settingsFile()); window.setTestMode(true); window.show();
    QSignalSpy ready(&window, &MainWindow::documentReady); window.openPath(project);
    QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, 20000);
    QTRY_VERIFY_WITH_TIMEOUT(window.isAudioReady(), 20000);
    auto page = window.findChild<lmsc::AiRecognitionPage *>();
    auto service = window.findChild<lmsc::LocalAiGenerationService *>();
    auto nav = window.findChild<QListWidget *>(QStringLiteral("mainNavigation"));
    auto generate = window.findChild<QPushButton *>(QStringLiteral("aiGenerateButton"));
    auto target = window.findChild<QComboBox *>(QStringLiteral("aiGenerationDifficulty"));
    auto controller = session(window);
    QVERIFY(page && service && nav && generate && target && controller);
    QCOMPARE(page->generationMode(), lmsc::AiRecognitionPage::Hybrid);
    page->setGenerationMode(lmsc::AiRecognitionPage::LocalQuick);
    QVERIFY(!QFile::exists(settingsFile()));
    nav->setCurrentRow(1); target->setCurrentIndex(target->findData(QStringLiteral("Hard")));
    QVERIFY(generate->isEnabled());
    QSignalSpy drafts(service, &lmsc::AiGenerationService::draftReady);
    QSignalSpy failed(service, &lmsc::AiGenerationService::requestFailed);
    generate->click(); nav->setCurrentRow(0);
    QTRY_VERIFY_WITH_TIMEOUT(drafts.count() == 1 || failed.count() > 0, 30000);
    QVERIFY2(failed.isEmpty(), qPrintable(failed.isEmpty() ? QString() : failed.first().at(1).toString()));
    const auto draft = qvariant_cast<lmsc::GenerationDraft>(drafts.first().first());
    QVERIFY(draft.objects.size() > 1);
    QCOMPARE(controller->formalDocument()->objects().size(), 1);
    QCOMPARE(controller->workingDocument()->objects().size(), draft.objects.size());
    QCOMPARE(objectCount(window), draft.objects.size());
    QVERIFY(!window.findChild<lmsc::GenerationPreviewDialog *>());
    const QString captures = qEnvironmentVariable("LMSC_LOCAL_CAPTURE_DIRECTORY");
    if (!captures.isEmpty()) {
        QVERIFY(QDir().mkpath(captures)); const auto theme = lmsc::ThemeManager::mode();
        for (const QString &mode : QStringList{QStringLiteral("light"), QStringLiteral("dark")}) {
            lmsc::ThemeManager::apply(mode); window.resize(1200, 800); QTest::qWait(80);
            QVERIFY(window.grab().save(QDir(captures).filePath(QStringLiteral("local-generation-%1.png").arg(mode))));
        }
        lmsc::ThemeManager::apply(theme);
    }
    auto apply = window.findChild<QPushButton *>(QStringLiteral("editorCandidateApply"));
    QVERIFY(apply && apply->isEnabled()); apply->click();
    QCOMPARE(controller->formalDocument()->objects().size(), draft.objects.size());
    QCOMPARE(controller->formalDocument()->difficulties().size(), 2);
    auto versions = window.findChild<QComboBox *>(QStringLiteral("editorCandidateVersion"));
    versions->setCurrentIndex(versions->findData(lmsc::EditorRefinementPanel::Formal));
    auto undo = namedAction(window, QStringLiteral("撤销"));
    auto redo = namedAction(window, QStringLiteral("重做"));
    auto save = namedAction(window, QStringLiteral("保存工程"));
    QVERIFY(undo && redo && save);
    undo->trigger(); QCOMPARE(controller->formalDocument()->objects().size(), 1);
    QCOMPARE(controller->formalDocument()->difficulties().size(), 1);
    redo->trigger(); QCOMPARE(controller->formalDocument()->objects().size(), draft.objects.size());
    save->trigger(); QTRY_VERIFY_WITH_TIMEOUT(save->isEnabled(), 20000);
    lmsc::BeatmapDocument reopened; QVERIFY2(reopened.loadProject(project, &error), qPrintable(error));
    QCOMPARE(reopened.difficulties().size(), 2);
    QCOMPARE(reopened.objects().size(), draft.objects.size());
    QVERIFY(source.open(QIODevice::ReadOnly));
    QCOMPARE(QCryptographicHash::hash(source.readAll(), QCryptographicHash::Sha256), hash);
    window.close();
}

void MainWindowTest::infernoGenerationUsesEditorDraftAndPreservesOfflineTask() {
    QString error; lmsc::BeatmapDocument original;
    QVERIFY2(original.createNew(QDir(m_song).filePath(QStringLiteral("song.ogg")), QStringLiteral("本地模型编辑集成"),
        120, 0, {}, &error), qPrintable(error));
    lmsc::BeatObject formalNote; formalNote.beat = 8; formalNote.direction = 1;
    QVERIFY(original.addObject(formalNote, &error));
    const QString project = m_temp.filePath(QStringLiteral("inferno-editor-project/project.lmsc"));
    QVERIFY(original.saveProject(project, &error));
    MainGenerationService model; MainRefinementService refinement;
    MainWindow window(nullptr, settingsFile()); window.setTestMode(true);
    window.setInfernoGenerationService(&model); window.setAiRefinementService(&refinement); window.show();
    QSignalSpy ready(&window, &MainWindow::documentReady); window.openPath(project);
    QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, 20000); QTRY_VERIFY_WITH_TIMEOUT(window.isAudioReady(), 20000);
    auto page = window.findChild<lmsc::AiRecognitionPage *>();
    auto nav = window.findChild<QListWidget *>(QStringLiteral("mainNavigation"));
    auto generate = window.findChild<QPushButton *>(QStringLiteral("aiGenerateButton"));
    auto target = window.findChild<QComboBox *>(QStringLiteral("aiGenerationDifficulty"));
    auto apply = window.findChild<QPushButton *>(QStringLiteral("editorCandidateApply"));
    auto settings = window.findChild<lmsc::SettingsPanel *>();
    auto controller = session(window);
    QVERIFY(page && nav && generate && target && settings && controller && apply);
    QCOMPARE(page->generationMode(), lmsc::AiRecognitionPage::InfernoSaber);
    nav->setCurrentRow(1); target->setCurrentIndex(target->findData(QStringLiteral("Hard")));
    generate->click(); QCOMPARE(model.requests.size(), 1);
    const auto request = model.requests.last();
    QCOMPARE(request.allowedTypes, lmsc::GeneratedTypes(lmsc::DirectionalType | lmsc::DotType));
    auto tokens = window.findChild<QSpinBox *>(QStringLiteral("aiMaxOutputTokens")); QVERIFY(tokens);
    tokens->setValue(4096); QVERIFY(settings->applyPreferences());
    QVERIFY(page->isRecognizing()); QVERIFY(!model.cancelledIds.contains(request.jobId));
    nav->setCurrentRow(1);
    auto draft = syntheticDraft(request);
    draft.summary = QStringLiteral("InfernoSaber 本地模型生成的可编辑候选");
    draft.warnings.append(QStringLiteral("模型动作仍需试听和人工检查"));
    draft.requiresPlayabilityReview = true;
    draft.playabilityActiveSeconds = request.audio.durationSeconds;
    draft.objects.first().x = 3; // The model returned a left-hand crossover.
    emit model.draftReady(draft);
    QCOMPARE(controller->formalDocument()->objects().size(), 1);
    QCOMPARE(controller->workingDocument()->objects().size(), 3);
    QVERIFY(!window.findChild<lmsc::GenerationPreviewDialog *>());
    QVERIFY(!apply->isEnabled());
    QVERIFY(window.findChild<QLabel *>(QStringLiteral("editorCandidateSummary"))->text().contains(QStringLiteral("模型动作")));
    auto grid = window.findChild<GridEditor *>(); QVERIFY(grid);
    emit grid->selectionChanged(QSet<QString>{controller->workingDocument()->objects().first().id});
    auto y = field<QSpinBox>(window, QStringLiteral("层 (0–2)")); QVERIFY(y); y->setValue(0);
    auto x = field<QSpinBox>(window, QStringLiteral("列 (0–3)")); QVERIFY(x); x->setValue(0);
    auto edit = button(window, QStringLiteral("应用属性")); QVERIFY(edit && edit->isEnabled()); edit->click();
    QCOMPARE(controller->workingDocument()->objects().first().y, 0);
    QCOMPARE(controller->workingDocument()->objects().first().x, 0); QVERIFY(apply->isEnabled());
    auto undo = namedAction(window, QStringLiteral("撤销"));
    auto redo = namedAction(window, QStringLiteral("重做"));
    auto save = namedAction(window, QStringLiteral("保存工程")); QVERIFY(undo && redo && save);
    undo->trigger(); QCOMPARE(controller->workingDocument()->objects().first().y, 1); QVERIFY(!apply->isEnabled());
    redo->trigger(); QCOMPARE(controller->workingDocument()->objects().first().y, 0);
    save->trigger(); QTRY_VERIFY_WITH_TIMEOUT(save->isEnabled(), 20000);
    lmsc::BeatmapDocument saved; QVERIFY2(saved.loadProject(project, &error), qPrintable(error));
    QCOMPARE(saved.objects().size(), 1); QVERIFY(!saved.editorDraftRecords().isUndefined());
    auto panel = window.findChild<lmsc::EditorRefinementPanel *>(); QVERIFY(panel); panel->setSelection(0, 4);
    auto refine = window.findChild<QPushButton *>(QStringLiteral("editorRefineButton"));
    QVERIFY(refine && refine->isEnabled()); refine->click(); QCOMPARE(refinement.requests.size(), 1);
    QVERIFY(refinement.requests.last().selectedOnly); refinement.finish(refinement.requests.last());
    QCOMPARE(controller->formalDocument()->objects().size(), 1);
    QVERIFY(apply->isEnabled()); apply->click();
    QCOMPARE(controller->formalDocument()->objects().size(), 3); QCOMPARE(controller->formalDocument()->difficulties().size(), 2);
    undo->trigger(); QCOMPARE(controller->formalDocument()->objects().size(), 1);
    redo->trigger(); QCOMPARE(controller->formalDocument()->objects().size(), 3);
    const auto captures = qEnvironmentVariable("LMSC_INFERNO_CAPTURE_DIRECTORY");
    if (!captures.isEmpty()) {
        QVERIFY(QDir().mkpath(captures));
        nav->setCurrentRow(1); page->setExpanded(true); window.resize(1200, 800); QTest::qWait(80);
        QVERIFY(window.grab().save(QDir(captures).filePath(QStringLiteral("inferno-editor-integration.png"))));
    }
    window.close();
}

void MainWindowTest::realInfernoGenerationEditorCheck() {
    const auto input = qEnvironmentVariable("LMSC_INFERNO_REAL_PROJECT");
    if (input.isEmpty()) QSKIP("通过 LMSC_INFERNO_REAL_PROJECT 显式启用现成模型真实编辑检查");
    const auto output = qEnvironmentVariable("LMSC_INFERNO_REAL_OUTPUT");
    const auto difficulty = qEnvironmentVariable("LMSC_INFERNO_REAL_DIFFICULTY", QStringLiteral("Hard"));
    QVERIFY(!output.isEmpty() && !QFileInfo::exists(output));
    QVERIFY(difficulty == QStringLiteral("Hard") || difficulty == QStringLiteral("Expert"));
    const auto fileHash = [](const QString &path) {
        QFile file(path); if (!file.open(QIODevice::ReadOnly)) return QByteArray();
        QCryptographicHash digest(QCryptographicHash::Sha256);
        while (!file.atEnd()) digest.addData(file.read(1024 * 1024));
        return digest.result().toHex();
    };
    const auto projectHash = fileHash(input); QVERIFY(!projectHash.isEmpty());
    lmsc::InfernoSaberGenerationService model;
    lmsc::InfernoSaberGenerationService::Config config;
    config.runnerPath = QDir(QString::fromUtf8(LMSC_REPOSITORY_DIR)).filePath(QStringLiteral("scripts/infernosaber_trial.py"));
    config.toolsDirectory = QDir(QString::fromUtf8(LMSC_AUDIO_TOOLS_DIR)).filePath(QStringLiteral("bin"));
    config.workDirectory = QDir(output).filePath(QStringLiteral("jobs"));
    model.setConfig(config); QVERIFY2(model.isAvailable(), qPrintable(model.availabilityReason()));
    MainWindow window(nullptr, settingsFile()); window.setTestMode(true); window.setInfernoGenerationService(&model);
    // The source project is only a read-only fixture; suppress its autosave.
    auto autosave = window.findChild<QTimer *>(QStringLiteral("documentAutosave")); QVERIFY(autosave); autosave->stop();
    window.show(); QSignalSpy ready(&window, &MainWindow::documentReady); window.openPath(input);
    QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, 30000); QTRY_VERIFY_WITH_TIMEOUT(window.isAudioReady(), 30000);
    auto controller = session(window); QVERIFY(controller);
    auto audio = window.findChild<AudioService *>(); QVERIFY(audio);
    const auto sourceAudio = audio->pcmSnapshot(); const auto audioHash = fileHash(sourceAudio.sourcePath); QVERIFY(!audioHash.isEmpty());
    const auto formalHash = lmsc::refinementBaselineHash(controller->formalDocument()->objects());
    const auto formalDifficultyCount = controller->formalDocument()->difficulties().size();
    window.findChild<QListWidget *>(QStringLiteral("mainNavigation"))->setCurrentRow(1);
    auto target = window.findChild<QComboBox *>(QStringLiteral("aiGenerationDifficulty")); QVERIFY(target);
    target->setCurrentIndex(target->findData(difficulty));
    auto generate = window.findChild<QPushButton *>(QStringLiteral("aiGenerateButton")); QVERIFY(generate && generate->isEnabled());
    QSignalSpy failed(&model, &lmsc::AiGenerationService::requestFailed);
    QSignalSpy drafts(&model, &lmsc::AiGenerationService::draftReady);
    bool runningCaptured = false, runningStageFits = false;
    connect(&model, &lmsc::AiGenerationService::progress, &window,
        [&](const QString &, int percent, const QString &) {
            if (percent < 50 || percent >= 95 || runningCaptured) return;
            runningCaptured = true;
            QTimer::singleShot(0, &window, [&] {
                auto progress = window.findChild<lmsc::AiRecognitionPage *>()->taskProgress();
                const auto stage = progress->stageLabel();
                auto cancel = window.findChild<QPushButton *>(QStringLiteral("aiCancelRecognition"));
                runningStageFits = stage->isVisible()
                    && stage->height() >= stage->heightForWidth(stage->width());
                QVERIFY(runningStageFits); QVERIFY(cancel && cancel->isVisible() && cancel->isEnabled());
                QVERIFY(window.grab().save(QDir(output).filePath(QStringLiteral("editor-model-running.png"))));
            });
        });
    QElapsedTimer timer; timer.start(); generate->click();
    QTRY_VERIFY_WITH_TIMEOUT(drafts.count() == 1 || !failed.isEmpty(), 180000);
    const auto elapsed = timer.elapsed();
    QVERIFY2(failed.isEmpty(), qPrintable(failed.isEmpty() ? QString() : failed.first().at(1).toString()));
    QCOMPARE(drafts.count(), 1); QVERIFY(controller->hasDraft());
    QVERIFY(runningCaptured && runningStageFits);
    HomepageUrlReceiver modelLogReceiver; ScopedFileUrlHandler modelLogHandler(&modelLogReceiver);
    auto logs = window.findChild<QPushButton *>(QStringLiteral("aiViewLog")); QVERIFY(logs && logs->isEnabled()); logs->click();
    QCOMPARE(modelLogReceiver.openedUrl.toLocalFile(),model.jobDirectory(model.status().jobId));
    QVERIFY(!window.findChild<lmsc::GenerationPreviewDialog *>());
    const auto draft = controller->generationDraft(); QVERIFY(draft.requiresPlayabilityReview && !draft.objects.isEmpty());
    QCOMPARE(draft.source.profile.name, difficulty);
    QCOMPARE(controller->formalDocument()->difficulties().size(), formalDifficultyCount);
    QCOMPARE(lmsc::refinementBaselineHash(controller->formalDocument()->objects()), formalHash);
    QCOMPARE(audio->pcmSnapshot().revision, sourceAudio.revision); QCOMPARE(fileHash(sourceAudio.sourcePath), audioHash);
    double firstSeconds = sourceAudio.durationSeconds;
    for (const auto &note : draft.objects) firstSeconds = std::min(firstSeconds, draft.source.timeMap.beatToSeconds(note.beat));
    QVERIFY(firstSeconds >= 0 && firstSeconds < 5);
    auto apply = window.findChild<QPushButton *>(QStringLiteral("editorCandidateApply")); QVERIFY(apply);
    QString reviewReason; const bool initiallyApplicable = controller->canApplyDraft(&reviewReason);
    QCOMPARE(apply->isEnabled(), initiallyApplicable);
    if (!initiallyApplicable) {
        QString error; QVERIFY(!controller->applyDraft(&error)); QVERIFY(!error.isEmpty());
        QCOMPARE(lmsc::refinementBaselineHash(controller->formalDocument()->objects()), formalHash);
    }
    auto grid = window.findChild<GridEditor *>(); QVERIFY(grid);
    const auto originalNote = controller->workingDocument()->objects().first();
    const auto originalWorkingHash = lmsc::refinementBaselineHash(controller->workingDocument()->objects());
    emit grid->selectionChanged(QSet<QString>{originalNote.id});
    auto y = field<QSpinBox>(window, QStringLiteral("层 (0–2)")); QVERIFY(y); y->setValue((originalNote.y + 1) % 3);
    auto edit = button(window, QStringLiteral("应用属性")); QVERIFY(edit && edit->isEnabled()); edit->click();
    const auto manualHash = lmsc::refinementBaselineHash(controller->workingDocument()->objects()); QVERIFY(manualHash != originalWorkingHash);
    auto undo = namedAction(window, QStringLiteral("撤销")); auto redo = namedAction(window, QStringLiteral("重做")); QVERIFY(undo && redo);
    undo->trigger(); QCOMPARE(lmsc::refinementBaselineHash(controller->workingDocument()->objects()), originalWorkingHash);
    redo->trigger(); QCOMPARE(lmsc::refinementBaselineHash(controller->workingDocument()->objects()), manualHash);
    QVERIFY(QDir().mkpath(output)); const auto savedProject = QDir(output).filePath(QStringLiteral("project.lmsc"));
    QString error; QVERIFY2(controller->formalDocument()->saveProject(savedProject, &error), qPrintable(error)); controller->markSaved();
    QCOMPARE(fileHash(input), projectHash); QCOMPARE(fileHash(sourceAudio.sourcePath), audioHash);
    lmsc::BeatmapDocument reopened; QVERIFY2(reopened.loadProject(savedProject, &error), qPrintable(error));
    lmsc::EditorSessionController restored; restored.setFormalDocument(&reopened, QStringLiteral("restored-real-model-check"));
    QStringList restoreWarnings; QVERIFY(restored.restoreFromJson(reopened.editorDraftRecords(), &restoreWarnings));
    restored.selectTarget(difficulty); QVERIFY(restored.hasDraft());
    QCOMPARE(lmsc::refinementBaselineHash(restored.workingDocument()->objects()), manualHash);
    QVERIFY(restored.generationDraft().requiresPlayabilityReview);
    QCOMPARE(restored.canApplyDraft(), controller->canApplyDraft());
    window.resize(1440, 900); QTest::qWait(100);
    QVERIFY(window.grab().save(QDir(output).filePath(QStringLiteral("editor-working-draft.png"))));
    window.findChild<lmsc::AiRecognitionPage *>()->setExpanded(true); QTest::qWait(100);
    QVERIFY(window.grab().save(QDir(output).filePath(QStringLiteral("editor-model-panel.png"))));
    QJsonObject receipt{{"difficulty", difficulty}, {"elapsedMs", static_cast<double>(elapsed)}, {"firstNoteSeconds", firstSeconds},
        {"notes", draft.objects.size()}, {"initiallyApplicable", initiallyApplicable}, {"reviewReason", reviewReason},
        {"originalAudioSha256", QString::fromLatin1(audioHash)}, {"sourceProjectUnchanged", true},
        {"audioUnchanged", true}, {"formalUnchanged", true}, {"manualUndoRedo", true},
        {"draftSaveReopen", true}, {"reviewGuardPreserved", true}, {"separatePreviewWindow", false},
        {"runningStageFits", runningStageFits}, {"runningCancelAvailable", runningCaptured}};
    QSaveFile report(QDir(output).filePath(QStringLiteral("editor-check.json"))); QVERIFY(report.open(QIODevice::WriteOnly));
    QVERIFY(report.write(QJsonDocument(receipt).toJson()) > 0); QVERIFY(report.commit());
    controller->markSaved(); window.close();
}

void MainWindowTest::existingNewSongRefinementIsAtomicAndReusable() {
    QString error; lmsc::BeatmapDocument original;
    QVERIFY2(original.createNew(QDir(m_song).filePath(QStringLiteral("song.ogg")), QStringLiteral("已有谱精修"), 120, 0, {}, &error), qPrintable(error));
    lmsc::BeatObject note; note.beat = 2; note.x = 0; note.y = 0; note.direction = 1;
    note.preservedCustomData = {{"_customData", QJsonObject{{"_color", QJsonArray{.2, .3, .4, 1}}}}};
    QVERIFY2(original.addObject(note, &error), qPrintable(error));
    const QString originalId = original.objects().first().id;
    const auto preserved = original.objects().first().preservedCustomData;
    const QString project = m_temp.filePath(QStringLiteral("refinement-project/project.lmsc"));
    QVERIFY2(original.saveProject(project, &error), qPrintable(error));
    MainRefinementService service; MainWindow window(nullptr, settingsFile());
    window.setTestMode(true); window.setAiRefinementService(&service); window.show();
    QSignalSpy ready(&window, &MainWindow::documentReady); window.openPath(project);
    QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, 20000); QTRY_VERIFY_WITH_TIMEOUT(window.isAudioReady(), 20000);
    auto entry = window.findChild<QPushButton *>(QStringLiteral("refineCurrentChartButton"));
    QVERIFY(entry && entry->isEnabled());
    auto audio = window.findChild<AudioService *>(); QVERIFY(audio);
    const QString audioPath = audio->pcmSnapshot().sourcePath;
    QSignalSpy decoded(audio, &AudioService::audioReady); audio->loadAudio(audioPath);
    QVERIFY(!entry->isEnabled());
    QTRY_COMPARE_WITH_TIMEOUT(decoded.count(), 1, 20000); QTRY_VERIFY_WITH_TIMEOUT(window.isAudioReady(), 20000);
    QVERIFY(entry->isEnabled());
    auto controller = session(window); QVERIFY(controller);
    auto panel = window.findChild<lmsc::EditorRefinementPanel *>(); QVERIFY(panel);
    auto nav = window.findChild<QListWidget *>(QStringLiteral("mainNavigation"));
    nav->setCurrentRow(1); panel->setSelection(0, 4);
    auto refine = window.findChild<QPushButton *>(QStringLiteral("editorRefineButton"));
    QVERIFY(refine && refine->isEnabled()); refine->click(); QCOMPARE(service.requests.size(), 1);
    const auto request = service.requests.last();
    QVERIFY(request.selectedOnly); QCOMPARE(request.endSeconds, 4.0);
    QCOMPARE(request.baseline.first().id, originalId);
    service.finish(request);
    QCOMPARE(controller->formalDocument()->objects().first().y, 0);
    QCOMPARE(controller->workingDocument()->objects().first().y, 2);
    QCOMPARE(controller->initialDocument()->objects().first().y, 0);
    QVERIFY(controller->hasBeforeRefinement());
    QCOMPARE(controller->workingDocument()->objects().first().preservedCustomData, preserved);
    QVERIFY(!window.findChild<lmsc::GenerationPreviewDialog *>());
    const QString captures = qEnvironmentVariable("LMSC_HYBRID_CAPTURE_DIRECTORY");
    if (!captures.isEmpty()) {
        QVERIFY(QDir().mkpath(captures)); const auto theme = lmsc::ThemeManager::mode();
        window.resize(1080, 760);
        for (const QString &mode : QStringList{QStringLiteral("light"), QStringLiteral("dark")}) {
            lmsc::ThemeManager::apply(mode); QTest::qWait(80);
            QVERIFY(window.grab().save(QDir(captures).filePath("refinement-existing-" + mode + ".png")));
        }
        lmsc::ThemeManager::apply(theme);
    }
    nav->setCurrentRow(3); nav->setCurrentRow(1);
    QCOMPARE(controller->workingDocument()->objects().first().y, 2);
    QVERIFY(!service.discardedJobs.contains(request.generation.jobId));
    auto resume = window.findChild<QPushButton *>(QStringLiteral("editorRefinementResume"));
    QVERIFY(resume && resume->isEnabled()); resume->click();
    QCOMPARE(service.resumedJobs, QStringList{request.generation.jobId}); service.finish(request);
    auto apply = window.findChild<QPushButton *>(QStringLiteral("editorCandidateApply"));
    QVERIFY(apply->isEnabled()); apply->click();
    QCOMPARE(controller->formalDocument()->objects().first().id, originalId);
    QCOMPARE(controller->formalDocument()->objects().first().y, 2);
    auto undo = namedAction(window, QStringLiteral("撤销"));
    auto redo = namedAction(window, QStringLiteral("重做"));
    auto save = namedAction(window, QStringLiteral("保存工程"));
    QVERIFY(undo && redo && save);
    undo->trigger(); QCOMPARE(controller->formalDocument()->objects().first().y, 0);
    redo->trigger(); QCOMPARE(controller->formalDocument()->objects().first().y, 2);
    save->trigger(); QTRY_VERIFY_WITH_TIMEOUT(save->isEnabled(), 20000);
    lmsc::BeatmapDocument reopened; QVERIFY2(reopened.loadProject(project, &error), qPrintable(error));
    QCOMPARE(reopened.objects().first().id, originalId); QCOMPARE(reopened.objects().first().y, 2);
    QCOMPARE(reopened.objects().first().preservedCustomData, preserved);
    window.openPath(m_song); QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 2, 20000);
    QTRY_VERIFY_WITH_TIMEOUT(window.isAudioReady(), 20000);
    QVERIFY(!entry->isEnabled()); window.close();
}

void MainWindowTest::aiGenerationPreviewAndAtomicApply() {
    QString error; lmsc::BeatmapDocument original;
    QVERIFY2(original.createNew(QDir(m_song).filePath(QStringLiteral("song.ogg")), QStringLiteral("统一编辑测试"), 120, 0, {}, &error), qPrintable(error));
    lmsc::BeatObject old; old.beat = 8; old.x = 1; old.direction = 1;
    old.preservedCustomData = {{"_customData", QJsonObject{{"_color", QJsonArray{.2, .3, .4, 1}}}}};
    QVERIFY2(original.addObject(old, &error), qPrintable(error));
    const QString expertId = original.currentDifficultyId();
    const auto originalObject = original.objects().first();
    const QString project = m_temp.filePath(QStringLiteral("ai-generation-project/project.lmsc"));
    QVERIFY2(original.saveProject(project, &error), qPrintable(error));
    MainGenerationService service; MainWindow window(nullptr, settingsFile());
    window.setTestMode(true); window.setAiGenerationService(&service); window.show();
    QSignalSpy ready(&window, &MainWindow::documentReady); window.openPath(project);
    QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, 20000); QTRY_VERIFY_WITH_TIMEOUT(window.isAudioReady(), 20000);
    auto controller = session(window); QVERIFY(controller);
    auto audio = window.findChild<AudioService *>();
    auto nav = window.findChild<QListWidget *>(QStringLiteral("mainNavigation"));
    auto generate = window.findChild<QPushButton *>(QStringLiteral("aiGenerateButton"));
    auto target = window.findChild<QComboBox *>(QStringLiteral("aiGenerationDifficulty"));
    auto apply = window.findChild<QPushButton *>(QStringLiteral("editorCandidateApply"));
    auto workspace = window.findChild<QStackedWidget *>(QStringLiteral("workspacePages"));
    QVERIFY(audio && nav && generate && target && apply && workspace);
    audio->seek(1.25); audio->setLoop(1, 2, true);
    const auto originalAudio = audio->pcmSnapshot();
    QFile sourceAudio(originalAudio.sourcePath); QVERIFY(sourceAudio.open(QIODevice::ReadOnly));
    const auto audioHash = QCryptographicHash::hash(sourceAudio.readAll(), QCryptographicHash::Sha256); sourceAudio.close();
    nav->setCurrentRow(1); target->setCurrentIndex(target->findData(QStringLiteral("Hard")));
    generate->click(); QCOMPARE(service.requests.size(), 1);
    const auto obsolete = syntheticDraft(service.requests.last());
    auto bpm = field<QDoubleSpinBox>(window, QStringLiteral("BPM"));
    auto calibrate = button(window, QStringLiteral("应用校准")); QVERIFY(bpm && calibrate);
    bpm->setValue(121); calibrate->click();
    emit service.draftReady(obsolete);
    QVERIFY(!controller->hasDraft());
    QCOMPARE(controller->formalDocument()->objects().size(), 1);
    bpm->setValue(120); calibrate->click();
    generate->click(); QCOMPARE(service.requests.size(), 2);
    auto draft = syntheticDraft(service.requests.last());
    nav->setCurrentRow(3); emit service.draftReady(draft);
    QCOMPARE(workspace->currentWidget()->objectName(), QStringLiteral("settingsPanel"));
    nav->setCurrentRow(1);
    QCOMPARE(workspace->currentWidget()->objectName(), QStringLiteral("editorPage"));
    QCOMPARE(controller->currentTargetKey(), QStringLiteral("Hard"));
    QCOMPARE(controller->formalDocument()->objects().size(), 1);
    QCOMPARE(controller->workingDocument()->objects().size(), 3);
    QCOMPARE(controller->initialDocument()->objects().size(), 3);
    QVERIFY(!window.findChild<lmsc::GenerationPreviewDialog *>());
    QVERIFY(audio->loopEnabled()); QCOMPARE(audio->loopStartSeconds(), 1.0); QCOMPARE(audio->loopEndSeconds(), 2.0);
    nav->setCurrentRow(3); nav->setCurrentRow(1);
    QCOMPARE(controller->workingDocument()->objects().size(), 3);
    QVERIFY(apply->isEnabled()); apply->click();
    QCOMPARE(controller->formalDocument()->difficulties().size(), 2);
    QCOMPARE(controller->formalDocument()->objects().size(), 3);
    auto undo = namedAction(window, QStringLiteral("撤销"));
    auto redo = namedAction(window, QStringLiteral("重做")); QVERIFY(undo && redo);
    undo->trigger();
    QCOMPARE(controller->formalDocument()->difficulties().size(), 1);
    QCOMPARE(controller->formalDocument()->objects().size(), 1);
    QCOMPARE(controller->formalDocument()->objects().first().id, originalObject.id);
    redo->trigger(); QCOMPARE(controller->formalDocument()->objects().size(), 3);

    target->setCurrentIndex(target->findData(QStringLiteral("Easy"))); generate->click();
    QCOMPARE(service.requests.size(), 3); emit service.draftReady(syntheticDraft(service.requests.last(), 2));
    QCOMPARE(controller->formalDocument()->difficulties().size(), 2);
    QCOMPARE(controller->workingDocument()->objects().size(), 2);
    apply->click(); QCOMPARE(controller->formalDocument()->difficulties().size(), 3);
    QCOMPARE(controller->formalDocument()->objectCount(expertId), 1);
    undo->trigger(); QCOMPARE(controller->formalDocument()->difficulties().size(), 2);
    redo->trigger(); QCOMPARE(controller->formalDocument()->difficulties().size(), 3);

    target->setCurrentIndex(target->findData(QStringLiteral("Hard"))); generate->click();
    QCOMPARE(service.requests.size(), 4); emit service.draftReady(syntheticDraft(service.requests.last(), 1));
    auto replacement = window.findChild<QWidget *>(QStringLiteral("draftReplacementPrompt"));
    if (replacement->isVisible()) window.findChild<QPushButton *>(QStringLiteral("draftReplacementAccept"))->click();
    QCOMPARE(controller->workingDocument()->objects().size(), 1);
    apply->click(); QCOMPARE(controller->formalDocument()->objects().size(), 1);
    QCOMPARE(controller->formalDocument()->difficulties().size(), 3);
    undo->trigger(); QCOMPARE(controller->formalDocument()->objects().size(), 3);
    QCOMPARE(controller->formalDocument()->objectCount(expertId), 1);
    const auto currentId = controller->formalDocument()->currentDifficultyId();
    QVERIFY(controller->formalDocument()->setDifficulty(expertId, &error));
    QCOMPARE(controller->formalDocument()->objects().first().preservedCustomData, originalObject.preservedCustomData);
    QCOMPARE(controller->formalDocument()->objects().first().id, originalObject.id);
    QVERIFY(controller->formalDocument()->setDifficulty(currentId, &error));
    QVERIFY(sourceAudio.open(QIODevice::ReadOnly));
    QCOMPARE(QCryptographicHash::hash(sourceAudio.readAll(), QCryptographicHash::Sha256), audioHash);
    QCOMPARE(audio->pcmSnapshot().revision, originalAudio.revision);
    lmsc::BeatmapDocument unchangedDisk; QVERIFY2(unchangedDisk.loadProject(project, &error), qPrintable(error));
    QCOMPARE(unchangedDisk.objects().size(), 1);
    QCOMPARE(unchangedDisk.difficulties().first().name, QStringLiteral("Expert"));
    window.close();
}

void MainWindowTest::manualEditCancelsRefinementAndRejectsLateResult() {
    QString error; lmsc::BeatmapDocument original;
    QVERIFY2(original.createNew(QDir(m_song).filePath(QStringLiteral("song.ogg")), QStringLiteral("手改取消精修"), 120, 0, {}, &error), qPrintable(error));
    lmsc::BeatObject note; note.beat = 2; note.x = 0; note.y = 0; note.direction = 1;
    QVERIFY(original.addObject(note, &error)); note.beat = 4; note.color = 1; note.x = 2; note.direction = 0;
    QVERIFY(original.addObject(note, &error));
    const auto secondId = original.objects()[1].id;
    const QString project = m_temp.filePath(QStringLiteral("manual-refinement-project/project.lmsc"));
    QVERIFY(original.saveProject(project, &error));
    MainRefinementService service; service.publishPartialOnCancel = true;
    MainWindow window(nullptr, settingsFile()); window.setTestMode(true); window.setAiRefinementService(&service); window.show();
    QSignalSpy ready(&window, &MainWindow::documentReady); window.openPath(project);
    QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, 20000); QTRY_VERIFY_WITH_TIMEOUT(window.isAudioReady(), 20000);
    auto controller = session(window); QVERIFY(controller);
    auto panel = window.findChild<lmsc::EditorRefinementPanel *>(); QVERIFY(panel);
    window.findChild<QPushButton *>(QStringLiteral("refineCurrentChartButton"))->click();
    QCOMPARE(service.requests.size(), 1); const auto request = service.requests.last();
    emit service.progress(request.generation.jobId, 31, QStringLiteral("精修乐句 2/5 · 9.3–11.1 秒 · 剩余 8 个乐句"));
    auto percentage = panel->taskProgress()->percentageLabel();
    window.resize(1080, 760); QTest::qWait(40);
    QCOMPARE(percentage->text(), QStringLiteral("31%")); QVERIFY(percentage->isVisible());
    auto refinementDetails = panel->findChild<QScrollArea *>(QStringLiteral("editorRefinementDetailsScroll"));
    QVERIFY(refinementDetails && refinementDetails->isVisible());
    QVERIFY(refinementDetails->height() >= panel->fontMetrics().height() * 3);
    QVERIFY(panel->taskProgress()->rect().contains(QRect(percentage->mapTo(panel->taskProgress(), QPoint()), percentage->size())));
    QVERIFY(window.rect().contains(QRect(percentage->mapTo(&window, QPoint()), percentage->size())));
    auto track = window.findChild<TrackView *>();
    auto grid = window.findChild<GridEditor *>();
    QVERIFY(track && grid && track->isVisible() && grid->isVisible());
    auto centerTop = track->parentWidget();
    QCOMPARE(grid->parentWidget(), centerTop);
    for (auto view : QVector<QWidget *>{track, grid}) {
        QVERIFY(centerTop->rect().contains(QRect(view->mapTo(centerTop, QPoint()), view->size())));
        QVERIFY(window.rect().contains(QRect(view->mapTo(&window, QPoint()), view->size())));
    }
    for (auto ancestor = percentage->parentWidget(); ancestor; ancestor = ancestor->parentWidget())
        QVERIFY(!qobject_cast<QScrollArea *>(ancestor));
    const auto captures = qEnvironmentVariable("LMSC_EDITOR_SCREENSHOT");
    if (!captures.isEmpty()) {
        QVERIFY(QDir().mkpath(captures)); const auto theme = lmsc::ThemeManager::mode();
        const auto scale = qEnvironmentVariable("QT_SCALE_FACTOR", QStringLiteral("1"));
        for (const auto &mode : QStringList{QStringLiteral("light"), QStringLiteral("dark")}) {
            lmsc::ThemeManager::apply(mode); QTest::qWait(80);
            QVERIFY(!track->grabFramebuffer().isNull());
            QVERIFY(window.grab().save(QDir(captures).filePath(QStringLiteral("unified-editor-%1-scale-%2.png").arg(mode, scale))));
        }
        lmsc::ThemeManager::apply(theme);
    }
    emit grid->selectionChanged(QSet<QString>{secondId});
    auto y = field<QSpinBox>(window, QStringLiteral("层 (0–2)")); QVERIFY(y);
    y->setValue(1); auto applyProperties = button(window, QStringLiteral("应用属性"));
    QVERIFY(applyProperties && applyProperties->isEnabled()); applyProperties->click();
    QCOMPARE(service.cancelledJobs, QStringList{request.generation.jobId});
    QVERIFY(service.discardedJobs.contains(request.generation.jobId));
    QCOMPARE(controller->workingDocument()->objects()[0].y, 2);
    QCOMPARE(controller->workingDocument()->objects()[1].y, 1);
    QCOMPARE(controller->formalDocument()->objects()[0].y, 0);
    QCOMPARE(controller->formalDocument()->objects()[1].y, 0);
    const auto editedHash = lmsc::refinementBaselineHash(controller->workingDocument()->objects());
    service.finish(request); // Deliberately late, with the older second-note position.
    QCOMPARE(lmsc::refinementBaselineHash(controller->workingDocument()->objects()), editedHash);
    QVERIFY(!window.findChild<QPushButton *>(QStringLiteral("editorRefinementResume"))->isEnabled());
    QVERIFY(!window.findChild<lmsc::GenerationPreviewDialog *>());
    window.close();
}

void MainWindowTest::savedDraftReopensForManualEditingWithoutApplying() {
    QString error; lmsc::BeatmapDocument original;
    QVERIFY(original.createNew(QDir(m_song).filePath(QStringLiteral("song.ogg")), QStringLiteral("草稿恢复"), 120, 0, {}, &error));
    lmsc::BeatObject note; note.beat = 8; note.direction = 1; QVERIFY(original.addObject(note, &error));
    const QString project = m_temp.filePath(QStringLiteral("saved-editor-draft/project.lmsc"));
    QVERIFY(original.saveProject(project, &error));
    MainGenerationService service;
    {
        MainWindow window(nullptr, settingsFile()); window.setTestMode(true); window.setAiGenerationService(&service); window.show();
        QSignalSpy ready(&window, &MainWindow::documentReady); window.openPath(project);
        QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, 20000); QTRY_VERIFY_WITH_TIMEOUT(window.isAudioReady(), 20000);
        window.findChild<QListWidget *>(QStringLiteral("mainNavigation"))->setCurrentRow(1);
        window.findChild<QPushButton *>(QStringLiteral("aiGenerateButton"))->click();
        QCOMPARE(service.requests.size(), 1); emit service.draftReady(syntheticDraft(service.requests.last(), 2));
        auto controller = session(window); QVERIFY(controller && controller->hasDraft());
        const auto id = controller->workingDocument()->objects().first().id;
        emit window.findChild<GridEditor *>()->selectionChanged(QSet<QString>{id});
        field<QSpinBox>(window, QStringLiteral("层 (0–2)"))->setValue(2);
        button(window, QStringLiteral("应用属性"))->click();
        QCOMPARE(controller->workingDocument()->objects().first().y, 2);
        QCOMPARE(controller->formalDocument()->objects().size(), 1);
        auto save = namedAction(window, QStringLiteral("保存工程")); QVERIFY(save); save->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(save->isEnabled(), 20000);
        lmsc::BeatmapDocument saved; QVERIFY(saved.loadProject(project, &error));
        QCOMPARE(saved.objects().size(), 1); QVERIFY(!saved.editorDraftRecords().isUndefined());
        const QString exported = m_temp.filePath(QStringLiteral("saved-draft-formal-export"));
        QVERIFY2(saved.exportSong(exported, &error), qPrintable(error));
        lmsc::BeatmapDocument exportedChart; QVERIFY(exportedChart.loadSong(exported, &error));
        QCOMPARE(exportedChart.objects().size(), 1);
        window.close();
    }
    MainWindow restored(nullptr, settingsFile()); restored.setTestMode(true); restored.show();
    QSignalSpy ready(&restored, &MainWindow::documentReady); restored.openPath(project);
    QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, 20000); QTRY_VERIFY_WITH_TIMEOUT(restored.isAudioReady(), 20000);
    auto controller = session(restored); QVERIFY(controller && controller->hasDraft());
    QVERIFY(!controller->viewReadOnly());
    QCOMPARE(controller->workingDocument()->objects().size(), 2);
    QCOMPARE(controller->workingDocument()->objects().first().y, 2);
    QCOMPARE(controller->initialDocument()->objects().first().y, 1);
    const auto id = controller->workingDocument()->objects().first().id;
    emit restored.findChild<GridEditor *>()->selectionChanged(QSet<QString>{id});
    field<QSpinBox>(restored, QStringLiteral("列 (0–3)"))->setValue(1);
    button(restored, QStringLiteral("应用属性"))->click();
    QCOMPARE(controller->workingDocument()->objects().first().x, 1);
    QCOMPARE(controller->formalDocument()->objects().size(), 1);
    QVERIFY(!restored.findChild<QPushButton *>(QStringLiteral("editorRefinementResume"))->isEnabled());
    QCOMPARE(service.requests.size(), 1); QVERIFY(!restored.findChild<lmsc::GenerationPreviewDialog *>());
    restored.close();
}

void MainWindowTest::draftSynchronizationFailureDoesNotMarkSavedOrReplaceDisk() {
    QString error; lmsc::BeatmapDocument original;
    QVERIFY(original.createNew(QDir(m_song).filePath(QStringLiteral("song.ogg")), QStringLiteral("草稿同步失败保护"), 120, 0, {}, &error));
    lmsc::BeatObject note; note.beat = 8; note.direction = 1; QVERIFY(original.addObject(note, &error));
    const QString project = m_temp.filePath(QStringLiteral("failed-draft-sync/project.lmsc"));
    QVERIFY(original.saveProject(project, &error));
    MainGenerationService service; MainWindow window(nullptr, settingsFile());
    window.setTestMode(true); window.setAiGenerationService(&service); window.show();
    auto autosave = window.findChild<QTimer *>(QStringLiteral("documentAutosave")); QVERIFY(autosave);
    autosave->stop(); // Trigger the real timeout handler below without relying on a 30-second wait.
    QSignalSpy ready(&window, &MainWindow::documentReady); window.openPath(project);
    QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, 20000); QTRY_VERIFY_WITH_TIMEOUT(window.isAudioReady(), 20000);
    window.findChild<QListWidget *>(QStringLiteral("mainNavigation"))->setCurrentRow(1);
    window.findChild<QPushButton *>(QStringLiteral("aiGenerateButton"))->click();
    QCOMPARE(service.requests.size(), 1); emit service.draftReady(syntheticDraft(service.requests.last(), 2));
    auto controller = session(window); QVERIFY(controller && controller->hasDraft());
    auto save = namedAction(window, QStringLiteral("保存工程")); QVERIFY(save && save->isEnabled());
    save->trigger(); QVERIFY(!controller->isDirty()); QVERIFY(!controller->formalDocument()->isModified());
    const auto savedRecords = controller->formalDocument()->editorDraftRecords();
    const auto readFile = [](const QString &path) {
        QFile file(path); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
    };
    const auto savedBytes = readFile(project); QVERIFY(!savedBytes.isEmpty());

    // Keep an earlier, valid recovery snapshot with a distinct formal edit. A
    // stale autosave after failed synchronization would overwrite this file.
    lmsc::BeatmapDocument earlierRecovery; QVERIFY(earlierRecovery.loadProject(project, &error));
    note.beat = 10; note.x = 3; note.color = 1; note.direction = 0;
    QVERIFY(earlierRecovery.addObject(note, &error)); QVERIFY(earlierRecovery.autoSave(&error));
    const auto recoveryPath = lmsc::BeatmapDocument::recoveryPath(project);
    const auto recoveryBytes = readFile(recoveryPath); QVERIFY(!recoveryBytes.isEmpty());

    // JSON escapes each control character to six bytes, exceeding the real
    // 32 MiB limit while keeping the in-memory fixture small on 32-bit builds.
    auto oversizedRecords = controller->toJson().toObject();
    oversizedRecords.insert(QStringLiteral("futureOpaqueData"), QString(6 * 1024 * 1024, QChar(1)));
    QStringList warnings; QVERIFY(controller->restoreFromJson(oversizedRecords, &warnings));
    QVERIFY(warnings.isEmpty()); QVERIFY(controller->isApplicable());
    QVERIFY(!controller->isDirty());
    auto grid = window.findChild<GridEditor *>(); QVERIFY(grid);
    emit grid->selectionChanged(QSet<QString>{controller->workingDocument()->objects().first().id});
    auto y = field<QSpinBox>(window, QStringLiteral("层 (0–2)")); QVERIFY(y); y->setValue(2);
    auto properties = button(window, QStringLiteral("应用属性")); QVERIFY(properties && properties->isEnabled());
    properties->click();
    QCOMPARE(controller->workingDocument()->objects().first().y, 2);
    const auto workingHash = lmsc::refinementBaselineHash(controller->workingDocument()->objects());
    QVERIFY(controller->isDirty()); QVERIFY(!controller->formalDocument()->isModified());
    QVERIFY(controller->formalDocument()->editorDraftRecords() == savedRecords);

    window.statusBar()->clearMessage(); save->trigger();
    QVERIFY(window.statusBar()->currentMessage().contains(QStringLiteral("32 MiB")));
    QVERIFY(controller->isDirty()); QVERIFY(save->isEnabled());
    QVERIFY(readFile(project) == savedBytes);
    QVERIFY(lmsc::refinementBaselineHash(controller->workingDocument()->objects()) == workingHash);
    QVERIFY(controller->formalDocument()->editorDraftRecords() == savedRecords);

    window.statusBar()->clearMessage();
    QVERIFY(QMetaObject::invokeMethod(autosave, "timeout", Qt::DirectConnection));
    QVERIFY(window.statusBar()->currentMessage().contains(QStringLiteral("32 MiB")));
    QVERIFY(readFile(recoveryPath) == recoveryBytes);
    QVERIFY(readFile(project) == savedBytes);
    QVERIFY(controller->isDirty());
    QVERIFY(lmsc::refinementBaselineHash(controller->workingDocument()->objects()) == workingHash);
    lmsc::BeatmapDocument saved; QVERIFY(saved.loadProject(project, &error));
    QVERIFY(saved.editorDraftRecords() == savedRecords); QCOMPARE(saved.objects().size(), 1);
    window.close();
}

void MainWindowTest::difficultySwitchPreservesIndependentDrafts() {
    QString error; lmsc::BeatmapDocument original;
    QVERIFY(original.createNew(QDir(m_song).filePath(QStringLiteral("song.ogg")), QStringLiteral("两难度草稿"), 120, 0, {}, &error));
    lmsc::BeatObject note; note.beat = 8; note.direction = 1; QVERIFY(original.addObject(note, &error));
    QVector<lmsc::BeatObject> hard{note}; hard[0].beat = 6;
    QVERIFY(original.applyGeneratedChart(hard, QStringLiteral("Hard"), 5, original.revision(), &error));
    const QString project = m_temp.filePath(QStringLiteral("two-editor-drafts/project.lmsc"));
    QVERIFY(original.saveProject(project, &error));
    MainGenerationService service; MainWindow window(nullptr, settingsFile());
    window.setTestMode(true); window.setAiGenerationService(&service); window.show();
    QSignalSpy ready(&window, &MainWindow::documentReady); window.openPath(project);
    QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, 20000); QTRY_VERIFY_WITH_TIMEOUT(window.isAudioReady(), 20000);
    auto controller = session(window); QVERIFY(controller);
    auto target = window.findChild<QComboBox *>(QStringLiteral("aiGenerationDifficulty"));
    auto generate = window.findChild<QPushButton *>(QStringLiteral("aiGenerateButton"));
    window.findChild<QListWidget *>(QStringLiteral("mainNavigation"))->setCurrentRow(1);
    target->setCurrentIndex(target->findData(QStringLiteral("Hard"))); generate->click();
    emit service.draftReady(syntheticDraft(service.requests.last(), 2));
    const auto hardHash = lmsc::refinementBaselineHash(controller->workingDocument()->objects());
    target->setCurrentIndex(target->findData(QStringLiteral("Expert"))); generate->click();
    QCOMPARE(service.requests.size(), 2); emit service.draftReady(syntheticDraft(service.requests.last(), 3));
    const auto expertHash = lmsc::refinementBaselineHash(controller->workingDocument()->objects());
    QCOMPARE(controller->targetKeys().size(), 2);
    auto difficulties = window.findChild<QListWidget *>(QStringLiteral("difficultyList")); QVERIFY(difficulties);
    for (int i = 0; i < difficulties->count(); ++i)
        if (difficulties->item(i)->data(Qt::UserRole + 1).toString() == QStringLiteral("Hard")) { difficulties->setCurrentRow(i); break; }
    QCOMPARE(controller->currentTargetKey(), QStringLiteral("Hard"));
    QCOMPARE(lmsc::refinementBaselineHash(controller->workingDocument()->objects()), hardHash);
    QVERIFY(controller->isApplicable());
    for (int i = 0; i < difficulties->count(); ++i)
        if (difficulties->item(i)->data(Qt::UserRole + 1).toString() == QStringLiteral("Expert")) { difficulties->setCurrentRow(i); break; }
    QCOMPARE(controller->currentTargetKey(), QStringLiteral("Expert"));
    QCOMPARE(lmsc::refinementBaselineHash(controller->workingDocument()->objects()), expertHash);
    QVERIFY(controller->isApplicable());
    QCOMPARE(controller->formalDocument()->objects().size(), 1);
    QVERIFY(!window.findChild<lmsc::GenerationPreviewDialog *>()); window.close();
}

void MainWindowTest::viewingCachedDraftPreservesRefinementPartialAndManualChanges() {
    QString error; lmsc::BeatmapDocument original;
    QVERIFY(original.createNew(QDir(m_song).filePath(QStringLiteral("song.ogg")), QStringLiteral("查看草稿时保留精修"), 120, 0, {}, &error));
    lmsc::BeatObject note; note.beat = 2; note.x = 0; note.y = 0; note.direction = 1;
    QVERIFY(original.addObject(note, &error)); note.beat = 4; note.color = 1; note.x = 2; note.direction = 0;
    QVERIFY(original.addObject(note, &error));
    const QString project = m_temp.filePath(QStringLiteral("view-cached-draft/project.lmsc"));
    QVERIFY(original.saveProject(project, &error));
    MainGenerationService generation; MainRefinementService refinement; refinement.publishPartialOnCancel = true;
    MainWindow window(nullptr, settingsFile()); window.setTestMode(true);
    window.setAiGenerationService(&generation); window.setAiRefinementService(&refinement); window.show();
    QSignalSpy ready(&window, &MainWindow::documentReady); window.openPath(project);
    QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, 20000); QTRY_VERIFY_WITH_TIMEOUT(window.isAudioReady(), 20000);
    auto controller = session(window); QVERIFY(controller);
    auto target = window.findChild<QComboBox *>(QStringLiteral("aiGenerationDifficulty"));
    auto generate = window.findChild<QPushButton *>(QStringLiteral("aiGenerateButton"));
    auto view = window.findChild<QPushButton *>(QStringLiteral("aiViewCandidate"));
    auto difficulties = window.findChild<QListWidget *>(QStringLiteral("difficultyList"));
    auto grid = window.findChild<GridEditor *>();
    auto y = field<QSpinBox>(window, QStringLiteral("层 (0–2)"));
    auto properties = button(window, QStringLiteral("应用属性"));
    QVERIFY(target && generate && view && difficulties && grid && y && properties);
    window.findChild<QListWidget *>(QStringLiteral("mainNavigation"))->setCurrentRow(1);
    target->setCurrentIndex(target->findData(QStringLiteral("Hard"))); generate->click();
    QCOMPARE(generation.requests.size(), 1); emit generation.draftReady(syntheticDraft(generation.requests.last(), 2));
    emit grid->selectionChanged(QSet<QString>{controller->workingDocument()->objects().first().id});
    y->setValue(2); properties->click();
    QCOMPARE(controller->workingDocument()->objects().first().y, 2);
    const auto hardHash = lmsc::refinementBaselineHash(controller->workingDocument()->objects());
    QVERIFY(hardHash != lmsc::refinementBaselineHash(controller->initialDocument()->objects()));
    const auto selectDifficulty = [&](const QString &name) {
        for (int i = 0; i < difficulties->count(); ++i)
            if (difficulties->item(i)->data(Qt::UserRole + 1).toString() == name) {
                difficulties->setCurrentRow(i); return true;
            }
        return false;
    };
    QVERIFY(selectDifficulty(QStringLiteral("Expert")));
    QCOMPARE(controller->view(), lmsc::EditorSessionController::View::Formal);
    auto refine = window.findChild<QPushButton *>(QStringLiteral("refineCurrentChartButton"));
    QVERIFY(refine && refine->isEnabled()); refine->click();
    QCOMPARE(refinement.requests.size(), 1); const auto request = refinement.requests.last();
    QCOMPARE(controller->currentTargetKey(), QStringLiteral("Expert"));
    QVERIFY(view->isEnabled()); view->click();

    QCOMPARE(controller->currentTargetKey(), QStringLiteral("Hard"));
    QCOMPARE(controller->view(), lmsc::EditorSessionController::View::Working);
    QCOMPARE(lmsc::refinementBaselineHash(controller->workingDocument()->objects()), hardHash);
    QCOMPARE(generation.requests.size(), 1);
    QCOMPARE(refinement.cancelledJobs, QStringList{request.generation.jobId});
    QVERIFY(refinement.discardedJobs.contains(request.generation.jobId));
    QVERIFY(refinement.status().jobId.isEmpty());
    auto prompt = window.findChild<QWidget *>(QStringLiteral("draftReplacementPrompt"));
    QVERIFY(prompt && !prompt->isVisible());
    QVERIFY(selectDifficulty(QStringLiteral("Expert")));
    QVERIFY(controller->workingDocument() && controller->initialDocument() && controller->beforeRefinementDocument());
    QCOMPARE(controller->workingDocument()->objects().first().y, 2);
    QCOMPARE(controller->initialDocument()->objects().first().y, 0);
    QCOMPARE(controller->beforeRefinementDocument()->objects().first().y, 0);
    QCOMPARE(controller->formalDocument()->objects().first().y, 0);
    const auto expertHash = lmsc::refinementBaselineHash(controller->workingDocument()->objects());
    auto late = refinement.resultFor(request); late.candidate.objects.last().y = 1;
    late.patch = lmsc::refinementDifference(request.baseline, late.candidate.objects);
    late.stats = lmsc::refinementStatistics(late.patch, request.baseline);
    emit refinement.candidateReady(late); // A distinct late result must not mutate either target.
    QCOMPARE(lmsc::refinementBaselineHash(controller->workingDocument()->objects()), expertHash);
    QVERIFY(selectDifficulty(QStringLiteral("Hard")));
    QCOMPARE(lmsc::refinementBaselineHash(controller->workingDocument()->objects()), hardHash);
    QVERIFY(!window.findChild<QPushButton *>(QStringLiteral("editorRefinementResume"))->isEnabled());
    QVERIFY(!window.findChild<lmsc::GenerationPreviewDialog *>()); window.close();
}

void MainWindowTest::inlinePromptCannotReplaceOtherDifficulty() {
    QString error; lmsc::BeatmapDocument original;
    QVERIFY(original.createNew(QDir(m_song).filePath(QStringLiteral("song.ogg")), QStringLiteral("固定提示目标"), 120, 0, {}, &error));
    lmsc::BeatObject note; note.beat = 8; note.direction = 1;
    QVERIFY(original.addObject(note, &error));
    const auto formalHash = lmsc::refinementBaselineHash(original.objects());
    const QString project = m_temp.filePath(QStringLiteral("inline-prompt-target/project.lmsc"));
    QVERIFY(original.saveProject(project, &error));
    MainGenerationService service; MainWindow window(nullptr, settingsFile());
    window.setTestMode(true); window.setAiGenerationService(&service); window.show();
    QSignalSpy ready(&window, &MainWindow::documentReady); window.openPath(project);
    QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, 20000); QTRY_VERIFY_WITH_TIMEOUT(window.isAudioReady(), 20000);
    auto controller = session(window); QVERIFY(controller);
    auto target = window.findChild<QComboBox *>(QStringLiteral("aiGenerationDifficulty"));
    auto generate = window.findChild<QPushButton *>(QStringLiteral("aiGenerateButton"));
    auto versions = window.findChild<QComboBox *>(QStringLiteral("editorCandidateVersion"));
    auto difficulties = window.findChild<QListWidget *>(QStringLiteral("difficultyList"));
    auto grid = window.findChild<GridEditor *>();
    auto y = field<QSpinBox>(window, QStringLiteral("层 (0–2)"));
    auto applyProperties = button(window, QStringLiteral("应用属性"));
    auto prompt = window.findChild<QWidget *>(QStringLiteral("draftReplacementPrompt"));
    auto proceed = window.findChild<QPushButton *>(QStringLiteral("draftReplacementAccept"));
    QVERIFY(target && generate && versions && difficulties && grid && y && applyProperties && prompt && proceed);
    window.findChild<QListWidget *>(QStringLiteral("mainNavigation"))->setCurrentRow(1);
    target->setCurrentIndex(target->findData(QStringLiteral("Hard"))); generate->click();
    QCOMPARE(service.requests.size(), 1); emit service.draftReady(syntheticDraft(service.requests.last(), 2));
    QCOMPARE(controller->currentTargetKey(), QStringLiteral("Hard"));

    // Hard has a candidate but no formal chart. Its formal comparison must be
    // an empty, locked baseline, rather than silently editing formal Expert.
    versions->setCurrentIndex(versions->findData(lmsc::EditorRefinementPanel::Formal));
    QCOMPARE(controller->view(), lmsc::EditorSessionController::View::Formal);
    QVERIFY(controller->viewReadOnly()); QVERIFY(!grid->isEnabled());
    QCOMPARE(controller->activeDocument()->objects().size(), 0);
    QCOMPARE(objectCount(window), 0);
    QCOMPARE(controller->formalDocument()->objects().size(), 1);
    QCOMPARE(lmsc::refinementBaselineHash(controller->formalDocument()->objects()), formalHash);
    versions->setCurrentIndex(versions->findData(lmsc::EditorRefinementPanel::Working));
    QVERIFY(grid->isEnabled());
    emit grid->selectionChanged(QSet<QString>{controller->workingDocument()->objects().first().id});
    y->setValue(2); applyProperties->click();
    const auto hardHash = lmsc::refinementBaselineHash(controller->workingDocument()->objects());
    QVERIFY(hardHash != lmsc::refinementBaselineHash(controller->initialDocument()->objects()));

    target->setCurrentIndex(target->findData(QStringLiteral("Expert"))); generate->click();
    QCOMPARE(service.requests.size(), 2); emit service.draftReady(syntheticDraft(service.requests.last(), 3));
    emit grid->selectionChanged(QSet<QString>{controller->workingDocument()->objects().first().id});
    y->setValue(2); applyProperties->click();
    const auto expertHash = lmsc::refinementBaselineHash(controller->workingDocument()->objects());
    QVERIFY(expertHash != lmsc::refinementBaselineHash(controller->initialDocument()->objects()));
    const auto selectDifficulty = [&](const QString &name) {
        for (int i = 0; i < difficulties->count(); ++i)
            if (difficulties->item(i)->data(Qt::UserRole + 1).toString() == name) {
                difficulties->setCurrentRow(i); return true;
            }
        return false;
    };
    for (const auto &actionName : QStringList{QStringLiteral("editorRestoreInitial"), QStringLiteral("editorCandidateDiscard")}) {
        QVERIFY(selectDifficulty(QStringLiteral("Hard")));
        QCOMPARE(controller->currentTargetKey(), QStringLiteral("Hard"));
        auto action = window.findChild<QPushButton *>(actionName); QVERIFY(action && action->isEnabled());
        action->click(); QVERIFY(prompt->isVisible());
        QVERIFY(selectDifficulty(QStringLiteral("Expert")));
        QCOMPARE(controller->currentTargetKey(), QStringLiteral("Expert"));
        proceed->click();
        QVERIFY(!prompt->isVisible());
        QCOMPARE(controller->targetKeys().size(), 2);
        QCOMPARE(lmsc::refinementBaselineHash(controller->workingDocument()->objects()), expertHash);
        QVERIFY(selectDifficulty(QStringLiteral("Hard")));
        QCOMPARE(lmsc::refinementBaselineHash(controller->workingDocument()->objects()), hardHash);
        QCOMPARE(lmsc::refinementBaselineHash(controller->formalDocument()->objects()), formalHash);
    }
    QVERIFY(!window.findChild<lmsc::GenerationPreviewDialog *>()); window.close();
}

void MainWindowTest::importHeadsetThroughDialog() {
    if (!qEnvironmentVariableIsSet("LMSC_TEST_HEADSET"))
        QSKIP("实机检查通过 LMSC_TEST_HEADSET=1 显式启用");
    MainWindow window(nullptr, settingsFile());
    window.setTestMode(true);
    window.show();
    auto service = window.findChild<MtpImportService *>();
    QVERIFY(service);
    QSignalSpy loaded(&window, &MainWindow::documentReady);
    QSignalSpy failed(&window, &MainWindow::loadFailed);
    bool found = false;
    connect(service, &MtpImportService::songsListed, &window, [&](const QVector<MtpSongEntry> &) {
        QTimer::singleShot(0, &window, [&] {
            auto dialog = window.findChild<QDialog *>("songImportDialog");
            if (!dialog) return;
            auto list = dialog->findChild<QTreeWidget *>("mtpSongTree");
            auto import = dialog->findChild<QPushButton *>("importHeadsetSong");
            if (list && import) {
                for (QTreeWidgetItemIterator iterator(list); *iterator; ++iterator) {
                    if (!(*iterator)->data(0, Qt::UserRole).isValid() || !(*iterator)->text(0).contains("Dry Hands", Qt::CaseInsensitive)) continue;
                    list->setCurrentItem(*iterator);
                    found = true;
                    dialog->grab().save(QString::fromUtf8(LMSC_AUDIO_FIXTURES_DIR) + "/pico-import.png");
                    import->click();
                    return;
                }
            }
            dialog->reject();
        });
    });
    QTimer timeout;
    timeout.setSingleShot(true);
    connect(&timeout, &QTimer::timeout, &window, [&] {
        if (auto dialog = window.findChild<QDialog *>("songImportDialog")) dialog->reject();
    });
    timeout.start(60000);
    QTimer::singleShot(0, &window, [&] { if (auto dialog = window.findChild<SongImportDialog *>("songImportDialog")) dialog->findChild<QTabWidget *>("songImportTabs")->setCurrentIndex(1); });
    auto import = button(window, QStringLiteral("导入歌曲"));
    QVERIFY(import);
    import->click();
    timeout.stop();
    QVERIFY2(found, "在连接的头显中找不到 Dry Hands");
    QTRY_COMPARE_WITH_TIMEOUT(loaded.count(), 1, 60000);
    QCOMPARE(failed.count(), 0);
    QTRY_VERIFY_WITH_TIMEOUT(window.isAudioReady(), 60000);
    QVERIFY(window.windowTitle().contains("Dry Hands", Qt::CaseInsensitive));
    QVERIFY(objectCount(window) > 0);
    window.grab().save(QString::fromUtf8(LMSC_AUDIO_FIXTURES_DIR) + "/pico-editor.png");
    window.close();
}

void MainWindowTest::unifiedImportAndExportDialogs() {
    MainWindow window(nullptr, settingsFile()); window.setTestMode(true);
    QAction *importAction = nullptr, *projectAction = nullptr;
    int importActions = 0;
    for (auto action : window.findChildren<QAction *>()) {
        if (action->text() == QStringLiteral("导入歌曲")) { importAction = action; ++importActions; }
        if (action->text() == QStringLiteral("打开编辑工程")) projectAction = action;
        QVERIFY(action->text() != QStringLiteral("打开曲谱 ZIP"));
    }
    QCOMPARE(importActions, 1); QVERIFY(importAction && projectAction);
    QCOMPARE(importAction->shortcut(), QKeySequence(QKeySequence::Open));
    MtpImportService importer;
    importer.setScriptPath(m_temp.filePath("missing-import.ps1"));
    SongImportDialog dialog(&importer);
    dialog.show();
    auto localButton = dialog.findChild<QPushButton *>("importComputerSong");
    auto deviceButton = dialog.findChild<QPushButton *>("importHeadsetSong");
    auto deleteButton = dialog.findChild<QPushButton *>("deleteHeadsetSong");
    auto tabs = dialog.findChild<QTabWidget *>("songImportTabs");
    auto tree = dialog.findChild<QTreeWidget *>("mtpSongTree");
    QVERIFY(localButton && deviceButton && deleteButton && tabs && tree);
    dialog.setLocalPath(m_song);
    QVERIFY(localButton->isEnabled());
    dialog.setLocalPath(m_temp.path());
    QVERIFY(!localButton->isEnabled());
    const QString zip = m_temp.filePath("SyntheticChart.zip");
    QFile zipFile(zip); QVERIFY(zipFile.open(QIODevice::WriteOnly)); zipFile.write("PK"); zipFile.close();
    QMimeData mime; mime.setUrls({QUrl::fromLocalFile(zip)});
    QDragEnterEvent drag(QPoint(12, 12), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&dialog, &drag); QVERIFY(drag.isAccepted());
    QDropEvent drop(QPointF(12, 12), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&dialog, &drop);
    QVERIFY(localButton->isEnabled()); QCOMPARE(dialog.findChild<QLineEdit *>("localSongPath")->text(), QDir::toNativeSeparators(zip));
    tabs->setCurrentIndex(1);
    MtpSongEntry root; root.name = QStringLiteral("星穹绿洲"); root.gameName = root.name; root.gameId = "oasis";
    root.deviceName = "PICO Neo 3"; root.storageName = QStringLiteral("内部共享存储空间"); root.isSong = false;
    root.location = QStringLiteral("内部共享存储空间/SoulTopia/BeatNote/Custom");
    MtpSongEntry song = root; song.name = QStringLiteral("霓虹节拍（示例）"); song.isSong = true;
    song.categorySegments = QStringList{QStringLiteral("光剑曲谱制作"), QStringLiteral("电子音乐")};
    MtpSongEntry other = song; other.gameName = QStringLiteral("光之乐团"); other.gameId = "lightband";
    other.categorySegments = QStringList{QStringLiteral("练习曲")};
    MtpSongEntry empty = root; empty.gameName = QStringLiteral("光之乐团"); empty.gameId = "lightband";
    dialog.populateSongs({root, song, empty, other});
    QCOMPARE(tree->topLevelItemCount(), 1); QCOMPARE(tree->topLevelItem(0)->childCount(), 2);
    tree->setCurrentItem(tree->topLevelItem(0)); QVERIFY(!deviceButton->isEnabled()); QVERIFY(!deleteButton->isEnabled());
    tree->setCurrentItem(tree->topLevelItem(0)->child(0)); QVERIFY(!deleteButton->isEnabled());
    QTreeWidgetItem *leaf = nullptr;
    for (QTreeWidgetItemIterator iterator(tree); *iterator; ++iterator)
        if ((*iterator)->data(0, Qt::UserRole).isValid()) { leaf = *iterator; break; }
    QVERIFY(leaf); tree->setCurrentItem(leaf); QVERIFY(deviceButton->isEnabled()); QVERIFY(deleteButton->isEnabled());
    auto search = dialog.findChild<QLineEdit *>("mtpSongSearch");
    search->setText(QStringLiteral("不存在的歌曲")); QVERIFY(tree->topLevelItem(0)->isHidden()); QVERIFY(!deviceButton->isEnabled()); QVERIFY(!deleteButton->isEnabled());
    search->setText(QStringLiteral("电子音乐")); QVERIFY(!tree->topLevelItem(0)->isHidden());
    search->clear();

    MtpExportService exporter; exporter.setHelperPath(m_temp.filePath("missing-export.exe"));
    SongExportDialog exportDialog(&exporter, QStringLiteral("霓虹节拍（示例）"));
    exportDialog.show(); QTest::qWait(30);
    exportDialog.findChild<QLineEdit *>("songExportParentDirectory")->setText(m_temp.path());
    auto mode = exportDialog.findChild<QComboBox *>("songExportMode");
    auto exportButton = exportDialog.findChild<QPushButton *>("confirmSongExport");
    QVERIFY(exportButton->isEnabled()); mode->setCurrentIndex(1); QVERIFY(!exportButton->isEnabled());
    MtpExportDestination destination; destination.deviceId = "synthetic-device"; destination.storageId = "synthetic-storage"; destination.rootId = "synthetic-root";
    destination.deviceName = "PICO Neo 3"; destination.storageName = QStringLiteral("内部共享存储空间"); destination.gameName = QStringLiteral("星穹绿洲"); destination.gameId = "oasis";
    auto destination2 = destination; destination2.rootId = "synthetic-root-2"; destination2.gameName = QStringLiteral("光之乐团"); destination2.gameId = "lightband";
    exportDialog.populateDestinations({destination, destination2});
    QVERIFY(exportButton->isEnabled()); exportDialog.findChild<QComboBox *>("songExportDestinations")->setCurrentIndex(1);
    QCOMPARE(exportDialog.destination().gameId, QString("lightband"));
    QCOMPARE(exportDialog.parentDirectory(), QFileInfo(m_temp.path()).absoluteFilePath());
    exportDialog.findChild<QLineEdit *>("songExportParentDirectory")->setText(m_temp.filePath("missing-folder")); QVERIFY(!exportButton->isEnabled());
    exportDialog.findChild<QLineEdit *>("songExportParentDirectory")->setText(m_temp.path());
    const QString captureDirectory = qEnvironmentVariable("LMSC_CAPTURE_IMPORT_EXPORT");
    if (!captureDirectory.isEmpty()) {
        QVERIFY(QDir().mkpath(captureDirectory));
        for (const auto &theme : {QStringLiteral("dark"), QStringLiteral("light")}) {
            lmsc::ThemeManager::apply(theme); dialog.resize(880, 610); exportDialog.resize(760, 560); QTest::qWait(100);
            QVERIFY(dialog.grab().save(QDir(captureDirectory).filePath("import-" + theme + ".png")));
            // A drive root illustrates the selection without exposing a user directory.
            exportDialog.findChild<QLineEdit *>("songExportParentDirectory")->setText(QDir::drives().first().absoluteFilePath());
            QVERIFY(exportDialog.grab().save(QDir(captureDirectory).filePath("export-" + theme + ".png")));
        }
    }
    dialog.resize(600, 430); exportDialog.resize(540, 400); QTest::qWait(10);
    QCOMPARE(dialog.size(), QSize(600, 430));
    QVERIFY(dialog.findChild<QPushButton *>("importHeadsetSong")->isVisible());
    QVERIFY(deleteButton->isVisible());
    QVERIFY(exportDialog.findChild<QPushButton *>("confirmSongExport")->isVisible());
    exportDialog.close(); dialog.close(); lmsc::ThemeManager::apply(QStringLiteral("dark"));
    SongImportDialog localSelection(&importer);
    localSelection.setLocalPath(zip);
    localSelection.findChild<QPushButton *>("importComputerSong")->click();
    QCOMPARE(localSelection.result(), int(QDialog::Accepted)); QCOMPARE(localSelection.localPath(), zip);
    QVERIFY(!localSelection.fromDevice());
}

void MainWindowTest::deleteHeadsetSongThroughDialog_data() {
    QTest::addColumn<QString>("resultMode");
    QTest::newRow("cancel-default-confirmation") << QStringLiteral("confirmation-cancel");
    QTest::newRow("safe-cancel-preparation") << QStringLiteral("prepare-cancel");
    QTest::newRow("preparation-failure-requires-refresh") << QStringLiteral("prepare-failure");
    QTest::newRow("verified-deletion-and-refresh") << QStringLiteral("success");
    QTest::newRow("partial-failure-requires-refresh") << QStringLiteral("delete-failure");
}

void MainWindowTest::deleteHeadsetSongThroughDialog() {
    QFETCH(QString, resultMode);
    const QString captureDirectory = qEnvironmentVariable("LMSC_CAPTURE_DEVICE_DELETE");
    if (!captureDirectory.isEmpty()) QVERIFY(QDir().mkpath(captureDirectory));
    const QString work = m_temp.filePath("mtp-delete-ui-" + resultMode);
    QVERIFY(QDir().mkpath(work));
    const QString callsPath = QDir(work).filePath("calls.txt");
    const QString deleteHelper = QDir(work).filePath("mock-delete.ps1");
    QFile script(deleteHelper); QVERIFY(script.open(QIODevice::WriteOnly));
    QString body = QStringLiteral(
        "param([string]$JobPath)\n$ErrorActionPreference='Stop'\n[Console]::OutputEncoding=New-Object Text.UTF8Encoding($false)\n"
        "$job=[IO.File]::ReadAllText($JobPath)|ConvertFrom-Json\n"
        "function Send($v){[Console]::WriteLine(($v|ConvertTo-Json -Depth 12 -Compress))}\n"
        "[IO.File]::AppendAllText((Join-Path $PSScriptRoot 'calls.txt'),$job.mode+[Environment]::NewLine)\n"
        "if($job.mode -eq 'delete-prepare'){\n");
    if (resultMode == "prepare-failure")
        body += QStringLiteral("Send @{type='error';message='模拟歌曲已被替换';deletionStarted=$false};exit 1\n");
    else if (resultMode == "prepare-cancel")
        body += QStringLiteral("Send @{type='progress';message='正在只读检查模拟歌曲';percent=0};while(-not [IO.File]::Exists($job.cancelFile)){Start-Sleep -Milliseconds 20};Send @{type='cancelled'};exit 2\n");
    else
        body += QStringLiteral("[IO.File]::WriteAllText($job.planFile,'{}');Send @{type='prepared';locator=$job.locator;files=3;bytes=123;planToken=('a'*64)};exit 0\n");
    body += QStringLiteral("}\nSend @{type='deleting';locator=$job.locator}\nSend @{type='progress';message='正在删除模拟歌曲';percent=0}\nStart-Sleep -Milliseconds 350\n");
    if (resultMode == "success")
        body += QStringLiteral("Send @{type='deleted';locator=$job.locator;verified=$true};exit 0\n");
    else
        body += QStringLiteral("Send @{type='error';message='模拟删除期间断开连接';deletionStarted=$true};exit 1\n");
    script.write(QByteArray::fromHex("efbbbf") + body.toUtf8()); script.close();

    const QString importHelper = QDir(work).filePath("mock-list.ps1");
    QFile listScript(importHelper); QVERIFY(listScript.open(QIODevice::WriteOnly));
    listScript.write(QByteArray::fromHex("efbbbf") + QStringLiteral(
        "$ErrorActionPreference='Stop'\n[Console]::OutputEncoding=New-Object Text.UTF8Encoding($false)\n"
        "[IO.File]::AppendAllText((Join-Path $PSScriptRoot 'calls.txt'),'list'+[Environment]::NewLine)\n"
        "[Console]::WriteLine((@{type='songs';songs=@(@{name='星穹绿洲';deviceName='PICO Neo 3';storageName='内部共享存储空间';gameId='oasis';gameName='星穹绿洲';isSong=$false;categorySegments=@();location='内部共享存储空间/SoulTopia/BeatNote/Custom';locator=@{device='PICO Neo 3';segments=@('内部共享存储空间','SoulTopia','BeatNote','Custom')}})}|ConvertTo-Json -Depth 12 -Compress))\n").toUtf8());
    listScript.close();
    MtpImportService importer; importer.setScriptPath(importHelper);
    SongImportDialog dialog(&importer); dialog.setLocalPath(m_song);
    auto deleteService = dialog.deleteService(); deleteService->setHelperPath(deleteHelper);
    QSignalSpy deleted(deleteService, &MtpDeleteService::songDeleted);
    QSignalSpy failed(deleteService, &MtpDeleteService::errorOccurred);
    QSignalSpy cancelled(deleteService, &MtpDeleteService::cancelled);
    QSignalSpy refreshed(&importer, &MtpImportService::songsListed);
    MtpSongEntry root; root.isSong = false; root.deviceName = "PICO Neo 3";
    root.gameId = "oasis"; root.gameName = QStringLiteral("星穹绿洲"); root.storageName = QStringLiteral("内部共享存储空间");
    MtpSongEntry song = root; song.isSong = true;
    song.name = QStringLiteral("合成歌曲 <b>| $(abc) %5"); song.categorySegments = QStringList{QStringLiteral("测试分类")};
    song.location = root.storageName + QStringLiteral("/SoulTopia/BeatNote/Custom/测试分类/") + song.name;
    song.locator = QString::fromUtf8(QJsonDocument(QJsonObject{{"device", root.deviceName}, {"segments", QJsonArray{
        root.storageName, "SoulTopia", "BeatNote", "Custom", QStringLiteral("测试分类"), song.name}}}).toJson(QJsonDocument::Compact));
    dialog.populateSongs({root, song}); dialog.show();
    auto tabs = dialog.findChild<QTabWidget *>("songImportTabs"); tabs->setCurrentIndex(1);
    auto tree = dialog.findChild<QTreeWidget *>("mtpSongTree");
    auto deleteButton = dialog.findChild<QPushButton *>("deleteHeadsetSong");
    auto importButton = dialog.findChild<QPushButton *>("importHeadsetSong");
    auto refreshButton = dialog.findChild<QPushButton *>("refreshHeadsetSongs");
    auto localButton = dialog.findChild<QPushButton *>("importComputerSong");
    auto cancelButton = dialog.findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Cancel);
    auto status = dialog.findChild<QLabel *>("mtpImportStatus");
    QVERIFY(tree && deleteButton && importButton && refreshButton && localButton && cancelButton && status);
    for (QTreeWidgetItemIterator iterator(tree); *iterator; ++iterator)
        if ((*iterator)->data(0, Qt::UserRole).isValid()) { tree->setCurrentItem(*iterator); break; }
    QVERIFY(deleteButton->isEnabled());
    if (!captureDirectory.isEmpty() && resultMode == "success") {
        for (const auto &theme : {QStringLiteral("dark"), QStringLiteral("light")}) {
            lmsc::ThemeManager::apply(theme); dialog.resize(880, 610); QTest::qWait(30);
            QVERIFY(dialog.grab().save(QDir(captureDirectory).filePath("device-browser-" + theme + ".png")));
            dialog.resize(600, 430); QTest::qWait(30);
            QCOMPARE(dialog.size(), QSize(600, 430));
            QVERIFY(deleteButton->isVisible());
            QVERIFY(dialog.rect().contains(QRect(deleteButton->mapTo(&dialog, QPoint()), deleteButton->size())));
            QVERIFY(dialog.grab().save(QDir(captureDirectory).filePath("device-browser-" + theme + "-minimum.png")));
        }
        lmsc::ThemeManager::apply(QStringLiteral("dark"));
    }
    int phase = 0; bool inspectedConfirmation = false, guardedCommit = false;
    QTimer driver; driver.setInterval(10);
    connect(&driver, &QTimer::timeout, &dialog, [&] {
        if (resultMode == "prepare-cancel" && phase == 0 && QFileInfo::exists(callsPath) && deleteService->isBusy()) {
            QVERIFY(cancelButton->isEnabled()); phase = 1; dialog.reject(); return;
        }
        auto confirmation = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
        if (phase == 0 && confirmation && confirmation->objectName() == QStringLiteral("confirmHeadsetSongDeletion")) {
            inspectedConfirmation = true; phase = 1;
            QCOMPARE(confirmation->textFormat(), Qt::PlainText);
            QVERIFY(confirmation->text().contains(song.name));
            QVERIFY(confirmation->text().contains(QStringLiteral("此电脑\\") + song.deviceName + QLatin1Char('\\') + QDir::toNativeSeparators(song.location)));
            QVERIFY(confirmation->text().contains(QStringLiteral("文件：3 个")));
            QCOMPARE(confirmation->defaultButton(), qobject_cast<QPushButton *>(confirmation->button(QMessageBox::Cancel)));
            QCOMPARE(confirmation->button(QMessageBox::Cancel)->text(), QStringLiteral("取消"));
            if (!captureDirectory.isEmpty() && resultMode == "success") {
                for (const auto &theme : {QStringLiteral("dark"), QStringLiteral("light")}) {
                    lmsc::ThemeManager::apply(theme); QApplication::processEvents();
                    QVERIFY(confirmation->grab().save(QDir(captureDirectory).filePath("delete-confirmation-" + theme + ".png")));
                }
                lmsc::ThemeManager::apply(QStringLiteral("dark"));
            }
            if (resultMode == "confirmation-cancel") QTest::keyClick(confirmation, Qt::Key_Escape);
            else { phase = 2; confirmation->findChild<QPushButton *>("confirmDeleteHeadsetSong")->click(); }
        } else if (phase == 2 && deleteService->isBusy()) {
            QVERIFY(!deleteButton->isEnabled()); QVERIFY(!importButton->isEnabled()); QVERIFY(!refreshButton->isEnabled());
            QVERIFY(!localButton->isEnabled()); QVERIFY(!cancelButton->isEnabled()); QVERIFY(!tree->isEnabled());
            dialog.reject(); QVERIFY(dialog.isVisible()); dialog.done(QDialog::Accepted); QVERIFY(dialog.isVisible());
            dialog.accept(); QVERIFY(dialog.isVisible()); QTest::keyClick(&dialog, Qt::Key_Escape); QVERIFY(dialog.isVisible());
            QVERIFY(!dialog.close()); QVERIFY(dialog.isVisible());
            QMimeData mime; mime.setUrls({QUrl::fromLocalFile(m_song)});
            QDragEnterEvent drag(QPoint(12, 12), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(&dialog, &drag); QVERIFY(!drag.isAccepted());
            QDropEvent drop(QPointF(12, 12), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(&dialog, &drop); QVERIFY(!drop.isAccepted()); QCOMPARE(tabs->currentIndex(), 1);
            const QString zip = QDir(work).filePath("guard.zip");
            QFile zipFile(zip); QVERIFY(zipFile.open(QIODevice::WriteOnly)); zipFile.write("PK"); zipFile.close();
            dialog.setLocalPath(zip); QVERIFY(!localButton->isEnabled()); localButton->click(); QVERIFY(dialog.isVisible());
            auto localTree = dialog.findChild<QTreeView *>("localSongTree");
            auto files = qobject_cast<QFileSystemModel *>(localTree->model()); QVERIFY(files);
            QVERIFY(QMetaObject::invokeMethod(localTree, "doubleClicked", Qt::DirectConnection, Q_ARG(QModelIndex, files->index(zip))));
            QVERIFY(dialog.isVisible()); guardedCommit = true; phase = 3;
        }
    });
    driver.start(); deleteButton->click();
    if (resultMode == "confirmation-cancel") {
        QTRY_VERIFY_WITH_TIMEOUT(inspectedConfirmation, 10000);
        QTRY_VERIFY_WITH_TIMEOUT(!deleteService->isBusy() && deleteButton->isEnabled(), 10000);
        QCOMPARE(deleted.count(), 0); QCOMPARE(failed.count(), 0); QCOMPARE(refreshed.count(), 0); QVERIFY(dialog.isVisible());
    } else if (resultMode == "prepare-cancel") {
        QTRY_COMPARE_WITH_TIMEOUT(cancelled.count(), 1, 10000);
        QVERIFY(!dialog.isVisible()); QVERIFY(!inspectedConfirmation); QCOMPARE(deleted.count(), 0);
    } else if (resultMode == "success") {
        QTRY_COMPARE_WITH_TIMEOUT(deleted.count(), 1, 10000);
        QTRY_COMPARE_WITH_TIMEOUT(refreshed.count(), 1, 10000);
        QVERIFY(inspectedConfirmation && guardedCommit); QCOMPARE(failed.count(), 0);
        QVERIFY(status->text().contains(QStringLiteral("已删除头显歌曲"))); QVERIFY(!deleteButton->isEnabled());
    } else {
        QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 1, 10000);
        QCOMPARE(deleted.count(), 0); QCOMPARE(refreshed.count(), 0); QCOMPARE(tree->topLevelItemCount(), 0);
        QVERIFY(refreshButton->isEnabled()); QVERIFY(!deleteButton->isEnabled()); QVERIFY(!importButton->isEnabled());
        QVERIFY(status->text().contains(QStringLiteral("重新刷新")));
        if (resultMode == "delete-failure") { QVERIFY(inspectedConfirmation && guardedCommit); QVERIFY(status->text().contains(QStringLiteral("可能残留部分内容"))); }
        else QVERIFY(!inspectedConfirmation);
    }
    driver.stop(); QVERIFY(!deleteService->isBusy());
    QFile calls(callsPath); QVERIFY(calls.open(QIODevice::ReadOnly));
    const QStringList operations = QString::fromUtf8(calls.readAll()).split(QRegularExpression("[\\r\\n]+"), QString::SkipEmptyParts);
    QCOMPARE(operations.count(QStringLiteral("delete-prepare")), 1);
    QCOMPARE(operations.count(QStringLiteral("delete")), (resultMode == "success" || resultMode == "delete-failure") ? 1 : 0);
    QCOMPARE(operations.count(QStringLiteral("list")), resultMode == "success" ? 1 : 0);
    dialog.close();
}

void MainWindowTest::deviceExportThroughDialog_data() {
    QTest::addColumn<QString>("resultMode");
    QTest::newRow("verified-upload") << QStringLiteral("success");
    QTest::newRow("checksum-failure") << QStringLiteral("failure");
    QTest::newRow("cancel-retains-copy") << QStringLiteral("cancel");
}

void MainWindowTest::deviceExportThroughDialog() {
    QFETCH(QString, resultMode);
    MainWindow window(nullptr, settingsFile()); window.setTestMode(true); window.show();
    QSignalSpy ready(&window, &MainWindow::documentReady);
    window.openPath(m_song); QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, 20000);
    QTRY_VERIFY_WITH_TIMEOUT(window.isAudioReady(), 20000);
    const QString originalTitle = window.windowTitle();
    QFile originalInfo(m_song + "/Info.dat"); QVERIFY(originalInfo.open(QIODevice::ReadOnly));
    const QByteArray infoHash = QCryptographicHash::hash(originalInfo.readAll(), QCryptographicHash::Sha256); originalInfo.close();
    HomepageUrlReceiver folderReceiver; ScopedFileUrlHandler folderHandler(&folderReceiver);
    auto exporter = window.findChild<MtpExportService *>(); QVERIFY(exporter);
    const QString parent = m_temp.filePath("device-export-" + resultMode); QVERIFY(QDir().mkpath(parent));
    const QString helper = QDir(parent).filePath("mock-device.ps1");
    QFile script(helper); QVERIFY(script.open(QIODevice::WriteOnly));
    QString body = QStringLiteral(
        "param([string]$JobPath)\n$ErrorActionPreference='Stop'\n[Console]::OutputEncoding=New-Object Text.UTF8Encoding($false)\n"
        "$job=[IO.File]::ReadAllText($JobPath)|ConvertFrom-Json\n"
        "function Send($v){[Console]::WriteLine(($v|ConvertTo-Json -Depth 12 -Compress))}\n"
        "if($job.mode -eq 'list'){Send @{type='destinations';destinations=@(@{deviceId='synthetic';storageId='storage';rootId='root';deviceName='PICO Neo 3';storageName='内部共享存储空间';gameId='lightband';gameName='光之乐团'})};exit 0}\n"
        "Send @{type='created';location='PICO Neo 3/光之乐团/光剑曲谱制作/本次新歌曲'}\n"
        "Send @{type='progress';message='校验模拟设备歌曲';percent=0}\n");
    if (resultMode == "success") body += QStringLiteral("Send @{type='uploaded';localFolder=$job.localFolder;location='PICO Neo 3/光之乐团/光剑曲谱制作/本次新歌曲';verified=$true}\n");
    else if (resultMode == "failure") body += QStringLiteral("Send @{type='error';message='SHA256 回读校验不一致'}\nexit 1\n");
    else body += QStringLiteral("while(-not [IO.File]::Exists($job.cancelFile)){Start-Sleep -Milliseconds 30};Send @{type='cancelled';location='PICO Neo 3/光之乐团/光剑曲谱制作/本次新歌曲'};exit 2\n");
    script.write(QByteArray::fromHex("efbbbf") + body.toUtf8()); script.close(); exporter->setHelperPath(helper);
    QSignalSpy uploaded(exporter, &MtpExportService::songUploaded);
    QSignalSpy failed(exporter, &MtpExportService::errorOccurred);
    QSignalSpy cancelled(exporter, &MtpExportService::cancelled);
    if (resultMode == "cancel") connect(exporter, &MtpExportService::taskProgress, &window, [&](const QString &, int percent) {
        if (percent != 0) return;
        QTimer::singleShot(0, &window, [&] {
            if (auto progress = window.findChild<QProgressDialog *>())
                for (auto button : progress->findChildren<QPushButton *>()) if (button->text() == QStringLiteral("取消上传")) button->click();
        });
    });
    int phase = 0; QTimer driver;
    connect(&driver, &QTimer::timeout, &window, [&] {
        if (phase == 0) {
            auto dialog = qobject_cast<SongExportDialog *>(QApplication::activeModalWidget());
            if (!dialog || exporter->isBusy() || dialog->findChild<QComboBox *>("songExportDestinations")->count() == 0) return;
            dialog->findChild<QComboBox *>("songExportMode")->setCurrentIndex(1);
            dialog->findChild<QLineEdit *>("songExportParentDirectory")->setText(parent);
            phase = 1; dialog->findChild<QPushButton *>("confirmSongExport")->click();
        } else if (phase == 1 && resultMode == "success") {
            auto message = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
            if (!message || message->windowTitle() != QStringLiteral("导出完成")) return;
            QVERIFY(message->text().contains(QStringLiteral("设备传输与回读校验已完成")));
            QVERIFY(message->text().contains(QStringLiteral("本次新歌曲"))); phase = 2;
            auto open = button(*message, QStringLiteral("打开歌曲目录")); QVERIFY(open); open->click();
        }
    });
    QAction *action = nullptr; for (auto candidate : window.findChildren<QAction *>()) if (candidate->text() == QStringLiteral("导出歌曲")) action = candidate;
    QVERIFY(action); driver.start(20); action->trigger(); driver.stop();
    QVERIFY(phase >= 1); QVERIFY(action->isEnabled()); QVERIFY(!exporter->isBusy());
    const QString localFolder = QDir(parent).filePath(QStringLiteral("光剑曲谱制作/GUI Fixture-by光剑曲谱"));
    QVERIFY(QFileInfo::exists(localFolder + "/Info.dat"));
    lmsc::BeatmapDocument copy; QString error; QVERIFY2(copy.loadSong(localFolder, &error), qPrintable(error));
    QCOMPARE(copy.difficulties().size(), 2); QCOMPARE(window.windowTitle(), originalTitle);
    QVERIFY(originalInfo.open(QIODevice::ReadOnly)); QCOMPARE(QCryptographicHash::hash(originalInfo.readAll(), QCryptographicHash::Sha256), infoHash); originalInfo.close();
    if (resultMode == "success") { QCOMPARE(phase, 2); QCOMPARE(uploaded.count(), 1); QCOMPARE(failed.count(), 0); QCOMPARE(folderReceiver.openedCount, 1); QCOMPARE(folderReceiver.openedUrl, QUrl::fromLocalFile(localFolder)); }
    else {
        QCOMPARE(uploaded.count(), 0); QCOMPARE(failed.count(), 1);
        QVERIFY(window.statusBar()->currentMessage().contains(QStringLiteral("完整电脑副本仍保留")));
        QVERIFY(window.statusBar()->currentMessage().contains(QStringLiteral("本次新歌曲")));
        if (resultMode == "cancel") QCOMPARE(cancelled.count(), 1);
        else QVERIFY(window.statusBar()->currentMessage().contains("SHA256"));
    }
    window.close();
}

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

void MainWindowTest::chineseBrandIconAndAboutLicense() {
    MainWindow window(nullptr, settingsFile()); window.setTestMode(true); window.show();
    QCOMPARE(window.windowTitle(), QStringLiteral("光剑曲谱制作"));
    QVERIFY(!window.windowIcon().isNull());
    const QPixmap icon = window.windowIcon().pixmap(32, 32);
    QVERIFY(!icon.isNull());
    QCOMPARE(icon.size(), QSize(32, 32));

    QFile license(QStringLiteral(":/licenses/GPL-3.0.txt"));
    QVERIFY(license.open(QIODevice::ReadOnly));
    const QString officialLicense = QString::fromUtf8(license.readAll());
    QVERIFY(officialLicense.contains(QStringLiteral("GNU GENERAL PUBLIC LICENSE")));
    QVERIFY(officialLicense.contains(QStringLiteral("Version 3, 29 June 2007")));
    QVERIFY(officialLicense.size() > 30000);
    QAction *aboutAction = nullptr;
    for (auto *action : window.findChildren<QAction *>())
        if (action->text() == QStringLiteral("关于光剑曲谱制作")) aboutAction = action;
    QVERIFY(aboutAction);

    aboutAction->trigger();
    auto nav = window.findChild<QListWidget *>(QStringLiteral("mainNavigation"));
    auto workspace = window.findChild<QStackedWidget *>(QStringLiteral("workspacePages"));
    auto pages = window.findChild<QStackedWidget *>(QStringLiteral("settingsPages"));
    auto version = window.findChild<QLabel *>(QStringLiteral("aboutVersion"));
    auto author = window.findChild<QLabel *>(QStringLiteral("aboutAuthor"));
    auto homepage = window.findChild<QPushButton *>(QStringLiteral("openProjectHomepage"));
    auto showLicense = window.findChild<QPushButton *>(QStringLiteral("showGplLicense"));
    auto displayedLicense = window.findChild<QTextBrowser *>(QStringLiteral("gplLicenseText"));
    QVERIFY(nav && workspace && pages && version && author && homepage && showLicense && displayedLicense);
    QCOMPARE(nav->currentRow(), 6);
    QCOMPARE(workspace->currentIndex(), 1);
    QCOMPARE(pages->currentIndex(), 4);
    QCOMPARE(version->text(), lmsc::AppInfo::version());
    QCOMPARE(author->text(), QStringLiteral("Vae-x"));
    QVERIFY(hasText(*pages->currentWidget(), QStringLiteral("GNU GPL 第 3 版")));
    QVERIFY(hasText(*pages->currentWidget(), QStringLiteral("第三方组件")));
    QVERIFY(!QApplication::activeModalWidget());
    QVERIFY(!displayedLicense->isVisible());
    QTest::mouseClick(showLicense, Qt::LeftButton);
    QVERIFY(displayedLicense->isVisible());
    QCOMPARE(displayedLicense->toPlainText().trimmed(), officialLicense.trimmed());
    QVERIFY(!QApplication::activeModalWidget());
    window.close();
}

void MainWindowTest::aboutHomepageOpensProject() {
    HomepageUrlReceiver receiver;
    ScopedHttpsUrlHandler handler(&receiver);
    MainWindow window(nullptr, settingsFile()); window.setTestMode(true); window.show();
    QAction *aboutAction = nullptr;
    for (auto *action : window.findChildren<QAction *>())
        if (action->text() == QStringLiteral("关于光剑曲谱制作")) aboutAction = action;
    QVERIFY(aboutAction);

    aboutAction->trigger();
    auto homepage = window.findChild<QPushButton *>(QStringLiteral("openProjectHomepage"));
    QVERIFY(homepage);
    QVERIFY(homepage->isVisible());
    QTest::mouseClick(homepage, Qt::LeftButton);
    QCOMPARE(receiver.openedCount, 1);
    QCOMPARE(receiver.openedUrl, QUrl(lmsc::AppInfo::homepageUrl()));
    QVERIFY(window.isVisible());
    QVERIFY(!QApplication::activeModalWidget());
    window.close();
}

void MainWindowTest::clickPlaceApplyUndoAndDifficulty() {
    MainWindow window(nullptr, settingsFile()); window.setTestMode(true); window.show();
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
    auto *difficulties = window.findChild<QListWidget *>(QStringLiteral("difficultyList"));
    auto *newSongDifficulty = window.findChild<QComboBox *>(QStringLiteral("newSongDifficultySelector"));
    auto *exportLeadIn = window.findChild<QDoubleSpinBox *>(QStringLiteral("exportLeadInSeconds"));
    QVERIFY(difficulties && newSongDifficulty);
    QVERIFY(!newSongDifficulty->isVisible() || !newSongDifficulty->isEnabled());
    QVERIFY(exportLeadIn && (!exportLeadIn->isVisible() || !exportLeadIn->isEnabled()));
    QCOMPARE(difficulties->count(), 2);
    difficulties->setCurrentRow(1);
    QCOMPARE(objectCount(window), 1);
    QVERIFY(!apply->isEnabled());
    difficulties->setCurrentRow(0);
    QCOMPARE(objectCount(window), 4);
    window.close();
}

void MainWindowTest::protectedSelectionRejectsEntireDrag() {
    MainWindow window(nullptr, settingsFile()); window.setTestMode(true); window.show();
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
    QTest::addColumn<QString>("difficultyName");
    QTest::addColumn<int>("difficultyRank");
    QTest::newRow("MP3-Easy") << QStringLiteral("clicks.mp3") << 0 << false << QStringLiteral("Easy") << 1;
    QTest::newRow("MP3-Normal") << QStringLiteral("clicks.mp3") << 0 << false << QStringLiteral("Normal") << 3;
    QTest::newRow("MP3-Hard") << QStringLiteral("clicks.mp3") << 0 << false << QStringLiteral("Hard") << 5;
    QTest::newRow("MP3-Expert") << QStringLiteral("clicks.mp3") << 0 << false << QStringLiteral("Expert") << 7;
    QTest::newRow("MP4-second-audio-ExpertPlus") << QStringLiteral("two-tracks.mp4") << 2 << true
                                              << QStringLiteral("ExpertPlus") << 9;
}

void MainWindowTest::importMp3AndCropThroughDialogs() {
    QFETCH(QString, mediaName);
    QFETCH(int, streamIndex);
    QFETCH(bool, previewBeforeConvert);
    QFETCH(QString, difficultyName);
    QFETCH(int, difficultyRank);
    const QStringList difficultyNames{QStringLiteral("Easy"), QStringLiteral("Normal"), QStringLiteral("Hard"),
                                      QStringLiteral("Expert"), QStringLiteral("ExpertPlus")};
    const QVector<int> difficultyRanks{1, 3, 5, 7, 9};
    const QString captureDirectory = qEnvironmentVariable("LMSC_DIFFICULTY_CAPTURE_DIRECTORY");
    if (!captureDirectory.isEmpty()) QVERIFY(QDir().mkpath(captureDirectory));
    const QString inputMedia = QString::fromUtf8(LMSC_AUDIO_FIXTURES_DIR) + "/" + mediaName;
    MainWindow window(nullptr, settingsFile()); window.setTestMode(true); window.show();
    QSignalSpy ready(&window, &MainWindow::documentReady);
    int phase = 0;
    bool configuredCrop = false;
    bool configuredDifficulty = false;
    bool capturedCreation = false;
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
            auto *difficulty = dialog->findChild<QComboBox *>(QStringLiteral("newSongDifficulty"));
            auto *buttons = dialog->findChild<QDialogButtonBox *>();
            if (!(title && start && end && track && difficulty && buttons)) { dialog->reject(); phase = 2; return; }
            if (field<QComboBox>(*dialog, QStringLiteral("曲谱难度")) != difficulty
                || difficulty->count() != difficultyNames.size()
                || difficulty->currentData().toString() != QStringLiteral("Expert")) {
                dialog->reject(); phase = 2; return;
            }
            for (int index = 0; index < difficultyNames.size(); ++index)
                if (difficulty->itemData(index).toString() != difficultyNames.at(index)
                    || difficulty->itemData(index, Qt::UserRole + 1).toInt() != difficultyRanks.at(index)) {
                    dialog->reject(); phase = 2; return;
                }
            const int difficultyChoice = difficulty->findData(difficultyName);
            if (difficultyChoice < 0) { dialog->reject(); phase = 2; return; }
            difficulty->setCurrentIndex(difficultyChoice);
            configuredDifficulty = difficulty->currentData().toString() == difficultyName
                && difficulty->currentData(Qt::UserRole + 1).toInt() == difficultyRank;
            const int trackChoice = track->findData(streamIndex);
            if (trackChoice < 0) { dialog->reject(); phase = 2; return; }
            track->setCurrentIndex(trackChoice);
            title->setText(QStringLiteral("GUI Crop")); start->setValue(1.0); end->setValue(4.0);
            configuredCrop = true; phase = 2; dialogDriver.stop();
            if (!captureDirectory.isEmpty() && difficultyName == QStringLiteral("Hard")) {
                QApplication::processEvents();
                capturedCreation = dialog->grab().save(QDir(captureDirectory).filePath("new-song-dialog-hard.png"));
            }
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
    QVERIFY(configuredDifficulty);
    QTRY_VERIFY_WITH_TIMEOUT(ready.count() == 1, 20000);
    QTRY_VERIFY_WITH_TIMEOUT(window.isAudioReady(), 20000);
    QVERIFY(window.windowTitle().contains(QStringLiteral("GUI Crop")));
    auto *difficulties = window.findChild<QListWidget *>(QStringLiteral("difficultyList"));
    auto *difficultySelector = window.findChild<QComboBox *>(QStringLiteral("newSongDifficultySelector"));
    auto *audio = window.findChild<AudioService *>();
    QVERIFY(difficulties && difficultySelector && audio);
    QCOMPARE(difficulties->count(), 1);
    QCOMPARE(difficulties->item(0)->text(), QStringLiteral("Standard · ") + difficultyName);
    QVERIFY(difficultySelector->isVisible() && difficultySelector->isEnabled());
    QCOMPARE(difficultySelector->count(), difficultyNames.size());
    QCOMPARE(difficultySelector->currentData().toString(), difficultyName);
    QCOMPARE(difficultySelector->currentData(Qt::UserRole + 1).toInt(), difficultyRank);
    QCOMPARE(objectCount(window), 0);
    QVERIFY(std::abs(window.findChild<AudioService *>()->duration() - 3.0) < 0.06);
    auto *rhythmStatus = window.findChild<QLabel *>(QStringLiteral("rhythmAnalysisStatus"));
    QVERIFY(rhythmStatus);
    const QRegularExpression rhythmResult(QStringLiteral(
        "^建议 [0-9]+(?:\\.[0-9]+)? BPM，第一拍 [0-9]+(?:\\.[0-9]+)? 秒\\n可信度 [0-9]+%"));
    QTRY_VERIFY_WITH_TIMEOUT(rhythmResult.match(rhythmStatus->text()).hasMatch(), 20000);
    if (previewBeforeConvert) QVERIFY(convertedDuringPreview);
    if (!captureDirectory.isEmpty() && difficultyName == QStringLiteral("Hard")) {
        QVERIFY(capturedCreation);
        QApplication::processEvents();
        QVERIFY(window.grab().save(QDir(captureDirectory).filePath("new-song-editor-hard.png")));
    }
    const QString output = QDir(m_temp.path()).filePath(mediaName + "-" + difficultyName + "-roundtrip");
    QString error;
    QVERIFY2(window.runEditorCheck(output, &error), qPrintable(error));
    lmsc::BeatmapDocument reopened;
    QVERIFY2(reopened.loadProject(QDir(output).filePath("smoke.lmsc"), &error), qPrintable(error));
    QCOMPARE(reopened.difficulties().size(), 1);
    QCOMPARE(reopened.difficulties().first().name, difficultyName);
    QCOMPARE(reopened.difficulties().first().rank, difficultyRank);
    QCOMPARE(reopened.difficulties().first().filename, QStringLiteral("Expert.dat"));
    QCOMPARE(reopened.objects().size(), 1);
    lmsc::BeatmapDocument exportedSong;
    QVERIFY2(exportedSong.loadSong(QDir(output).filePath("export"), &error), qPrintable(error));
    QCOMPARE(exportedSong.difficulties().size(), 1);
    QCOMPARE(exportedSong.difficulties().first().name, difficultyName);
    QCOMPARE(exportedSong.difficulties().first().rank, difficultyRank);
    QCOMPARE(exportedSong.difficulties().first().filename, difficultyName + QStringLiteral(".dat"));
    QCOMPARE(exportedSong.objects().size(), 1);
    const auto source = reopened.importSource();
    QVERIFY(source.isAvailable());
    QCOMPARE(source.streamIndex, streamIndex);
    QCOMPARE(source.startSeconds, 1.0);
    QCOMPARE(source.endSeconds, 4.0);
    QFile original(inputMedia), retained(source.path);
    QVERIFY(original.open(QIODevice::ReadOnly) && retained.open(QIODevice::ReadOnly));
    QCOMPARE(QCryptographicHash::hash(retained.readAll(), QCryptographicHash::Sha256),
             QCryptographicHash::hash(original.readAll(), QCryptographicHash::Sha256));
    original.close();
    retained.close();
    const auto exported = lmsc::ProjectStore::files(QDir(output).filePath("export"), &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    for (const auto &path : exported)
        QVERIFY(!path.endsWith(".mp3", Qt::CaseInsensitive) && !path.endsWith(".mp4", Qt::CaseInsensitive));

    const auto before = reopened.objects().first();
    const QString difficultyId = reopened.currentDifficultyId();
    QFile beforeAudio(reopened.audioPath());
    QVERIFY(beforeAudio.open(QIODevice::ReadOnly));
    const QByteArray audioHash = QCryptographicHash::hash(beforeAudio.readAll(), QCryptographicHash::Sha256);
    beforeAudio.close();
    audio->seek(0.75);
    const double audioPosition = audio->position();
    const double audioDuration = audio->duration();
    const int nextDifficulty = (difficultyNames.indexOf(difficultyName) + 1) % difficultyNames.size();
    difficultySelector->setCurrentIndex(nextDifficulty);
    QCOMPARE(difficulties->count(), 1);
    QCOMPARE(difficulties->item(0)->text(), QStringLiteral("Standard · ") + difficultyNames.at(nextDifficulty));
    QCOMPARE(difficulties->item(0)->data(Qt::UserRole).toString(), difficultyId);
    QCOMPARE(difficultySelector->currentData().toString(), difficultyNames.at(nextDifficulty));
    QCOMPARE(difficultySelector->currentData(Qt::UserRole + 1).toInt(), difficultyRanks.at(nextDifficulty));
    QCOMPARE(objectCount(window), 1);
    QVERIFY(window.isAudioReady());
    QCOMPARE(audio->duration(), audioDuration);
    QCOMPARE(audio->position(), audioPosition);
    QAction *undo = nullptr;
    QAction *redo = nullptr;
    QAction *save = nullptr;
    for (auto *action : window.findChildren<QAction *>()) {
        if (action->text() == QStringLiteral("撤销")) undo = action;
        if (action->text() == QStringLiteral("重做")) redo = action;
        if (action->text() == QStringLiteral("保存工程")) save = action;
    }
    QVERIFY(undo && redo && undo->isEnabled());
    undo->trigger();
    QCOMPARE(objectCount(window), 1);
    QCOMPARE(difficultySelector->currentData().toString(), difficultyName);
    QCOMPARE(difficulties->item(0)->text(), QStringLiteral("Standard · ") + difficultyName);
    QVERIFY(undo->isEnabled());
    undo->trigger();
    QCOMPARE(objectCount(window), 0);
    QCOMPARE(difficultySelector->currentData().toString(), difficultyName);
    QCOMPARE(difficulties->item(0)->text(), QStringLiteral("Standard · ") + difficultyName);
    QVERIFY(redo->isEnabled());
    redo->trigger();
    QCOMPARE(objectCount(window), 1);
    QCOMPARE(difficultySelector->currentData().toString(), difficultyName);
    QCOMPARE(difficulties->item(0)->text(), QStringLiteral("Standard · ") + difficultyName);
    QVERIFY(redo->isEnabled());
    redo->trigger();
    QCOMPARE(difficultySelector->currentData().toString(), difficultyNames.at(nextDifficulty));
    QCOMPARE(difficulties->item(0)->text(), QStringLiteral("Standard · ") + difficultyNames.at(nextDifficulty));
    auto *autosave = window.findChild<QTimer *>(QStringLiteral("documentAutosave"));
    QVERIFY(autosave);
    QVERIFY(QMetaObject::invokeMethod(autosave, "timeout", Qt::DirectConnection));
    const QString projectFile = QDir(output).filePath("smoke.lmsc");
    QVERIFY(lmsc::BeatmapDocument::hasRecovery(projectFile));
    lmsc::BeatmapDocument recovery;
    QVERIFY2(recovery.loadProject(lmsc::BeatmapDocument::recoveryPath(projectFile), &error), qPrintable(error));
    QCOMPARE(recovery.difficulties().size(), 1);
    QCOMPARE(recovery.difficulties().first().name, difficultyNames.at(nextDifficulty));
    QCOMPARE(recovery.difficulties().first().rank, difficultyRanks.at(nextDifficulty));
    QCOMPARE(recovery.currentDifficultyId(), difficultyId);
    QCOMPARE(recovery.objects().size(), 1);
    QVERIFY(save && save->isEnabled());
    save->trigger();
    QVERIFY(!lmsc::BeatmapDocument::hasRecovery(projectFile));
    lmsc::BeatmapDocument changed;
    QVERIFY2(changed.loadProject(QDir(output).filePath("smoke.lmsc"), &error), qPrintable(error));
    QCOMPARE(changed.difficulties().size(), 1);
    QCOMPARE(changed.difficulties().first().name, difficultyNames.at(nextDifficulty));
    QCOMPARE(changed.difficulties().first().rank, difficultyRanks.at(nextDifficulty));
    QCOMPARE(changed.currentDifficultyId(), difficultyId);
    QCOMPARE(changed.difficulties().first().filename, QStringLiteral("Expert.dat"));
    QCOMPARE(changed.objects().size(), 1);
    const auto after = changed.objects().first();
    QCOMPARE(after.id, before.id);
    QCOMPARE(static_cast<int>(after.kind), static_cast<int>(before.kind));
    QCOMPARE(after.beat, before.beat);
    QCOMPARE(after.x, before.x);
    QCOMPARE(after.y, before.y);
    QCOMPARE(after.color, before.color);
    QCOMPARE(after.direction, before.direction);
    QCOMPARE(changed.importSource().streamIndex, source.streamIndex);
    QCOMPARE(changed.importSource().startSeconds, source.startSeconds);
    QCOMPARE(changed.importSource().endSeconds, source.endSeconds);
    QFile afterAudio(changed.audioPath());
    QVERIFY(afterAudio.open(QIODevice::ReadOnly));
    QCOMPARE(QCryptographicHash::hash(afterAudio.readAll(), QCryptographicHash::Sha256), audioHash);
    afterAudio.close();
    const QString changedExport = QDir(output).filePath("changed-export");
    QVERIFY2(changed.exportSong(changedExport, &error), qPrintable(error));
    lmsc::BeatmapDocument changedExported;
    QVERIFY2(changedExported.loadSong(changedExport, &error), qPrintable(error));
    QCOMPARE(changedExported.difficulties().size(), 1);
    QCOMPARE(changedExported.difficulties().first().name, difficultyNames.at(nextDifficulty));
    QCOMPARE(changedExported.difficulties().first().rank, difficultyRanks.at(nextDifficulty));
    QCOMPARE(changedExported.difficulties().first().filename, difficultyNames.at(nextDifficulty) + QStringLiteral(".dat"));
    QCOMPARE(changedExported.objects().size(), 1);
    if (difficultyName == QStringLiteral("Hard")) {
        auto *leadIn = window.findChild<QDoubleSpinBox *>(QStringLiteral("exportLeadInSeconds"));
        QVERIFY(leadIn && leadIn->isVisible() && leadIn->isEnabled());
        QCOMPARE(leadIn->value(), 2.0);
        const QString exportParent = QDir(output).filePath(QStringLiteral("gui-export"));
        QVERIFY(QDir().mkpath(exportParent));
        int exportPhase = 0;
        bool bufferMessage = false;
        QString guiExportPath;
        QTimer exportDriver;
        connect(&exportDriver, &QTimer::timeout, &window, [&] {
            auto *modal = QApplication::activeModalWidget();
            if (exportPhase == 0) {
                auto *dialog = qobject_cast<SongExportDialog *>(modal);
                if (!dialog) return;
                dialog->findChild<QLineEdit *>("songExportParentDirectory")->setText(exportParent);
                exportPhase = 1;
                dialog->findChild<QPushButton *>("confirmSongExport")->click();
            } else if (exportPhase == 1) {
                auto *message = qobject_cast<QMessageBox *>(modal);
                if (!message || message->windowTitle() != QStringLiteral("导出完成")) return;
                bufferMessage = message->text().contains(QStringLiteral("2.000 秒开场静音"))
                    && message->text().contains(QStringLiteral("CustomMusic"));
                guiExportPath = QDir::fromNativeSeparators(message->text().section('\n', 1, 1));
                exportPhase = 2;
                exportDriver.stop();
                message->accept();
            }
        });
        QAction *exportAction = nullptr;
        for (auto *action : window.findChildren<QAction *>())
            if (action->text() == QStringLiteral("导出歌曲")) exportAction = action;
        QVERIFY(exportAction && exportAction->isEnabled());
        exportDriver.start(20);
        exportAction->trigger();
        exportDriver.stop();
        QCOMPARE(exportPhase, 2);
        QVERIFY(bufferMessage);
        QCOMPARE(QFileInfo(guiExportPath).absolutePath(), QDir(exportParent).filePath(QStringLiteral("光剑曲谱制作")));
        QVERIFY(QFileInfo(guiExportPath).fileName().endsWith(QStringLiteral("-by光剑曲谱")));
        lmsc::BeatmapDocument guiExport;
        QVERIFY2(guiExport.loadSong(guiExportPath, &error), qPrintable(error));
        QCOMPARE(guiExport.objects().size(), 1);
        QCOMPARE(guiExport.difficulties().first().name, difficultyNames.at(nextDifficulty));
        QVERIFY(std::abs(guiExport.timeMap().beatToSeconds(guiExport.objects().first().beat)
                         - changed.timeMap().beatToSeconds(changed.objects().first().beat) - 2.0) < 1e-8);
        QCOMPARE(audio->duration(), audioDuration);
        QCOMPARE(audio->position(), audioPosition);
        QVERIFY(!window.windowTitle().contains(QStringLiteral(" *")));
    }
    window.openPath(projectFile);
    QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 2, 20000);
    QTRY_VERIFY_WITH_TIMEOUT(window.isAudioReady(), 20000);
    QVERIFY(difficultySelector->isVisible() && difficultySelector->isEnabled());
    QCOMPARE(difficultySelector->currentData().toString(), difficultyNames.at(nextDifficulty));
    QCOMPARE(difficultySelector->currentData(Qt::UserRole + 1).toInt(), difficultyRanks.at(nextDifficulty));
    QCOMPARE(difficulties->count(), 1);
    QCOMPARE(objectCount(window), 1);
    window.close();
}

void MainWindowTest::importDryHands() {
    const QString sample = QString::fromUtf8(LMSC_REPOSITORY_DIR) + "/samples/beatmaps/2369d (Dry Hands - Darkrealm7).zip";
    if (!QFileInfo::exists(sample)) QSKIP("Local Dry Hands reference ZIP is unavailable");
    MainWindow window(nullptr, settingsFile()); window.setTestMode(true); window.show();
    QSignalSpy ready(&window, &MainWindow::documentReady);
    QSignalSpy failed(&window, &MainWindow::loadFailed);
    window.openPath(sample);
    QTRY_VERIFY_WITH_TIMEOUT(ready.count() == 1 || failed.count() > 0, 30000);
    QCOMPARE(failed.count(), 0);
    QTRY_VERIFY_WITH_TIMEOUT(window.isAudioReady(), 20000);
    QVERIFY(window.windowTitle().contains(QStringLiteral("Dry Hands")));
    QVERIFY(window.findChild<QListWidget *>(QStringLiteral("difficultyList"))->count() > 0);
    const int before = objectCount(window);
    QVERIFY(before > 0);
    auto *grid = window.findChild<GridEditor *>();
    QTest::mouseClick(grid, Qt::LeftButton, Qt::NoModifier, gridCell(*grid, 3, 2));
    QCOMPARE(objectCount(window), before + 1);
    window.close();
}

void MainWindowTest::trackFramebufferUsesLoadedObjects() {
    MainWindow window(nullptr, settingsFile()); window.setTestMode(true); window.show();
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

void MainWindowTest::captureWorkspace() {
    const QString directory = qEnvironmentVariable("LMSC_MAIN_CAPTURE_DIRECTORY");
    if (directory.isEmpty()) QSKIP("通过 LMSC_MAIN_CAPTURE_DIRECTORY 显式启用主界面截图");
    QVERIFY(QDir().mkpath(directory));
    MainWindow window(nullptr, settingsFile());
    window.setTestMode(true);
    window.resize(1440, 900);
    window.show();
    QSignalSpy ready(&window, &MainWindow::documentReady);
    window.openPath(m_song);
    QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, 20000);
    QTRY_VERIFY_WITH_TIMEOUT(window.isAudioReady(), 20000);
    auto sidebar = window.findChild<lmsc::NavigationSidebar *>(QStringLiteral("mainSidebar"));
    auto nav = window.findChild<QListWidget *>(QStringLiteral("mainNavigation"));
    auto theme = window.findChild<QComboBox *>(QStringLiteral("themeMode"));
    auto key = window.findChild<QLineEdit *>(QStringLiteral("aiApiKey"));
    auto workspace = window.findChild<QStackedWidget *>(QStringLiteral("workspacePages"));
    auto settingsPanel = window.findChild<QWidget *>(QStringLiteral("settingsPanel"));
    auto settingsButtons = window.findChild<QDialogButtonBox *>(QStringLiteral("settingsButtons"));
    auto start = window.findChild<QPushButton *>(QStringLiteral("aiRecognizeButton"));
    auto configure = window.findChild<QPushButton *>(QStringLiteral("aiConfigureConnection"));
    QVERIFY(sidebar && nav && theme && key && workspace && settingsPanel && settingsButtons && start && configure);
    auto codexPath = window.findChild<QLineEdit *>(QStringLiteral("codexExecutable"));
    QVERIFY(codexPath);
    codexPath->setText(m_temp.filePath(QStringLiteral("unavailable-capture-codex.exe")));
    QVERIFY(key->text().isEmpty());
    const QString originalTheme = lmsc::ThemeManager::mode();
    const auto capture = [&](const QString &name) {
        QApplication::processEvents();
        QTest::qWait(150);
        return window.grab().save(QDir(directory).filePath(name + QStringLiteral(".png")));
    };
    const auto fullyInside = [](QWidget *widget, QWidget *page) {
        return widget->isVisible() && page->rect().contains(QRect(widget->mapTo(page, QPoint()), widget->size()));
    };
    for (const QString &mode : {QStringLiteral("dark"), QStringLiteral("light")}) {
        window.resize(1440, 900);
        nav->setCurrentRow(2);
        theme->setCurrentIndex(theme->findData(mode));
        sidebar->setCollapsed(true);
        nav->setCurrentRow(0);
        QVERIFY(capture(QStringLiteral("main-editor-%1").arg(mode)));
        sidebar->setCollapsed(false);
        QVERIFY(capture(QStringLiteral("main-editor-%1-expanded").arg(mode)));
        nav->setCurrentRow(2);
        QVERIFY(capture(QStringLiteral("main-settings-appearance-%1").arg(mode)));
        nav->setCurrentRow(3);
        QVERIFY(capture(QStringLiteral("main-settings-model-%1").arg(mode)));
        nav->setCurrentRow(4);
        QVERIFY(capture(QStringLiteral("main-settings-account-submenu-%1").arg(mode)));
        nav->setCurrentRow(1);
        QVERIFY(capture(QStringLiteral("main-ai-recognition-%1").arg(mode)));
        nav->setCurrentRow(6);
        QVERIFY(capture(QStringLiteral("main-about-%1").arg(mode)));
        sidebar->setCollapsed(true);
        nav->setCurrentRow(3);
        nav->setFocus();
        QTest::keyClick(nav, Qt::Key_Right);
        auto submenu = sidebar->findChild<QMenu *>(QStringLiteral("navigationSubmenu"));
        QVERIFY(submenu && submenu->isVisible());
        QTest::qWait(30);
        QVERIFY(submenu->grab().save(QDir(directory).filePath(
            QStringLiteral("main-settings-account-popup-%1.png").arg(mode))));
        submenu->close();
        QApplication::processEvents();
        const QSize minimum = window.minimumSize();
        window.resize(minimum);
        QApplication::processEvents();
        qInfo() << "Main minimum capture" << mode << "minimum" << minimum
                << "actual" << window.size() << "workspace hint" << workspace->minimumSizeHint();
        QCOMPARE(window.size(), minimum);
        for (const auto role : {QDialogButtonBox::Save, QDialogButtonBox::Apply, QDialogButtonBox::Cancel})
            QVERIFY(fullyInside(settingsButtons->button(role), settingsPanel));
        QVERIFY(capture(QStringLiteral("main-settings-model-%1-minimum").arg(mode)));
        nav->setCurrentRow(1);
        QApplication::processEvents();
        auto details = window.findChild<QScrollArea *>(QStringLiteral("aiGenerationDetailsScroll"));
        QVERIFY(details); details->ensureWidgetVisible(start); QApplication::processEvents();
        QVERIFY(fullyInside(start, details->viewport()));
        details->ensureWidgetVisible(configure); QApplication::processEvents();
        if (configure->isVisible()) QVERIFY(fullyInside(configure, details->viewport()));
        QVERIFY(capture(QStringLiteral("main-ai-recognition-%1-minimum").arg(mode)));
    }
    window.close();
    lmsc::ThemeManager::apply(originalTheme);
}

int main(int argc, char **argv) {
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication application(argc, argv);
    application.setFont(QFont(QStringLiteral("Microsoft YaHei UI"), 9));
    MainWindowTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "MainWindowTest.moc"
