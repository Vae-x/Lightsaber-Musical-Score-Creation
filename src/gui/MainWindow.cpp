#include "MainWindow.h"
#include "ui_MainWindow.h"
#include "EditorViews.h"
#include "SettingsPanel.h"
#include "NavigationSidebar.h"
#include "AiRecognitionPage.h"
#include "EditorSessionController.h"
#include "EditorRefinementPanel.h"
#include "ThemeManager.h"
#include "SongImportDialog.h"
#include "SongExportDialog.h"
#include "core/AppInfo.h"
#include "core/AppSettings.h"
#include "core/AudioService.h"
#include "core/RhythmAnalyzer.h"
#include "core/MtpImportService.h"
#include "core/MtpExportService.h"
#include "core/WorkspacePaths.h"
#include "core/SongExporter.h"
#include "core/AiTextTransport.h"
#include "core/LocalAiGenerationService.h"
#include "core/HybridAiGenerationService.h"
#include "core/InfernoSaberGenerationService.h"
#include "core/AiRefinementService.h"
#include "core/BeatmapPlayabilityValidator.h"
#include <QtConcurrent>
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QEventLoop>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QImage>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenuBar>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QProgressBar>
#include <QProgressDialog>
#include <QDesktopServices>
#include <QPushButton>
#include <QRegularExpression>
#include <QScreen>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QSplitter>
#include <QStatusBar>
#include <QStackedWidget>
#include <QTemporaryDir>
#include <QTabWidget>
#include <QTimer>
#include <QToolBar>
#include <QUrl>
#include <QUuid>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>

namespace {
QStringList directionNames() {
    return {QStringLiteral("↑ 上"), QStringLiteral("↓ 下"), QStringLiteral("← 左"),
            QStringLiteral("→ 右"), QStringLiteral("↖ 左上"), QStringLiteral("↗ 右上"),
            QStringLiteral("↙ 左下"), QStringLiteral("↘ 右下"), QStringLiteral("● 无方向")};
}
void addNewSongDifficultyChoices(QComboBox *combo) {
    struct Choice { QString name, label; int rank; };
    const QVector<Choice> choices{
        {QStringLiteral("Easy"), QStringLiteral("简单 · Easy"), 1},
        {QStringLiteral("Normal"), QStringLiteral("普通 · Normal"), 3},
        {QStringLiteral("Hard"), QStringLiteral("困难 · Hard"), 5},
        {QStringLiteral("Expert"), QStringLiteral("专家 · Expert"), 7},
        {QStringLiteral("ExpertPlus"), QStringLiteral("专家+ · ExpertPlus"), 9}
    };
    for (const auto &choice : choices) {
        combo->addItem(choice.label, choice.name);
        combo->setItemData(combo->count() - 1, choice.rank, Qt::UserRole + 1);
    }
    combo->setCurrentIndex(combo->findData(QStringLiteral("Expert")));
}
struct StorageResult { bool ok = false; QString error; };
bool sameGenerationTiming(const lmsc::TimeMap &a, const lmsc::TimeMap &b) {
    if (a.baseBpm() != b.baseBpm() || a.firstBeatSeconds() != b.firstBeatSeconds()
        || a.changes().size() != b.changes().size()) return false;
    for (int i = 0; i < a.changes().size(); ++i)
        if (a.changes()[i].beat != b.changes()[i].beat || a.changes()[i].bpm != b.changes()[i].bpm) return false;
    return true;
}
bool sameAiPreferences(const lmsc::AppPreferences &a, const lmsc::AppPreferences &b) {
    if (a.aiConnection != b.aiConnection || a.providerId != b.providerId
        || a.codexExecutable != b.codexExecutable || a.codexModel != b.codexModel
        || a.networkProxy.mode != b.networkProxy.mode || a.networkProxy.host != b.networkProxy.host
        || a.networkProxy.port != b.networkProxy.port || a.requestTimeoutMinutes != b.requestTimeoutMinutes || a.providers.keys() != b.providers.keys()) return false;
    for (auto it = a.providers.begin(); it != a.providers.end(); ++it) {
        const auto other = b.providers.value(it.key());
        if (it.value().baseUrl != other.baseUrl || it.value().apiKey != other.apiKey
            || it.value().model != other.model || it.value().models != other.models
            || it.value().maxOutputTokens != other.maxOutputTokens) return false;
    }
    return true;
}
bool sameGenerationModel(const lmsc::AppPreferences &a, const lmsc::AppPreferences &b) {
    if (a.aiConnection!=b.aiConnection) return false;
    if (a.aiConnection=="codex") return a.codexExecutable==b.codexExecutable && a.codexModel==b.codexModel;
    const auto first=a.providers.value(a.providerId), second=b.providers.value(b.providerId);
    return a.providerId==b.providerId && first.baseUrl.trimmed()==second.baseUrl.trimmed() && first.model==second.model;
}

// Dense editor panes must not set the minimum size of a hidden settings page.
class WorkspacePages final : public QStackedWidget {
public:
    explicit WorkspacePages(QWidget *parent) : QStackedWidget(parent) {
        layout()->setSizeConstraint(QLayout::SetNoConstraint);
        connect(this, &QStackedWidget::currentChanged, this, [this] { updateGeometry(); });
    }
    QSize minimumSizeHint() const override {
        const auto page = currentWidget();
        return page ? page->minimumSizeHint().expandedTo(page->minimumSize()) : QSize();
    }
};
}

MainWindow::MainWindow(QWidget *parent, const QString &settingsFile)
    : QMainWindow(parent), ui(new Ui::MainWindow), m_settingsFile(settingsFile),
      m_document(std::make_shared<lmsc::BeatmapDocument>()),
      m_audio(new AudioService(this)), m_analyzer(new RhythmAnalyzer(this)),
      m_mtp(new MtpImportService(this)),
      m_mtpExport(new MtpExportService(this)),
      m_loader(new QFutureWatcher<DocumentLoadResult>(this)) {
    ui->setupUi(this);
    const auto preferences = lmsc::AppSettings(m_settingsFile).load();
    m_generationPreferences = preferences;
    lmsc::DiagnosticLog::instance().setEnabled(preferences.diagnosticLogEnabled);
    lmsc::DiagnosticLog::instance().record("application.started");
    lmsc::ThemeManager::apply(preferences.themeMode);
    m_documentId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_editorSession = new lmsc::EditorSessionController(this);
    m_editorSession->setObjectName(QStringLiteral("editorSessionController"));
    m_editorSession->setFormalDocument(m_document.get(), m_documentId);
    m_aiTransport = new lmsc::ConfiguredAiTextTransport(this);
    m_aiTransport->configure(preferences);
    m_defaultGenerationService = new lmsc::LlmAiGenerationService(m_aiTransport, this);
    m_localGenerationService = new lmsc::LocalAiGenerationService(this);
    m_hybridGenerationService = new lmsc::HybridAiGenerationService(m_aiTransport, this);
    m_infernoGenerationService = new lmsc::InfernoSaberGenerationService(this);
    configureInfernoGeneration(preferences);
    m_defaultRefinementService = new lmsc::AiRefinementService(m_aiTransport, this);
    m_refinementService = m_defaultRefinementService;
    lmsc::ThemeManager::watchSystemChanges(qApp);
    setWindowIcon(QIcon(QStringLiteral(":/icons/app.png")));
    lmsc::WorkspacePaths::projectsDirectory();
    const QRect available = QGuiApplication::primaryScreen()->availableGeometry();
    resize(std::min(1440, available.width() - 40), std::min(900, available.height() - 60));
    setMinimumSize(std::min(1050, available.width() - 40), std::min(660, available.height() - 60));
    buildEditor();
    buildActions();
    buildWorkspace();
    connectRefinementService();
    connectAudio();
    connect(m_mtp, &MtpImportService::songImported, this, [this](const QString &folder) {
        if (!m_mtpImportPending) return;
        m_mtpImportPending = false;
        setBusy(false);
        openPath(folder);
    });
    connect(m_mtp, &MtpImportService::taskProgress, this, [this](const QString &task, int percent) {
        if (!m_mtpImportPending) return;
        statusBar()->showMessage(task);
        m_progress->setRange(0, percent < 0 ? 0 : 100);
        if (percent >= 0) m_progress->setValue(percent);
    });
    connect(m_mtp, &MtpImportService::errorOccurred, this, [this](const QString &error) {
        if (!m_mtpImportPending) return;
        m_mtpImportPending = false;
        setBusy(false);
        emit loadFailed(error);
        showError(error);
    });
    connect(m_mtp, &MtpImportService::cancelled, this, [this] {
        if (!m_mtpImportPending) return;
        m_mtpImportPending = false;
        setBusy(false);
        statusBar()->showMessage(tr("已取消头显歌曲导入"), 8000);
    });
    connect(m_loader, &QFutureWatcher<DocumentLoadResult>::finished, this, [this] {
        const auto result = m_loader->result();
        setBusy(false);
        if (m_discardLoad) {
            m_discardLoad = false; m_creationLoad = false;
            if (m_mediaFlow) { m_mediaFlow = false; restoreDocumentAudio(); }
            return;
        }
        if (!result.error.isEmpty()) {
            m_creationLoad = false;
            if (m_mediaFlow) { m_mediaFlow = false; restoreDocumentAudio(); }
            emit loadFailed(result.error);
            showError(result.error);
            return;
        }
        const bool created = m_creationLoad;
        m_creationLoad = false;
        if (created) { m_analyzeNew = true; m_mediaFlow = false; }
        replaceDocument(result.document);
        if (created) {
            statusBar()->showMessage(tr("新歌已创建，正在估计节拍；保存工程后启用自动恢复"), 20000);
            if (!m_testMode) saveProject(true);
        }
    });
    m_autosave = new QTimer(this);
    m_autosave->setObjectName(QStringLiteral("documentAutosave"));
    m_autosave->setInterval(30000);
    connect(m_autosave, &QTimer::timeout, this, [this] {
        if (m_busy || (!m_document->isModified() && !m_editorSession->isDirty()) || m_document->projectPath().isEmpty()) return;
        if (!syncDraftRecords()) return;
        // Only the JSON snapshot is written; immutable assets were copied at first save.
        QString error;
        if (m_document->autoSave(&error)) statusBar()->showMessage(tr("已自动保存恢复快照"), 3000);
        else statusBar()->showMessage(tr("自动保存失败：") + error, 15000);
    });
    m_autosave->start();
    refreshDocument();
}

MainWindow::~MainWindow() {
    m_aiPage->cancelRecognition();
    cancelRefinement();
    for (const auto &connection : m_refinementConnections) disconnect(connection);
    if (m_refinementService && !m_refinementService->status().jobId.isEmpty())
        m_refinementService->discard(m_refinementService->status().jobId);
    m_cachedRefinement.reset();
    m_settingsPanel->discardChanges();
    m_mtp->cancel();
    m_mtpExport->cancel();
    m_audio->cancel();
    m_analyzer->cancel();
    m_loader->waitForFinished();
    delete ui;
}

void MainWindow::buildEditor() {
    m_editorPage = new QWidget(ui->centralwidget);
    m_editorPage->setObjectName(QStringLiteral("editorPage"));
    auto outer = new QVBoxLayout(m_editorPage);
    outer->setContentsMargins(10, 8, 10, 6);
    m_editorStateLabel = new QLabel(tr("正式谱"), m_editorPage);
    m_editorStateLabel->setObjectName(QStringLiteral("editorSessionState"));
    m_editorStateLabel->setProperty("role", "title");
    m_editorStateLabel->setWordWrap(true);
    outer->addWidget(m_editorStateLabel);
    auto split = new QSplitter(Qt::Horizontal, m_editorPage);
    outer->addWidget(split, 1);
    auto leftScroll = new QScrollArea(split);
    leftScroll->setWidgetResizable(true);
    leftScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    leftScroll->setMinimumWidth(235); leftScroll->setMaximumWidth(285);
    auto left = new QWidget;
    leftScroll->setWidget(left);
    auto leftLayout = new QVBoxLayout(left);
    m_songLabel = new QLabel(tr("打开曲谱或创建新歌"), left);
    m_songLabel->setWordWrap(true);
    m_songLabel->setProperty("role", "title");
    m_projectLabel = new QLabel(tr("工程尚未保存"), left);
    m_projectLabel->setWordWrap(true);
    leftLayout->addWidget(m_songLabel);
    leftLayout->addWidget(m_projectLabel);
    auto button = [&](const QString &text, auto callback) {
        auto b = new QPushButton(text, left);
        leftLayout->addWidget(b);
        connect(b, &QPushButton::clicked, this, callback);
    };
    button(tr("新歌 · MP3 / MP4"), [this] { newSong(); });
    button(tr("导入歌曲"), [this] { importSongFolder(); });
    button(tr("打开编辑工程"), [this] {
        if (m_busy || !confirmDocumentChange()) return;
        const QString path = QFileDialog::getOpenFileName(this, tr("打开编辑工程"),
            lmsc::WorkspacePaths::projectsDirectory(), tr("编辑工程 (*.lmsc)"));
        if (!path.isEmpty()) openPath(path);
    });
    m_recropButton = new QPushButton(tr("重新裁剪来源 · 新建空白谱"), left);
    leftLayout->addWidget(m_recropButton);
    connect(m_recropButton, &QPushButton::clicked, this, [this] {
        if (m_busy || !m_document->isNewSong() || !confirmDocumentChange()) return;
        m_recropPreset = m_document->importSource();
        if (!m_recropPreset.isAvailable()) { showError(tr("该工程没有保留原始媒体")); return; }
        QMessageBox::information(this, tr("重新裁剪"),
            tr("将使用工程保留的原始媒体创建另一张空白谱。原工程可继续保留；已有音符不会搬到新裁剪片段。"));
        m_recropPending = true;
        m_mediaFlow = true;
        ++m_analysisGeneration;
        m_analyzeNew = false; m_analyzer->cancel(); m_audio->cancel(); m_audio->stop();
        setBusy(true, tr("正在读取来源音轨…"));
        const QString source = m_recropPreset.path;
        queueAudioTask([this, source] { m_audio->probeMedia(source); });
    });
    leftLayout->addWidget(new QLabel(tr("文件中可用的难度"), left));
    m_refineCurrentChartButton = new QPushButton(tr("AI 精修当前曲谱"), left);
    m_refineCurrentChartButton->setObjectName(QStringLiteral("refineCurrentChartButton"));
    m_refineCurrentChartButton->setToolTip(tr("仅精修新歌工程中的 Standard v2.2.0 基础曲谱；先预览比较，确认后应用。"));
    leftLayout->addWidget(m_refineCurrentChartButton);
    connect(m_refineCurrentChartButton, &QPushButton::clicked, this, &MainWindow::refineCurrentChart);
    m_difficulties = new QListWidget(left);
    m_difficulties->setObjectName(QStringLiteral("difficultyList"));
    m_difficulties->setMinimumHeight(100);
    leftLayout->addWidget(m_difficulties, 1);
    connect(m_difficulties, &QListWidget::currentItemChanged, this,
            [this](QListWidgetItem *item, QListWidgetItem *) {
        if (!item || m_refreshing || m_busy) return;
        cancelRefinement();
        QString error;
        const auto difficultyId = item->data(Qt::UserRole).toString();
        if (!difficultyId.isEmpty() && !m_document->setDifficulty(difficultyId, &error)) showError(error);
        m_editorSession->selectTarget(item->data(Qt::UserRole + 1).toString());
        m_refinementPanel->clearResult();
        m_selection.clear();
        refreshDocument();
    });
    m_newDifficultyRow = new QWidget(left);
    auto difficultyLayout = new QVBoxLayout(m_newDifficultyRow);
    difficultyLayout->setContentsMargins(0, 0, 0, 0);
    difficultyLayout->addWidget(new QLabel(tr("新歌难度"), m_newDifficultyRow));
    m_newDifficultySelector = new QComboBox(m_newDifficultyRow);
    m_newDifficultySelector->setObjectName(QStringLiteral("newSongDifficultySelector"));
    addNewSongDifficultyChoices(m_newDifficultySelector);
    m_newDifficultySelector->setToolTip(tr("调整当前曲谱的难度标识；选择已存在的难度时切换到该谱。AI 生成其他难度会保留已有谱面。"));
    difficultyLayout->addWidget(m_newDifficultySelector);
    leftLayout->addWidget(m_newDifficultyRow);
    connect(m_newDifficultySelector, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int index) {
        if (index < 0 || m_refreshing || m_busy || !m_document->isNewSong()) return;
        cancelRefinement(true);
        QString error;
        const QString name = m_newDifficultySelector->currentData().toString();
        const int rank = m_newDifficultySelector->currentData(Qt::UserRole + 1).toInt();
        if (!m_document->setNewSongDifficulty(name, rank, &error)) showError(error);
        else statusBar()->showMessage(tr("已将新歌难度设为 %1").arg(m_newDifficultySelector->currentText()), 5000);
        m_editorSession->selectTarget(name);
        refreshDocument();
    });
    m_exportLeadInRow = new QWidget(left);
    auto leadInLayout = new QVBoxLayout(m_exportLeadInRow);
    leadInLayout->setContentsMargins(0, 0, 0, 0);
    leadInLayout->addWidget(new QLabel(tr("导出开场缓冲"), m_exportLeadInRow));
    m_exportLeadIn = new QDoubleSpinBox(m_exportLeadInRow);
    m_exportLeadIn->setObjectName(QStringLiteral("exportLeadInSeconds"));
    m_exportLeadIn->setRange(0, 10);
    m_exportLeadIn->setDecimals(3);
    m_exportLeadIn->setSingleStep(0.5);
    m_exportLeadIn->setSuffix(tr(" 秒"));
    m_exportLeadIn->setValue(2);
    m_exportLeadIn->setToolTip(tr("仅新歌导出：在音频开头补静音，并把音符、炸弹和墙后移相同时间。\n"
                                "编辑工程的音频和拍点不变；设为 0 关闭。"));
    leadInLayout->addWidget(m_exportLeadIn);
    leftLayout->addWidget(m_exportLeadInRow);
    auto tempo = new QGroupBox(tr("节拍校准"), left);
    auto tf = new QFormLayout(tempo);
    m_bpm = new QDoubleSpinBox(tempo);
    m_bpm->setRange(1, 1000);
    m_bpm->setDecimals(3);
    m_bpm->setValue(120);
    m_offset = new QDoubleSpinBox(tempo);
    m_offset->setRange(-3600, 86400);
    m_offset->setDecimals(4);
    m_offset->setSuffix(tr(" 秒"));
    tf->addRow(tr("BPM"), m_bpm);
    tf->addRow(tr("第一拍时间"), m_offset);
    m_calibrateButton = new QPushButton(tr("应用校准"), tempo);
    tf->addRow(m_calibrateButton);
    connect(m_calibrateButton, &QPushButton::clicked, this, &MainWindow::calibrateTempo);
    m_estimateButton = new QPushButton(tr("重新估计节拍"), tempo);
    tf->addRow(m_estimateButton);
    connect(m_estimateButton, &QPushButton::clicked, this, [this] {
        if (!m_busy && m_document->isNewSong() && m_audio->isReady() && !m_analyzer->isBusy()
            && m_editorSession->view() == lmsc::EditorSessionController::View::Formal
            && !m_editorSession->viewReadOnly() && editingDocument() == m_document.get())
            requestRhythmAnalysis();
    });
    m_analysisLabel = new QLabel(tr("已有曲谱保留原始节拍。\n新歌估拍后可在此校准。"), tempo);
    m_analysisLabel->setObjectName(QStringLiteral("rhythmAnalysisStatus"));
    m_analysisLabel->setWordWrap(true);
    tf->addRow(m_analysisLabel);
    leftLayout->addWidget(tempo);
    m_summaryLabel = new QLabel(left);
    m_summaryLabel->setWordWrap(true);
    leftLayout->addWidget(m_summaryLabel);
    auto center = new QSplitter(Qt::Vertical, split);
    auto top = new QSplitter(Qt::Horizontal, center);
    m_track = new TrackView(top);
    m_grid = new GridEditor(top);
    // Leave room for both editing surfaces beside the integrated tools panel.
    m_track->setMinimumWidth(180);
    m_grid->setMinimumWidth(180);
    top->setChildrenCollapsible(false);
    top->setStretchFactor(0, 3);
    top->setStretchFactor(1, 2);
    m_timeline = new TimelineView(center);
    center->setStretchFactor(0, 3);
    center->setStretchFactor(1, 2);
    m_toolsTabs = new QTabWidget(split);
    m_toolsTabs->setObjectName(QStringLiteral("editorToolsTabs"));
    m_toolsTabs->setMinimumWidth(290); m_toolsTabs->setMaximumWidth(400);
    auto rightScroll = new QScrollArea(m_toolsTabs);
    rightScroll->setWidgetResizable(true);
    rightScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    rightScroll->setMinimumWidth(245);
    m_toolsTabs->addTab(rightScroll, tr("物件"));
    auto right = new QWidget;
    rightScroll->setWidget(right);
    auto rl = new QVBoxLayout(right);
    auto placement = new QGroupBox(tr("放置工具"), right);
    auto pf = new QFormLayout(placement);
    m_placeType = new QComboBox(placement);
    m_placeType->addItems({tr("音符"), tr("炸弹"), tr("墙")});
    m_placeColor = new QComboBox(placement);
    m_placeColor->addItems({tr("左手 · 红"), tr("右手 · 蓝")});
    m_placeDirection = new QComboBox(placement);
    m_placeDirection->addItems(directionNames());
    m_placeDirection->setCurrentIndex(8);
    pf->addRow(tr("物件"), m_placeType);
    pf->addRow(tr("颜色"), m_placeColor);
    pf->addRow(tr("方向"), m_placeDirection);
    rl->addWidget(placement);
    auto updatePlacement = [this] {
        m_grid->setPlacement(m_placeType->currentIndex(), m_placeColor->currentIndex(),
                             m_placeDirection->currentIndex());
        m_placeColor->setEnabled(m_placeType->currentIndex() == 0);
        m_placeDirection->setEnabled(m_placeType->currentIndex() == 0);
    };
    for (auto c : {m_placeType, m_placeColor, m_placeDirection})
        connect(c, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
                [updatePlacement](int) { updatePlacement(); });
    updatePlacement();
    auto properties = new QGroupBox(tr("选中物件属性"), right);
    auto ef = new QFormLayout(properties);
    m_editBeat = new QDoubleSpinBox(properties);
    m_editBeat->setRange(0, 1000000); m_editBeat->setDecimals(4);
    m_editBeat->setSingleStep(0.25);
    m_editX = new QSpinBox(properties); m_editX->setRange(0, 3);
    m_editY = new QSpinBox(properties); m_editY->setRange(0, 2);
    m_editColor = new QComboBox(properties); m_editColor->addItems({tr("左手 · 红"), tr("右手 · 蓝")});
    m_editDirection = new QComboBox(properties); m_editDirection->addItems(directionNames());
    m_editDuration = new QDoubleSpinBox(properties);
    m_editDuration->setRange(0.001, 100000);
    m_editDuration->setDecimals(3); m_editDuration->setValue(1);
    m_editWidth = new QSpinBox(properties); m_editWidth->setRange(1, 4);
    m_editHeight = new QSpinBox(properties); m_editHeight->setRange(1, 5); m_editHeight->setValue(5);
    ef->addRow(tr("拍位置"), m_editBeat);
    ef->addRow(tr("列 (0–3)"), m_editX); ef->addRow(tr("层 (0–2)"), m_editY);
    ef->addRow(tr("颜色"), m_editColor); ef->addRow(tr("方向"), m_editDirection);
    ef->addRow(tr("墙时长 / 拍"), m_editDuration);
    ef->addRow(tr("墙宽度"), m_editWidth); ef->addRow(tr("墙高度"), m_editHeight);
    m_applyButton = new QPushButton(tr("应用属性"), properties);
    ef->addRow(m_applyButton);
    connect(m_applyButton, &QPushButton::clicked, this, &MainWindow::applyProperties);
    rl->addWidget(properties);
    m_protectionLabel = new QLabel(tr("点击物件查看属性"), right);
    m_protectionLabel->setWordWrap(true);
    m_protectionLabel->setProperty("role", "warning");
    rl->addWidget(m_protectionLabel);
    auto hint = new QLabel(tr("网格：点击空格放置，点击物件选择\nCtrl：追加选择\n时间轴：拖动物件移动；空白处框选\nShift 拖动：设置循环\n滚轮：缩放；中键拖动：平移\n墙使用下方宽度、高度、时长"), right);
    hint->setWordWrap(true);
    hint->setProperty("role", "muted");
    rl->addStretch(); rl->addWidget(hint);
    split->setStretchFactor(1, 1);
    auto transport = new QHBoxLayout;
    m_playButton = new QPushButton(tr("▶ 播放"), m_editorPage);
    auto stop = new QPushButton(tr("停止"), m_editorPage);
    transport->addWidget(m_playButton); transport->addWidget(stop);
    connect(m_playButton, &QPushButton::clicked, this, [this] {
        if (m_audio->isPlaying()) m_audio->pause(); else if (!m_busy && isAudioReady()) m_audio->play();
    });
    connect(stop, &QPushButton::clicked, m_audio, &AudioService::stop);
    m_speed = new QComboBox(m_editorPage);
    for (double value : {0.5, 0.75, 1.0, 1.25, 1.5}) m_speed->addItem(QString::number(value) + "×", value);
    m_speed->setCurrentIndex(2);
    transport->addWidget(new QLabel(tr("速度"))); transport->addWidget(m_speed);
    connect(m_speed, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) {
        if (!m_busy && isAudioReady()) m_audio->setPlaybackSpeed(m_speed->currentData().toDouble());
    });
    m_snap = new QComboBox(m_editorPage);
    for (int value : {1, 2, 4, 8, 16}) m_snap->addItem("1/" + QString::number(value) + tr(" 拍"), value);
    m_snap->setCurrentIndex(2);
    transport->addWidget(new QLabel(tr("吸附"))); transport->addWidget(m_snap);
    connect(m_snap, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) {
        m_timeline->setSnapDivision(m_snap->currentData().toInt());
        m_editBeat->setSingleStep(1.0 / m_snap->currentData().toInt());
        m_grid->setBeat(currentBeat());
    });
    m_loop = new QCheckBox(tr("循环"), m_editorPage);
    m_metronome = new QCheckBox(tr("节拍器"), m_editorPage);
    transport->addWidget(m_loop); transport->addWidget(m_metronome);
    connect(m_loop, &QCheckBox::toggled, this, [this](bool on) {
        m_audio->setLoop(m_loopStart, m_loopEnd, on);
    });
    connect(m_metronome, &QCheckBox::toggled, this, [this](bool on) {
        m_audio->setMetronome(on, m_document->timeMap().baseBpm(), m_document->timeMap().firstBeatSeconds());
    });
    m_seekSlider = new QSlider(Qt::Horizontal, m_editorPage);
    m_seekSlider->setRange(0, 10000);
    transport->addWidget(m_seekSlider, 1);
    connect(m_seekSlider, &QSlider::valueChanged, this, [this](int value) {
        seek(m_audio->duration() * value / 10000.0);
    });
    m_positionLabel = new QLabel(tr("0.00 秒 · 0.00 拍"), m_editorPage);
    m_positionLabel->setMinimumWidth(180);
    transport->addWidget(m_positionLabel);
    outer->addLayout(transport);
    m_progress = new QProgressBar(this);
    m_progress->setMaximumWidth(250);
    m_progress->setMinimumHeight(m_progress->fontMetrics().height() + 8);
    m_progress->hide();
    m_cancelButton = new QPushButton(tr("取消任务"), this); m_cancelButton->hide();
    statusBar()->addPermanentWidget(m_progress);
    statusBar()->addPermanentWidget(m_cancelButton);
    connect(m_cancelButton, &QPushButton::clicked, this, [this] {
        if (m_mtpImportPending) { m_mtp->cancel(); return; }
        m_analyzeNew = m_newPending = m_previewPending = false;
        ++m_analysisGeneration;
        ++m_audioGeneration; m_queuedAudio = false;
        m_analyzer->cancel(); m_audio->cancel();
        if (m_loader->isRunning()) { m_discardLoad = true; statusBar()->showMessage(tr("等待当前读取结束后取消")); }
        else setBusy(false);
    });
    connect(m_grid, &GridEditor::addRequested, this, &MainWindow::addObject);
    connect(m_grid, &GridEditor::selectionChanged, this, &MainWindow::selectObjects);
    connect(m_track, &TrackView::selectionChanged, this, &MainWindow::selectObjects);
    connect(m_timeline, &TimelineView::selectionChanged, this, &MainWindow::selectObjects);
    connect(m_timeline, &TimelineView::seekRequested, this, &MainWindow::seek);
    connect(m_timeline, &TimelineView::objectsMoveRequested, this, &MainWindow::moveObjects);
    connect(m_timeline, &TimelineView::deleteRequested, this, &MainWindow::deleteObjects);
    connect(m_timeline, &TimelineView::loopChanged, this, [this](double a, double b) {
        m_loopStart = a; m_loopEnd = b; m_loop->setChecked(true); m_audio->setLoop(a, b, true);
        if (m_refinementPanel) m_refinementPanel->setSelection(a, b, true);
    });
}

void MainWindow::buildWorkspace() {
    ui->centralwidget->setObjectName(QStringLiteral("workspaceContent"));
    auto layout = new QHBoxLayout(ui->centralwidget);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    m_sidebar = new lmsc::NavigationSidebar(ui->centralwidget);
    m_sidebar->setObjectName(QStringLiteral("mainSidebar"));
    m_sidebar->setHeadingText(tr("工作区"));
    auto navigation = m_sidebar->listWidget();
    navigation->setObjectName(QStringLiteral("mainNavigation"));
    navigation->setAccessibleName(tr("工作区导航"));
    navigation->setAccessibleDescription(tr("使用上下方向键切换页面，左右方向键展开或收起大语言模型的账号授权子菜单。"));
    m_sidebar->addItem(tr("曲谱编辑"), lmsc::NavigationIcon::Editor);
    m_sidebar->addItem(tr("生成与精修"), lmsc::NavigationIcon::Recognition);
    m_sidebar->addItem(tr("外观"), lmsc::NavigationIcon::Appearance);
    m_sidebar->addItem(tr("大语言模型"), lmsc::NavigationIcon::Model);
    m_sidebar->addSubItem(3, tr("账号授权"), lmsc::NavigationIcon::Account);
    m_sidebar->addItem(tr("网络"), lmsc::NavigationIcon::Network);
    m_sidebar->addItem(tr("关于"), lmsc::NavigationIcon::About);
    m_sidebar->setCollapsed(true);
    m_workspacePages = new WorkspacePages(ui->centralwidget);
    m_workspacePages->setObjectName(QStringLiteral("workspacePages"));
    m_workspacePages->addWidget(m_editorPage);
    m_generationTools = new QWidget(m_toolsTabs);
    m_generationTools->setObjectName(QStringLiteral("editorGenerationTools"));
    auto toolsLayout = new QVBoxLayout(m_generationTools);
    toolsLayout->setContentsMargins(6, 6, 6, 6);
    m_aiPage = new lmsc::AiRecognitionPage(m_generationTools);
    m_aiPage->setCompact(true);
    m_aiPage->setGenerationService(m_defaultGenerationService, m_defaultGenerationService);
    m_aiPage->setLocalGenerationService(m_localGenerationService, m_localGenerationService);
    m_aiPage->setHybridGenerationService(m_hybridGenerationService, m_hybridGenerationService);
    m_aiPage->setInfernoGenerationService(m_infernoGenerationService, m_infernoGenerationService);
    toolsLayout->addWidget(m_aiPage, 1);
    m_draftReplacement = new QWidget(m_generationTools);
    m_draftReplacement->setObjectName(QStringLiteral("draftReplacementPrompt"));
    auto replacementLayout = new QVBoxLayout(m_draftReplacement);
    replacementLayout->setContentsMargins(0, 0, 0, 0);
    m_draftReplacementLabel = new QLabel(m_draftReplacement);
    m_draftReplacementLabel->setWordWrap(true);
    replacementLayout->addWidget(m_draftReplacementLabel);
    auto replacementButtons = new QHBoxLayout;
    auto replace = new QPushButton(tr("继续替换"), m_draftReplacement);
    replace->setObjectName(QStringLiteral("draftReplacementAccept"));
    auto keep = new QPushButton(tr("保留草稿"), m_draftReplacement);
    keep->setObjectName(QStringLiteral("draftReplacementKeep"));
    auto cancel = new QPushButton(tr("取消"), m_draftReplacement);
    replacementButtons->addWidget(replace); replacementButtons->addWidget(keep); replacementButtons->addWidget(cancel);
    replacementLayout->addLayout(replacementButtons);
    connect(replace, &QPushButton::clicked, this, [this] {
        auto action = std::move(m_pendingDraftAction);
        m_pendingDraftAction = {}; m_draftReplacement->hide();
        if (action) action();
    });
    const auto keepDraft = [this] { m_pendingDraftAction = {}; m_draftReplacement->hide(); };
    connect(keep, &QPushButton::clicked, this, keepDraft);
    connect(cancel, &QPushButton::clicked, this, keepDraft);
    m_draftReplacement->hide(); toolsLayout->addWidget(m_draftReplacement);
    m_refinementPanel = new lmsc::EditorRefinementPanel(m_generationTools);
    toolsLayout->addWidget(m_refinementPanel);
    m_toolsTabs->addTab(m_generationTools, tr("生成与精修"));
    connect(m_refinementPanel, &lmsc::EditorRefinementPanel::refineRequested, this, &MainWindow::startRefinement);
    connect(m_refinementPanel, &lmsc::EditorRefinementPanel::resumeRequested, this, &MainWindow::resumeRefinement);
    connect(m_refinementPanel, &lmsc::EditorRefinementPanel::cancelRequested, this, [this] { cancelRefinement(); });
    connect(m_refinementPanel, &lmsc::EditorRefinementPanel::versionRequested, this, [this](auto version) {
        if (m_busy) return;
        cancelRefinement();
        m_editorSession->setView(static_cast<lmsc::EditorSessionController::View>(version));
        m_selection.clear(); refreshDocument();
    });
    connect(m_refinementPanel, &lmsc::EditorRefinementPanel::restoreInitialRequested, this, [this] {
        confirmDraftReplacement(tr("恢复初稿将替换当前草稿的手动修改和精修结果。"), [this] {
            cancelRefinement(true); QString error;
            if (!m_editorSession->restoreInitial(&error)) m_refinementPanel->showError(error);
            else { m_refinementPanel->clearResult(); editingChanged(); }
        });
    });
    connect(m_refinementPanel, &lmsc::EditorRefinementPanel::applyRequested, this, [this] {
        cancelRefinement(); QString error;
        if (!m_editorSession->applyDraft(&error)) { m_refinementPanel->showError(error); return; }
        m_selection.clear(); syncDraftRecords(); refreshDocument();
        m_aiPage->showGenerationApplied();
        statusBar()->showMessage(tr("工作草稿已应用到正式谱，可一次撤销。"), 10000);
    });
    connect(m_refinementPanel, &lmsc::EditorRefinementPanel::discardRequested, this, [this] {
        confirmDraftReplacement(tr("舍弃当前工作草稿及其对比版本？正式谱保持原样。"), [this] {
            cancelRefinement(true); QString error;
            if (!m_editorSession->discardDraft(&error)) { m_refinementPanel->showError(error); return; }
            m_refinementPanel->clearResult(); editingChanged();
        });
    });
    m_settingsPanel = new lmsc::SettingsPanel(m_workspacePages, m_settingsFile, true);
    m_workspacePages->addWidget(m_settingsPanel);
    layout->addWidget(m_sidebar);
    layout->addWidget(m_workspacePages, 1);
    connect(navigation, &QListWidget::currentRowChanged, this, &MainWindow::selectWorkspacePage);
    connect(m_settingsPanel, &lmsc::SettingsPanel::navigationRequested, this, [navigation](int index) {
        navigation->setCurrentRow(index + 2);
    });
    const auto returnToEditor = [navigation] { navigation->setCurrentRow(0); };
    connect(m_settingsPanel, &lmsc::SettingsPanel::done, this, returnToEditor);
    connect(m_settingsPanel, &lmsc::SettingsPanel::canceled, this, returnToEditor);
    connect(m_settingsPanel, &lmsc::SettingsPanel::preferencesChanged, this, [this] {
        const auto preferences = lmsc::AppSettings(m_settingsFile).load();
        lmsc::DiagnosticLog::instance().setEnabled(preferences.diagnosticLogEnabled);
        if (!sameAiPreferences(m_generationPreferences, preferences)) {
            if (sameGenerationModel(m_generationPreferences, preferences)) m_aiPage->pauseGenerationForConnectionChange();
            else m_aiPage->invalidateGenerationForConnectionChange();
            cancelRefinement(true);
            m_aiTransport->configure(preferences);
            refreshRecognitionContext();
        }
        if (m_generationPreferences.infernoRuntimeDirectory != preferences.infernoRuntimeDirectory
            || m_generationPreferences.infernoModelCacheDirectory != preferences.infernoModelCacheDirectory
            || m_generationPreferences.infernoThreads != preferences.infernoThreads) {
            if (m_aiPage->generationMode() == lmsc::AiRecognitionPage::InfernoSaber)
                m_aiPage->invalidateGeneration();
            configureInfernoGeneration(preferences);
            refreshRecognitionContext();
        }
        m_generationPreferences = preferences;
        statusBar()->showMessage(tr("设置已保存"), 5000);
    });
    connect(m_aiPage, &lmsc::AiRecognitionPage::configureConnectionRequested, this, [this, navigation] {
        navigation->setCurrentRow(3);
        if (m_aiPage->generationMode() == lmsc::AiRecognitionPage::InfernoSaber)
            m_settingsPanel->showInfernoSettings();
    });
    connect(m_aiPage, &lmsc::AiRecognitionPage::generationDraftReady, this, &MainWindow::previewGeneratedChart);
    connect(m_aiPage, &lmsc::AiRecognitionPage::generationInvalidated, this, [this] {
        cancelRefinement(true);
        m_pendingDraftAction = {}; m_draftReplacement->hide();
    });
    navigation->setCurrentRow(0);
}

void MainWindow::selectWorkspacePage(int row) {
    if (row < 0 || row > 6) return;
    if (row < 2) {
        m_workspacePages->setCurrentWidget(m_editorPage);
        if (row == 1) showGenerationTools();
    } else {
        m_workspacePages->setCurrentWidget(m_settingsPanel);
        m_settingsPanel->selectPage(row - 2);
    }
    updateWorkspaceActions();
}

void MainWindow::refreshRecognitionContext() {
    if (!m_aiPage) return;
    // Decoding and queued audio tasks change readiness without refreshing the
    // document. The editor's refinement entry follows the same live context.
    m_refineCurrentChartButton->setEnabled(currentChartSupportsRefinement());
    const bool loaded = m_document->isLoaded();
    m_aiPage->setContext(loaded ? m_document->audioPath() : QString(),
                         loaded ? m_document->title() : QString(),
                         m_document->timeMap().baseBpm(),
                         m_document->timeMap().firstBeatSeconds(),
                         loaded ? m_audio->duration() : 0,
                         m_busy || m_audio->isBusy() || m_queuedAudio);
    lmsc::GenerationRequest context;
    context.documentId = loaded ? m_documentId : QString();
    context.difficultyId = m_document->currentDifficultyId();
    context.documentRevision = m_document->revision();
    context.timeMap = m_document->timeMap();
    if (loaded) context.audio = m_audio->pcmSnapshot();
    context.audioRevision = context.audio.revision;
    QString difficultyName = QStringLiteral("Expert");
    for (const auto &difficulty : m_document->difficulties())
        if (difficulty.id == context.difficultyId) { difficultyName = difficulty.name; break; }
    context.profile = lmsc::DifficultyProfile::forName(difficultyName);
    m_aiPage->setGenerationContext(context, loaded && m_document->isNewSong(),
                                 m_busy || m_audio->isBusy() || m_queuedAudio);
}

void MainWindow::setAiRecognitionService(lmsc::AiRecognitionService *service) {
    m_aiPage->setService(service);
    refreshRecognitionContext();
}

void MainWindow::setAiGenerationService(lmsc::AiGenerationService *service) {
    m_aiPage->setGenerationService(service ? service : m_defaultGenerationService, m_defaultGenerationService);
    m_aiPage->setGenerationMode(lmsc::AiRecognitionPage::LanguageModel);
    refreshRecognitionContext();
}

void MainWindow::setAiLocalGenerationService(lmsc::AiGenerationService *service) {
    m_aiPage->setLocalGenerationService(service ? service : m_localGenerationService, m_localGenerationService);
    m_aiPage->setGenerationMode(lmsc::AiRecognitionPage::LocalQuick);
    refreshRecognitionContext();
}

void MainWindow::setAiHybridGenerationService(lmsc::AiGenerationService *service) {
    m_aiPage->setHybridGenerationService(service ? service : m_hybridGenerationService, m_hybridGenerationService);
    m_aiPage->setGenerationMode(lmsc::AiRecognitionPage::Hybrid);
    refreshRecognitionContext();
}

void MainWindow::setInfernoGenerationService(lmsc::AiGenerationService *service) {
    m_aiPage->setInfernoGenerationService(service ? service : m_infernoGenerationService, m_infernoGenerationService);
    m_aiPage->setGenerationMode(lmsc::AiRecognitionPage::InfernoSaber);
    refreshRecognitionContext();
}

void MainWindow::configureInfernoGeneration(const lmsc::AppPreferences &preferences) {
    lmsc::InfernoSaberGenerationService::Config config;
    config.runtimeDirectory = preferences.infernoRuntimeDirectory;
    config.modelCacheDirectory = preferences.infernoModelCacheDirectory;
    config.threads = preferences.infernoThreads;
    config.runnerPath = QDir(QCoreApplication::applicationDirPath())
        .filePath(QStringLiteral("tools/infernosaber/infernosaber_trial.py"));
    config.toolsDirectory = m_audio->toolsDirectory();
    config.workDirectory = QDir(config.runtimeDirectory).filePath(QStringLiteral("jobs"));
    m_infernoGenerationService->setConfig(config);
}

void MainWindow::updateWorkspaceActions() {
    const bool editing = m_workspacePages && m_workspacePages->currentWidget() == m_editorPage;
    m_editorToolbar->setVisible(editing);
    m_saveAction->setEnabled(editing && m_document->isLoaded() && !m_busy);
    m_exportAction->setEnabled(editing && m_document->isLoaded() && !m_busy);
    m_undoAction->setEnabled(editing && !m_busy && !m_editorSession->viewReadOnly() && editingDocument()->canUndo());
    m_redoAction->setEnabled(editing && !m_busy && !m_editorSession->viewReadOnly() && editingDocument()->canRedo());
}

bool MainWindow::editorCommandAllowed() const {
    // Text fields keep their own editing shortcuts.
    const QWidget *focus = QApplication::focusWidget();
    return m_workspacePages && m_workspacePages->currentWidget() == m_editorPage
        && !m_busy && editingDocument()->isLoaded() && !m_editorSession->viewReadOnly() && !qobject_cast<const QLineEdit *>(focus)
        && !qobject_cast<const QAbstractSpinBox *>(focus);
}

void MainWindow::buildActions() {
    auto file = menuBar()->addMenu(tr("文件"));
    auto edit = menuBar()->addMenu(tr("编辑"));
    auto toolbar = addToolBar(tr("编辑操作"));
    m_editorToolbar = toolbar;
    toolbar->setObjectName(QStringLiteral("editorToolbar"));
    toolbar->setMovable(false);
    auto action = [this](QMenu *menu, const QString &name, const QKeySequence &shortcut, auto callback) {
        auto a = menu->addAction(name); a->setShortcut(shortcut);
        connect(a, &QAction::triggered, this, callback); return a;
    };
    action(file, tr("新歌"), QKeySequence::New, [this] { newSong(); });
    action(file, tr("导入歌曲"), QKeySequence::Open, [this] { importSongFolder(); });
    action(file, tr("打开编辑工程"), QKeySequence("Ctrl+Shift+O"), [this] {
        if (m_busy || !confirmDocumentChange()) return;
        const auto path = QFileDialog::getOpenFileName(this, tr("打开编辑工程"), lmsc::WorkspacePaths::projectsDirectory(), tr("编辑工程 (*.lmsc)"));
        if (!path.isEmpty()) openPath(path);
    });
    file->addSeparator();
    m_saveAction = action(file, tr("保存工程"), QKeySequence::Save, [this] { saveProject(); });
    action(file, tr("工程另存为"), QKeySequence::SaveAs, [this] { saveProject(true); });
    m_exportAction = action(file, tr("导出歌曲"), QKeySequence("Ctrl+E"), [this] { exportSong(); });
    file->addSeparator();
    action(file, tr("退出"), QKeySequence("Alt+F4"), [this] { close(); });
    m_undoAction = action(edit, tr("撤销"), QKeySequence::Undo, [this] {
        if (editorCommandAllowed() && prepareManualEdit() && editingDocument()->undo()) editingChanged();
    });
    m_redoAction = action(edit, tr("重做"), QKeySequence::Redo, [this] {
        if (editorCommandAllowed() && prepareManualEdit() && editingDocument()->redo()) editingChanged();
    });
    m_redoAction->setShortcuts({QKeySequence("Ctrl+Y"), QKeySequence("Ctrl+Shift+Z")});
    edit->addSeparator();
    action(edit, tr("复制物件"), QKeySequence::Copy, [this] { if (editorCommandAllowed()) copyObjects(); });
    action(edit, tr("粘贴到当前拍"), QKeySequence::Paste, [this] { if (editorCommandAllowed()) pasteObjects(); });
    action(edit, tr("左右镜像"), QKeySequence("Ctrl+M"), [this] { if (editorCommandAllowed()) mirrorObjects(); });
    action(edit, tr("删除物件"), QKeySequence::Delete, [this] { if (editorCommandAllowed()) deleteObjects(); });
    action(edit, tr("选择全部"), QKeySequence::SelectAll, [this] {
        if (!editorCommandAllowed()) return;
        QSet<QString> ids; for (const auto &o : editingDocument()->objects()) ids.insert(o.id); selectObjects(ids);
    });
    toolbar->addAction(m_saveAction); toolbar->addAction(m_exportAction); toolbar->addSeparator();
    toolbar->addAction(m_undoAction); toolbar->addAction(m_redoAction);
    auto settings = menuBar()->addMenu(tr("设置"));
    auto settingsAction = action(settings, tr("外观设置"), QKeySequence("Ctrl+,"), [this] { showSettings(); });
    settingsAction->setObjectName(QStringLiteral("openSettingsAction"));
    auto play = new QAction(tr("播放 / 暂停"), this); play->setShortcut(Qt::Key_Space);
    addAction(play); connect(play, &QAction::triggered, this, [this] {
        if (m_workspacePages->currentWidget() != m_editorPage) return;
        const auto focus = QApplication::focusWidget();
        if (qobject_cast<QLineEdit *>(focus) || qobject_cast<QAbstractSpinBox *>(focus)) return;
        if (m_audio->isPlaying()) m_audio->pause(); else if (!m_busy && isAudioReady()) m_audio->play();
    });
    auto help = menuBar()->addMenu(tr("帮助"));
    help->addAction(tr("关于光剑曲谱制作"), this, &MainWindow::showAbout);
    help->addAction(tr("操作说明"), this, [this] {
        QMessageBox::information(this, tr("操作说明"),
            tr("导入文件夹或 ZIP → 选择难度 → 用波形定位、网格放置 → 保存工程 → 导出歌曲目录。\n\n"
               "工程 .lmsc 与旁边的 assets-*、source-* 资源目录需一起保留。导出的歌曲目录由你手动复制到头显。\n"
               "循环：时间轴 Shift 拖动；物件框选：空白处拖动。\n"
               "高级物件与关联音符会保留并锁定，选中可查看原因。\n"
               "BPM 估计只是建议，需要试听节拍器后校准。"));
    });
}

void MainWindow::showSettings() {
    m_sidebar->listWidget()->setCurrentRow(2);
}

void MainWindow::importSongFolder() {
    if (m_busy || !confirmDocumentChange()) return;
    SongImportDialog dialog(m_mtp, this);
    if (dialog.exec() != QDialog::Accepted) return;
    if (!dialog.fromDevice()) { openPath(dialog.localPath()); return; }
    m_mtpImportPending = true;
    setBusy(true, tr("正在从头显复制歌曲到电脑…"));
    m_mtp->importSong(dialog.selectedSong());
}

void MainWindow::showAbout() {
    m_sidebar->listWidget()->setCurrentRow(6);
}

void MainWindow::connectAudio() {
    connect(m_audio, &AudioService::mediaProbed, this, &MainWindow::showNewSongDialog);
    connect(m_audio, &AudioService::conversionFinished, this, &MainWindow::finishNewSong);
    connect(m_audio, &AudioService::audioReady, this, [this](double duration) {
        if (!m_storageBusy) { m_progress->hide(); m_cancelButton->hide(); }
        if (m_inMediaDialog) {
            if (m_previewPending) {
                m_previewPending = false;
                m_audio->seek(m_previewStart);
                m_audio->setLoop(m_previewStart, m_previewEnd, true);
                m_audio->play();
            }
            return;
        }
        m_timeline->setDuration(duration);
        if (m_initializeAudio || m_loopEnd <= m_loopStart) m_loopEnd = duration;
        m_initializeAudio = false;
        m_audio->setLoop(m_loopStart, m_loopEnd, m_loop->isChecked());
        m_playButton->setEnabled(!m_busy);
        m_estimateButton->setEnabled(!m_busy && m_document->isNewSong() && !m_analyzer->isBusy()
            && m_editorSession->view() == lmsc::EditorSessionController::View::Formal
            && !m_editorSession->viewReadOnly() && editingDocument() == m_document.get());
        m_speed->setEnabled(!m_busy);
        refreshRecognitionContext();
        if (m_analyzeNew) {
            requestRhythmAnalysis();
        }
    });
    connect(m_audio, &AudioService::waveformReady, this, [this](const QVector<float> &peaks) {
        if (!m_inMediaDialog) m_timeline->setWaveform(peaks, m_audio->duration());
    });
    connect(m_audio, &AudioService::positionChanged, this, [this](double seconds) {
        if (m_inMediaDialog || m_busy) return;
        m_timeline->setPlayheadSeconds(seconds); m_track->setPlayheadSeconds(seconds);
        m_grid->setBeat(currentBeat());
        const double beat = m_document->timeMap().secondsToBeat(seconds);
        m_positionLabel->setText(tr("%1 秒 · %2 拍").arg(seconds, 0, 'f', 2).arg(beat, 0, 'f', 2));
        if (!m_seekSlider->isSliderDown()) {
            QSignalBlocker blocker(m_seekSlider);
            m_seekSlider->setValue(m_audio->duration() > 0 ? int(seconds / m_audio->duration() * 10000) : 0);
        }
        if (m_metronome->isChecked())
            m_audio->setMetronome(true, m_document->timeMap().bpmAtBeat(beat),
                                 m_document->timeMap().beatToSeconds(std::floor(beat)));
    });
    connect(m_audio, &AudioService::playbackChanged, this, [this](bool playing) {
        m_playButton->setText(playing ? tr("Ⅱ 暂停") : tr("▶ 播放"));
    });
    connect(m_audio, &AudioService::playbackSpeedChanged, this, [this](double speed) {
        QSignalBlocker blocker(m_speed);
        const int index = m_speed->findData(speed);
        if (index >= 0) m_speed->setCurrentIndex(index);
    });
    connect(m_audio, &AudioService::taskProgress, this, [this](const QString &task, int percent) {
        if (m_storageBusy) return;
        m_progress->show(); m_cancelButton->show();
        m_progress->setRange(0, percent < 0 ? 0 : 100);
        if (percent >= 0) m_progress->setValue(percent);
        statusBar()->showMessage(task);
        m_playButton->setEnabled(!m_busy && isAudioReady());
        m_speed->setEnabled(!m_busy && isAudioReady());
        if (percent == 100 && !m_audio->isBusy() && !m_busy) {
            m_progress->hide(); m_cancelButton->hide();
        }
        refreshRecognitionContext();
    });
    connect(m_audio, &AudioService::errorOccurred, this, [this](const QString &error) {
        m_newPending = m_previewPending = m_analyzeNew = false;
        ++m_analysisGeneration;
        setBusy(false);
        if (m_mediaFlow && !m_inMediaDialog) { m_mediaFlow = false; restoreDocumentAudio(); }
        showError(error);
    });
    connect(m_audio, &AudioService::cancelled, this, [this] {
        if (!m_storageBusy) { m_progress->hide(); m_cancelButton->hide(); }
        if (!m_loader->isRunning() && !m_queuedAudio) {
            setBusy(false);
            if (m_mediaFlow && !m_inMediaDialog) { m_mediaFlow = false; restoreDocumentAudio(); }
        }
    });
    connect(m_analyzer, &RhythmAnalyzer::errorOccurred, this, [this](const QString &error) {
        m_analyzeNew = false;
        m_analysisLabel->setText(tr("估拍失败，可手动校准：") + error);
    });
}

void MainWindow::openPath(const QString &input) {
    if (m_busy) return;
    QString path = QFileInfo(input).absoluteFilePath();
    // The core handles folder/ZIP snapshots without changing the source.
    if (QFileInfo(path).suffix().compare("lmsc", Qt::CaseInsensitive) == 0 && !m_testMode
        && lmsc::BeatmapDocument::hasRecovery(path)) {
        if (QMessageBox::question(this, tr("恢复自动保存"),
                                  tr("该工程存在自动保存快照，是否恢复？"),
                                  QMessageBox::Yes | QMessageBox::No) == QMessageBox::Yes)
            path = lmsc::BeatmapDocument::recoveryPath(path);
    }
    m_analyzeNew = false; m_analyzer->cancel(); m_audio->cancel(); m_audio->stop();
    ++m_analysisGeneration;
    m_mediaFlow = false;
    ++m_audioGeneration; m_queuedAudio = false;
    m_creationLoad = false;
    m_discardLoad = false;
    setBusy(true, tr("正在读取曲谱和资源…"));
    m_loader->setFuture(QtConcurrent::run([path] {
        DocumentLoadResult result;
        result.document = std::make_shared<lmsc::BeatmapDocument>();
        const QFileInfo info(path);
        bool ok;
        if (info.suffix().compare("zip", Qt::CaseInsensitive) == 0 && info.isFile())
            ok = result.document->loadZip(path, &result.error);
        else if (info.suffix().compare("lmsc", Qt::CaseInsensitive) == 0
                 || (info.isDir() && QFileInfo(QDir(path).filePath("project.lmsc")).isFile()))
            ok = result.document->loadProject(path, &result.error);
        else ok = result.document->loadSong(path, &result.error);
        if (!ok && result.error.isEmpty()) result.error = QStringLiteral("无法读取工程或曲谱");
        return result;
    }));
}

void MainWindow::replaceDocument(std::shared_ptr<lmsc::BeatmapDocument> document) {
    cancelRefinement(true);
    m_aiPage->invalidateGeneration();
    m_document = std::move(document);
    m_documentId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_editorSession->setFormalDocument(m_document.get(), m_documentId);
    m_refinementPanel->clearResult();
    m_initializeAudio = true;
    m_selection.clear(); m_clipboard.clear(); m_loopStart = 0; m_loopEnd = 0;
    m_loop->setChecked(false);
    m_timeline->setWaveform({}, 0);
    m_analysisLabel->setText(m_document->isNewSong() ? tr("试听节拍器，校准 BPM 与第一拍时间。")
                                                   : tr("已有曲谱使用原始节拍，不改动歌曲时间参数。"));
    refreshDocument();
    if (!m_document->warnings().isEmpty()) {
        statusBar()->showMessage(m_document->warnings().join("；"), 30000);
        m_summaryLabel->setToolTip(m_document->warnings().join("\n"));
    }
    const QString audioPath = m_document->audioPath();
    queueAudioTask([this, audioPath] { m_audio->loadAudio(audioPath); });
    m_audio->setMetronome(m_metronome->isChecked(), m_document->timeMap().baseBpm(),
                         m_document->timeMap().firstBeatSeconds());
    m_sidebar->listWidget()->setCurrentRow(0);
    emit documentReady();
}

void MainWindow::refreshDocument() {
    m_refreshing = true;
    const auto active = editingDocument();
    const bool loaded = m_document->isLoaded();
    setWindowTitle((loaded ? m_document->title() + ((m_document->isModified() || m_editorSession->isDirty()) ? " *" : "") + " — " : "")
                   + tr("光剑曲谱制作"));
    m_songLabel->setText(loaded ? m_document->title() : tr("打开曲谱或创建新歌"));
    const QString project = m_document->projectPath();
    m_projectLabel->setText(project.isEmpty() ? tr("工程尚未保存") : QFileInfo(project).fileName());
    m_projectLabel->setToolTip(project);
    m_recropButton->setEnabled(!m_busy && m_document->isNewSong() && m_document->importSource().isAvailable());
    m_refineCurrentChartButton->setEnabled(currentChartSupportsRefinement());
    m_difficulties->clear();
    QSet<QString> listedDifficulties;
    for (const auto &difficulty : m_document->difficulties()) {
        auto item = new QListWidgetItem(difficulty.characteristic + " · " + difficulty.name, m_difficulties);
        item->setData(Qt::UserRole, difficulty.id);
        item->setData(Qt::UserRole + 1, difficulty.name);
        listedDifficulties.insert(difficulty.name);
        item->setToolTip(difficulty.filename + " / v" + difficulty.version);
        if (difficulty.name == m_editorSession->currentTargetKey()) m_difficulties->setCurrentItem(item);
    }
    for (const auto &name : m_editorSession->targetKeys()) if (!listedDifficulties.contains(name)) {
        auto item = new QListWidgetItem(tr("Standard · %1 · 草稿").arg(name), m_difficulties);
        item->setData(Qt::UserRole + 1, name);
        if (name == m_editorSession->currentTargetKey()) m_difficulties->setCurrentItem(item);
    }
    m_newDifficultyRow->setVisible(loaded && m_document->isNewSong());
    m_exportLeadInRow->setVisible(loaded && m_document->isNewSong());
    m_exportLeadIn->setEnabled(loaded && m_document->isNewSong() && !m_busy);
    m_newDifficultySelector->setEnabled(loaded && m_document->isNewSong() && !m_busy
        && m_editorSession->view() == lmsc::EditorSessionController::View::Formal
        && !m_editorSession->viewReadOnly() && active == m_document.get());
    if (loaded && m_document->isNewSong())
        for (const auto &difficulty : m_document->difficulties())
            if (difficulty.id==m_document->currentDifficultyId())
                m_newDifficultySelector->setCurrentIndex(m_newDifficultySelector->findData(difficulty.name));
    QVector<EditorObject> display;
    QSet<QString> valid;
    int protectedCount = 0;
    for (const auto &o : active->objects()) {
        EditorObject d;
        d.id = o.id; d.type = static_cast<int>(o.kind); d.beat = o.beat;
        d.x = o.x; d.y = o.y; d.color = o.color; d.direction = o.direction;
        d.duration = o.duration; d.width = o.width; d.height = o.height;
        d.locked = o.isProtected(); d.protectedReason = o.protectedReason;
        display.append(d); valid.insert(o.id); protectedCount += d.locked ? 1 : 0;
    }
    m_selection.intersect(valid);
    m_track->setObjects(display); m_grid->setObjects(display); m_timeline->setObjects(display);
    const auto &time = active->timeMap();
    m_bpm->setValue(time.baseBpm()); m_offset->setValue(time.firstBeatSeconds());
    m_track->setTempo(time.baseBpm(), time.firstBeatSeconds());
    m_timeline->setTempo(time.baseBpm(), time.firstBeatSeconds());
    auto toSeconds = [this](double beat) { return editingDocument()->timeMap().beatToSeconds(beat); };
    auto toBeat = [this](double seconds) { return editingDocument()->timeMap().secondsToBeat(seconds); };
    m_track->setTimeMapping(toSeconds, toBeat); m_timeline->setTimeMapping(toSeconds, toBeat);
    m_grid->setEnabled(loaded && active->readOnlyReason().isEmpty() && !m_busy && !m_editorSession->viewReadOnly());
    m_difficulties->setEnabled(!m_busy);
    for (QWidget *widget : QVector<QWidget *>{m_bpm, m_offset, m_calibrateButton})
        widget->setEnabled(loaded && m_document->isNewSong() && !m_busy
            && m_editorSession->view() == lmsc::EditorSessionController::View::Formal
            && !m_editorSession->viewReadOnly() && active == m_document.get());
    m_estimateButton->setEnabled(loaded && m_document->isNewSong() && m_audio->isReady() && !m_busy && !m_analyzer->isBusy()
        && m_editorSession->view() == lmsc::EditorSessionController::View::Formal
        && !m_editorSession->viewReadOnly() && active == m_document.get());
    m_playButton->setEnabled(isAudioReady() && !m_busy);
    m_speed->setEnabled(isAudioReady() && !m_busy);
    m_saveAction->setEnabled(loaded && !m_busy); m_exportAction->setEnabled(loaded && !m_busy);
    m_undoAction->setEnabled(!m_busy && active->canUndo()); m_redoAction->setEnabled(!m_busy && active->canRedo());
    m_summaryLabel->setText(loaded ? tr("%1 个物件 · %2 个受保护\n%3")
          .arg(display.size()).arg(protectedCount).arg(active->readOnlyReason()) : tr("支持 BeatSaver v2/v3 曲谱"));
    refreshSelection();
    m_grid->setBeat(currentBeat());
    refreshRecognitionContext();
    updateWorkspaceActions();
    refreshDraftPanel();
    m_refreshing = false;
}

void MainWindow::refreshSelection() {
    m_track->setSelectedIds(m_selection); m_grid->setSelectedIds(m_selection);
    m_timeline->setSelectedIds(m_selection);
    const lmsc::BeatObject *object = nullptr;
    if (m_selection.size() == 1)
        for (const auto &o : editingDocument()->objects()) if (m_selection.contains(o.id)) { object = &o; break; }
    const bool editable = object && !object->isProtected() && editingDocument()->readOnlyReason().isEmpty() && !m_busy && !m_editorSession->viewReadOnly();
    for (QWidget *w : QVector<QWidget *>{m_editBeat, m_editX, m_editY, m_editColor, m_editDirection, m_applyButton})
        w->setEnabled(editable);
    // Wall dimensions also serve as placement defaults when nothing is selected.
    for (QWidget *w : QVector<QWidget *>{m_editDuration, m_editWidth, m_editHeight})
        w->setEnabled(!m_busy && !m_editorSession->viewReadOnly() && (!object || (editable && object->kind == lmsc::ObjectKind::Wall)));
    if (object) {
        m_editBeat->setValue(object->beat); m_editX->setValue(object->x); m_editY->setValue(object->y);
        m_editColor->setCurrentIndex(object->color); m_editDirection->setCurrentIndex(object->direction);
        m_editColor->setEnabled(editable && object->kind == lmsc::ObjectKind::Note);
        m_editDirection->setEnabled(editable && object->kind == lmsc::ObjectKind::Note);
        if (object->kind == lmsc::ObjectKind::Wall) {
            m_editDuration->setValue(object->duration); m_editWidth->setValue(object->width);
            m_editHeight->setValue(object->height);
        }
        m_protectionLabel->setText(object->isProtected() ? tr("已保护：") + object->protectedReason
                                                        : tr("已选中 1 个物件"));
    } else m_protectionLabel->setText(m_selection.isEmpty() ? tr("点击物件查看属性")
                              : tr("已选中 %1 个物件，可拖动、复制、镜像或删除").arg(m_selection.size()));
    if (!editingDocument()->readOnlyReason().isEmpty()) m_protectionLabel->setText(editingDocument()->readOnlyReason());
    if (m_editorSession->viewReadOnly()) m_protectionLabel->setText(tr("当前为只读对比版本，切回工作草稿或正式谱继续编辑。"));
}

void MainWindow::selectObjects(const QSet<QString> &ids) { m_selection = ids; refreshSelection(); }
QStringList MainWindow::selectedIds() const { return m_selection.values(); }
double MainWindow::currentBeat() const {
    const double beat = editingDocument()->timeMap().secondsToBeat(m_audio->position());
    const int division = m_snap->currentData().toInt();
    return std::max(0.0, std::round(beat * division) / division);
}
void MainWindow::seek(double seconds) {
    if (m_busy) return;
    m_audio->seek(seconds);
    m_timeline->setPlayheadSeconds(seconds); m_track->setPlayheadSeconds(seconds); m_grid->setBeat(currentBeat());
}
void MainWindow::addObject(double beat, int x, int y) {
    if (!prepareManualEdit()) return;
    lmsc::BeatObject object;
    object.kind = static_cast<lmsc::ObjectKind>(m_placeType->currentIndex());
    object.beat = beat; object.x = x; object.y = y;
    object.color = m_placeColor->currentIndex(); object.direction = m_placeDirection->currentIndex();
    object.duration = m_editDuration->value(); object.width = m_editWidth->value(); object.height = m_editHeight->value();
    if (object.kind == lmsc::ObjectKind::Wall && object.height == 5) object.y = 0;
    if (object.kind == lmsc::ObjectKind::Wall && object.height == 3) object.y = 2;
    QString error;
    if (!editingDocument()->addObject(object, &error)) showError(error); else editingChanged();
}
void MainWindow::applyProperties() {
    if (m_busy || m_selection.size() != 1 || m_editorSession->viewReadOnly()) return;
    const auto id = *m_selection.begin();
    const double beat = m_editBeat->value(), duration = m_editDuration->value();
    const int x = m_editX->value(), y = m_editY->value(), color = m_editColor->currentIndex();
    const int direction = m_editDirection->currentIndex(), width = m_editWidth->value(), height = m_editHeight->value();
    if (!prepareManualEdit()) return;
    for (auto object : editingDocument()->objects()) {
        if (object.id != id) continue;
        object.beat = beat; object.x = x; object.y = y;
        if (object.kind == lmsc::ObjectKind::Note) { object.color = color; object.direction = direction; }
        if (object.kind == lmsc::ObjectKind::Wall) {
            object.duration = duration; object.width = width; object.height = height;
        }
        QString error;
        if (!editingDocument()->updateObject(object, &error)) showError(error); else editingChanged();
        return;
    }
}
void MainWindow::moveObjects(const QSet<QString> &ids, double beats, int x, int y) {
    if (!prepareManualEdit()) return;
    QVector<lmsc::BeatObject> edits;
    for (auto object : editingDocument()->objects()) if (ids.contains(object.id)) {
        object.beat += beats; object.x += x; object.y += y; edits.append(object);
    }
    QString error;
    if (!editingDocument()->updateObjects(edits, &error)) showError(error); else editingChanged();
}
void MainWindow::deleteObjects() {
    if (m_selection.isEmpty() || !prepareManualEdit()) return;
    QString error;
    if (!editingDocument()->removeObjects(selectedIds(), &error)) showError(error);
    else { m_selection.clear(); editingChanged(); }
}
void MainWindow::copyObjects() {
    QString error;
    auto copy = editingDocument()->copyObjects(selectedIds(), &error);
    if (!error.isEmpty()) showError(error);
    else { m_clipboard = copy; statusBar()->showMessage(tr("已复制 %1 个物件").arg(copy.size()), 3000); }
}
void MainWindow::pasteObjects() {
    if (m_clipboard.isEmpty() || !prepareManualEdit()) return;
    double first = m_clipboard.first().beat;
    for (const auto &o : m_clipboard) first = std::min(first, o.beat);
    QString error;
    if (!editingDocument()->pasteObjects(m_clipboard, currentBeat() - first, 0, false, &error)) showError(error);
    else editingChanged();
}
void MainWindow::mirrorObjects() {
    if (!prepareManualEdit()) return;
    QString error;
    if (!editingDocument()->mirrorObjects(selectedIds(), &error)) showError(error); else editingChanged();
}
void MainWindow::calibrateTempo() {
    if (m_busy || m_editorSession->viewReadOnly() || editingDocument() != m_document.get()) return;
    cancelRefinement(true);
    m_analyzeNew = false; m_analyzer->cancel();
    ++m_analysisGeneration;
    QString error;
    if (!m_document->setNewSongTempo(m_bpm->value(), m_offset->value(), &error)) showError(error);
    else { refreshDocument(); statusBar()->showMessage(tr("已更新节拍校准"), 5000); }
}

void MainWindow::newSong() {
    if (m_busy || !confirmDocumentChange()) return;
    const QString path = QFileDialog::getOpenFileName(this, tr("选择新歌音频或视频"), {},
          tr("音乐和视频 (*.mp3 *.mp4 *.m4a *.ogg *.wav *.flac);;所有文件 (*)"));
    if (path.isEmpty()) return;
    m_recropPending = false;
    m_mediaFlow = true;
    ++m_analysisGeneration;
    m_analyzeNew = false; m_analyzer->cancel(); m_audio->cancel(); m_audio->stop();
    setBusy(true, tr("正在读取音轨…"));
    queueAudioTask([this, path] { m_audio->probeMedia(path); });
}

void MainWindow::showNewSongDialog(const MediaInfo &info) {
    setBusy(false);
    if (info.tracks.isEmpty()) { m_mediaFlow = false; restoreDocumentAudio(); showError(tr("媒体没有可用音轨")); return; }
    QDialog dialog(this);
    dialog.setWindowTitle(tr("创建新歌 · 音轨与裁剪"));
    dialog.resize(580, 490);
    auto layout = new QVBoxLayout(&dialog);
    auto form = new QFormLayout;
    auto title = new QLineEdit(QFileInfo(info.path).completeBaseName(), &dialog);
    auto artist = new QLineEdit(&dialog);
    auto mapper = new QLineEdit("LMSC", &dialog);
    auto difficulty = new QComboBox(&dialog);
    difficulty->setObjectName(QStringLiteral("newSongDifficulty"));
    addNewSongDifficultyChoices(difficulty);
    auto cover = new QLineEdit(&dialog);
    auto coverRow = new QWidget(&dialog); auto coverLayout = new QHBoxLayout(coverRow);
    coverLayout->setContentsMargins(0, 0, 0, 0);
    auto coverBrowse = new QPushButton(tr("选择"), coverRow);
    coverLayout->addWidget(cover); coverLayout->addWidget(coverBrowse);
    connect(coverBrowse, &QPushButton::clicked, &dialog, [&] {
        const auto path = QFileDialog::getOpenFileName(&dialog, tr("选择封面"), {}, tr("图片 (*.png *.jpg *.jpeg)"));
        if (!path.isEmpty()) cover->setText(path);
    });
    auto track = new QComboBox(&dialog);
    for (const auto &a : info.tracks)
        track->addItem(tr("音轨 %1 · %2 · %3 声道 %4 %5").arg(a.index).arg(a.codec).arg(a.channels).arg(a.language, a.title), a.index);
    const double duration = info.duration > 0 ? info.duration : info.tracks.first().duration;
    if (duration <= 0) { m_mediaFlow = false; restoreDocumentAudio(); showError(tr("无法取得媒体时长，不能可靠裁剪。")); return; }
    auto start = new QDoubleSpinBox(&dialog), end = new QDoubleSpinBox(&dialog);
    for (auto spin : {start, end}) { spin->setRange(0, duration); spin->setDecimals(3); spin->setSuffix(tr(" 秒")); }
    end->setValue(duration);
    if (m_recropPending) {
        title->setText(m_document->title());
        if (!m_document->difficulties().isEmpty()) {
            const int preset = difficulty->findData(m_document->difficulties().first().name);
            if (preset >= 0) difficulty->setCurrentIndex(preset);
        }
        const int preset = track->findData(m_recropPreset.streamIndex);
        if (preset >= 0) track->setCurrentIndex(preset);
        start->setValue(std::min(duration, m_recropPreset.startSeconds));
        end->setValue(std::min(duration, m_recropPreset.endSeconds));
    }
    m_recropPending = false;
    form->addRow(tr("歌曲名"), title); form->addRow(tr("作者 / 歌手"), artist); form->addRow(tr("谱师"), mapper);
    form->addRow(tr("曲谱难度"), difficulty);
    form->addRow(tr("封面（可选）"), coverRow); form->addRow(tr("提取音轨"), track);
    form->addRow(tr("裁剪起点"), start); form->addRow(tr("裁剪终点"), end);
    layout->addLayout(form);
    auto explanation = new QLabel(tr("默认使用整首，只提取声音。导出为 Ogg/Vorbis。\n"
            "新歌按所选难度创建一张 Standard 曲谱，之后可在左侧调整难度。\n"
            "星穹绿洲的四档名称与映射待实机核验。自动估拍后可以手动校准。"), &dialog);
    explanation->setWordWrap(true); layout->addWidget(explanation);
    auto preview = new QPushButton(tr("试听所选片段"), &dialog);
    auto stop = new QPushButton(tr("停止试听"), &dialog);
    auto previewRow = new QHBoxLayout; previewRow->addWidget(preview); previewRow->addWidget(stop);
    layout->addLayout(previewRow);
    connect(preview, &QPushButton::clicked, &dialog, [&] {
        if (end->value() <= start->value()) { showError(tr("裁剪终点必须晚于起点")); return; }
        m_audio->cancel(); m_audio->stop();
        m_previewStart = start->value(); m_previewEnd = end->value(); m_previewPending = true;
        m_audio->setMetronome(false, 120, 0);
        const int stream = track->currentData().toInt();
        queueAudioTask([this, path = info.path, stream] { m_audio->loadAudio(path, stream); });
    });
    connect(stop, &QPushButton::clicked, m_audio, &AudioService::stop);
    auto buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("转换并创建"));
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        if (title->text().trimmed().isEmpty()) { showError(tr("请填写歌曲名")); return; }
        if (end->value() <= start->value()) { showError(tr("裁剪终点必须晚于起点")); return; }
        if (!cover->text().isEmpty() && QImage(cover->text()).isNull()) { showError(tr("无法读取封面图片")); return; }
        dialog.accept();
    });
    m_inMediaDialog = true;
    const int result = dialog.exec();
    m_inMediaDialog = false; m_previewPending = false;
    m_audio->stop(); m_audio->cancel(); m_progress->hide(); m_cancelButton->hide();
    if (result != QDialog::Accepted) {
        m_mediaFlow = false;
        restoreDocumentAudio();
        return;
    }
    m_newSettings = {title->text().trimmed(), artist->text().trimmed(), mapper->text().trimmed(),
                     cover->text(), info.path, difficulty->currentData().toString(),
                     track->currentData().toInt(), difficulty->currentData(Qt::UserRole + 1).toInt(), start->value(), end->value()};
    m_mediaTemp = std::make_unique<QTemporaryDir>();
    if (!m_mediaTemp->isValid()) { m_mediaFlow = false; restoreDocumentAudio(); showError(tr("无法创建媒体转换临时目录")); return; }
    m_newPending = true;
    setBusy(true, tr("正在裁剪并转换为 Ogg…"));
    const auto settings = m_newSettings;
    const QString output = QDir(m_mediaTemp->path()).filePath("song.ogg");
    queueAudioTask([this, settings, output] {
        m_audio->convertMedia(settings.source, settings.track, settings.start, settings.end, output);
    });
}

void MainWindow::finishNewSong(const QString &output) {
    if (!m_newPending) return;
    m_newPending = false;
    QImage image(m_newSettings.cover);
    if (image.isNull()) {
        image = QImage(512, 512, QImage::Format_RGB32); image.fill(QColor("#1b2941"));
        QPainter painter(&image); painter.setPen(QColor("#55b7ff"));
        QFont font("Microsoft YaHei UI", 38, QFont::Bold); painter.setFont(font);
        painter.drawText(image.rect().adjusted(36, 36, -36, -36), Qt::AlignCenter | Qt::TextWordWrap,
                         m_newSettings.title.left(30));
    } else {
        const int side = std::min(image.width(), image.height());
        image = image.copy((image.width() - side) / 2, (image.height() - side) / 2, side, side)
                     .scaled(512, 512, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    }
    const QString cover = QDir(m_mediaTemp->path()).filePath("cover.png");
    if (!image.save(cover)) { setBusy(false); m_mediaFlow = false; restoreDocumentAudio(); showError(tr("无法保存封面")); return; }
    m_creationLoad = true;
    m_discardLoad = false;
    setBusy(true, tr("正在建立工程和原媒体快照…"));
    const auto settings = m_newSettings;
    m_loader->setFuture(QtConcurrent::run([settings, output, cover] {
        DocumentLoadResult result;
        result.document = std::make_shared<lmsc::BeatmapDocument>();
        if (!result.document->createNew(output, settings.title, 120, 0, cover, &result.error)
            || !result.document->setNewSongMetadata(settings.title, settings.artist, settings.mapper, &result.error)
            || !result.document->setNewSongDifficulty(settings.difficultyName, settings.difficultyRank, &result.error)
            || !result.document->setImportSource(settings.source, settings.track, settings.start, settings.end, &result.error)) {
            if (result.error.isEmpty()) result.error = QStringLiteral("建立新歌失败");
        }
        return result;
    }));
}

void MainWindow::applyRhythm(const RhythmEstimate &estimate) {
    if (!m_analyzeNew || !m_document->isNewSong()) return;
    if (m_busy) {
        const auto origin = m_document;
        const auto generation = m_analysisGeneration;
        QTimer::singleShot(100, this, [this, estimate, origin, generation] {
            if (origin == m_document && generation == m_analysisGeneration) applyRhythm(estimate);
        });
        return;
    }
    m_analyzeNew = false;
    QString error;
    if (!m_document->setNewSongTempo(estimate.bpm, estimate.firstBeatSeconds, &error)) { showError(error); return; }
    refreshDocument();
    m_analysisLabel->setText(tr("建议 %1 BPM，第一拍 %2 秒\n可信度 %3% · %4\n请用节拍器试听并校准。")
            .arg(estimate.bpm, 0, 'f', 3).arg(estimate.firstBeatSeconds, 0, 'f', 3)
            .arg(estimate.confidence * 100, 0, 'f', 0).arg(estimate.message));
}

bool MainWindow::saveProject(bool saveAs) {
    if (m_busy || !m_document->isLoaded()) return false;
    QString path = saveAs ? QString() : m_document->projectPath();
    if (path.isEmpty()) {
        const QString suggested = lmsc::WorkspacePaths::suggestedProjectFile(m_document->title());
        if (suggested.isEmpty()) { showError(tr("无法创建默认工程目录，请检查磁盘空间和写入权限")); return false; }
        path = QFileDialog::getSaveFileName(this, tr("保存独立编辑工程"), suggested,
                                           tr("编辑工程 (*.lmsc)"));
        if (path.isEmpty()) return false;
        if (!path.endsWith(".lmsc", Qt::CaseInsensitive)) path += ".lmsc";
    }
    cancelRefinement();
    m_aiPage->cancelRecognition();
    if (!syncDraftRecords()) return false;
    m_storageBusy = true;
    setBusy(true, tr("正在保存工程与资源…"));
    auto doc = m_document;
    QFutureWatcher<StorageResult> watcher;
    QEventLoop loop;
    connect(&watcher, &QFutureWatcher<StorageResult>::finished, &loop, &QEventLoop::quit);
    watcher.setFuture(QtConcurrent::run([doc, path] {
        StorageResult result; result.ok = doc->saveProject(path, &result.error); return result;
    }));
    m_cancelButton->hide(); // Atomic save is allowed to finish.
    loop.exec();
    const auto result = watcher.result();
    m_storageBusy = false;
    setBusy(false); refreshDocument();
    if (!result.ok) { showError(result.error); return false; }
    m_editorSession->markSaved();
    refreshDocument();
    statusBar()->showMessage(tr("工程已保存；每 30 秒自动保存未保存改动"), 8000);
    return true;
}

void MainWindow::exportSong() {
    if (m_busy || !m_document->isLoaded()) return;
    SongExportDialog dialog(m_mtpExport, m_document->title(), this);
    if (dialog.exec() != QDialog::Accepted) return;
    QString namingError;
    const QString path = lmsc::WorkspacePaths::suggestedSongExportFolder(m_document->title(), dialog.parentDirectory(), &namingError);
    if (path.isEmpty()) { showError(namingError); return; }
    const double leadIn = m_document->isNewSong() ? m_exportLeadIn->value() : 0;
    const QString toolsDirectory = m_audio->toolsDirectory();
    m_storageBusy = true;
    setBusy(true, leadIn > 0 ? tr("正在添加开场缓冲并导出歌曲…") : tr("正在导出完整歌曲目录…"));
    auto doc = m_document;
    QFutureWatcher<StorageResult> watcher;
    QEventLoop loop;
    connect(&watcher, &QFutureWatcher<StorageResult>::finished, &loop, &QEventLoop::quit);
    watcher.setFuture(QtConcurrent::run([doc, path, leadIn, toolsDirectory] {
        StorageResult result;
        result.ok = lmsc::SongExporter::exportSong(*doc, path, leadIn, toolsDirectory, &result.error);
        return result;
    }));
    m_cancelButton->hide();
    loop.exec();
    const auto result = watcher.result();
    if (!result.ok) { m_storageBusy = false; setBusy(false); showError(result.error); return; }
    QString deviceLocation;
    if (dialog.toDevice()) {
        QString transferError;
        bool uploadComplete = false, uploadCancelled = false, cancelRequested = false;
        QEventLoop transferLoop;
        QProgressDialog progress(tr("正在向头显新增歌曲并校验…"), tr("取消上传"), 0, 100, this);
        progress.setWindowTitle(tr("导出到 PICO")); progress.setWindowModality(Qt::WindowModal);
        progress.setMinimumDuration(0); progress.setAutoClose(false); progress.setAutoReset(false);
        const auto uploaded = connect(m_mtpExport, &MtpExportService::songUploaded, &transferLoop,
            [&](const QString &, const QString &location) { uploadComplete = true; deviceLocation = location; transferLoop.quit(); });
        const auto failed = connect(m_mtpExport, &MtpExportService::errorOccurred, &transferLoop,
            [&](const QString &error) { transferError = error; transferLoop.quit(); });
        const auto cancelled = connect(m_mtpExport, &MtpExportService::cancelled, &transferLoop,
            [&] { uploadCancelled = true; transferLoop.quit(); });
        const auto advanced = connect(m_mtpExport, &MtpExportService::taskProgress, &progress,
            [&](const QString &task, int percent) { progress.setLabelText(task); progress.setRange(0, percent < 0 ? 0 : 100); if (percent >= 0) progress.setValue(percent); });
        connect(&progress, &QProgressDialog::canceled, &transferLoop, [&] {
            cancelRequested = true;
            if (m_mtpExport->isBusy()) m_mtpExport->cancel();
            else { uploadCancelled = true; transferLoop.quit(); }
        });
        progress.show();
        QTimer::singleShot(0, &transferLoop, [&] {
            if (cancelRequested) { transferLoop.quit(); return; }
            m_mtpExport->uploadSong(path, dialog.destination());
        });
        transferLoop.exec();
        disconnect(uploaded); disconnect(failed); disconnect(cancelled); disconnect(advanced);
        progress.close();
        if (!uploadComplete) {
            m_storageBusy = false; setBusy(false);
            QString reason = transferError;
            if (reason.isEmpty()) reason = uploadCancelled ? tr("已取消头显上传。") : tr("头显上传未完成。");
            showError(reason + tr("\n完整电脑副本仍保留在：\n%1").arg(QDir::toNativeSeparators(path)));
            return;
        }
    }
    m_storageBusy = false; setBusy(false);
    const QString bufferNote = leadIn > 0
        ? tr("\n已添加 %1 秒开场静音，音符、炸弹和墙同步后移。工程中的音频与拍点不变。\n").arg(leadIn, 0, 'f', 3)
        : QString();
    QMessageBox message(QMessageBox::Information, tr("导出完成"),
        tr("歌曲目录：\n%1\n%2\n手动复制整个歌曲目录到对应游戏的目录：\n"
           "星穹绿洲：SoulTopia\\BeatNote\\Custom\\光剑曲谱制作\n"
           "光之乐团：Android\\data\\com.StarRiverVR.LightBand\\files\\CustomMusic\\光剑曲谱制作\n\n"
           "在目标游戏中检查分类识别、声音、难度和开头物件。")
            .arg(QDir::toNativeSeparators(path), bufferNote)
            + (deviceLocation.isEmpty() ? QString() : tr("\n\n设备传输与回读校验已完成：\n%1").arg(deviceLocation)), QMessageBox::Ok, this);
    auto open = message.addButton(tr("打开歌曲目录"), QMessageBox::ActionRole);
    connect(open, &QPushButton::clicked, &message, [path] { QDesktopServices::openUrl(QUrl::fromLocalFile(path)); });
    message.exec();
}

bool MainWindow::confirmDocumentChange() {
    cancelRefinement();
    syncDraftRecords();
    if (!m_document->isModified() && !m_editorSession->isDirty()) return true;
    const auto answer = QMessageBox::question(this, tr("保存当前改动"),
       tr("当前工程或工作草稿有未保存的改动。是否先保存？"),
       QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
    if (answer == QMessageBox::Cancel) return false;
    return answer == QMessageBox::Discard || saveProject();
}
void MainWindow::showError(const QString &message) {
    statusBar()->showMessage(message, 30000);
    if (!m_testMode) QMessageBox::warning(this, tr("操作未完成"), message);
}
void MainWindow::setBusy(bool busy, const QString &message) {
    if (!busy && (m_storageBusy || m_loader->isRunning())) return;
    m_busy = busy;
    m_saveAction->setEnabled(!busy && m_document->isLoaded());
    m_exportAction->setEnabled(!busy && m_document->isLoaded());
    m_difficulties->setEnabled(!busy);
    m_newDifficultySelector->setEnabled(!busy && m_document->isNewSong());
    m_exportLeadIn->setEnabled(!busy && m_document->isNewSong());
    m_grid->setEnabled(!busy && editingDocument()->isLoaded() && editingDocument()->readOnlyReason().isEmpty() && !m_editorSession->viewReadOnly());
    const bool formalView = m_editorSession->view() == lmsc::EditorSessionController::View::Formal
        && !m_editorSession->viewReadOnly() && editingDocument() == m_document.get();
    m_newDifficultySelector->setEnabled(!busy && m_document->isNewSong() && formalView);
    m_bpm->setEnabled(!busy && m_document->isNewSong() && formalView);
    m_offset->setEnabled(!busy && m_document->isNewSong() && formalView);
    m_calibrateButton->setEnabled(!busy && m_document->isNewSong() && formalView);
    m_estimateButton->setEnabled(!busy && m_document->isNewSong() && m_audio->isReady() && !m_analyzer->isBusy() && formalView);
    m_recropButton->setEnabled(!busy && m_document->isNewSong() && m_document->importSource().isAvailable());
    m_playButton->setEnabled(!busy && isAudioReady());
    m_speed->setEnabled(!busy && isAudioReady());
    m_undoAction->setEnabled(!busy && m_document->canUndo()); m_redoAction->setEnabled(!busy && m_document->canRedo());
    if (busy) { m_progress->setRange(0, 0); m_progress->show(); m_cancelButton->show(); }
    else { m_progress->hide(); m_cancelButton->hide(); }
    refreshSelection();
    refreshRecognitionContext();
    updateWorkspaceActions();
    if (!message.isEmpty()) statusBar()->showMessage(message);
}
void MainWindow::closeEvent(QCloseEvent *event) {
    if (m_busy) { statusBar()->showMessage(tr("请等待当前任务完成或先取消任务"), 8000); event->ignore(); return; }
    if (!m_testMode && !confirmDocumentChange()) { event->ignore(); return; }
    cancelRefinement();
    m_analyzeNew = m_newPending = false;
    ++m_analysisGeneration; ++m_audioGeneration; m_queuedAudio = false;
    m_analyzer->cancel(); m_audio->cancel(); m_audio->stop();
    m_mtp->cancel();
    m_aiPage->cancelRecognition();
    m_settingsPanel->discardChanges();
    event->accept();
}
bool MainWindow::isAudioReady() const { return m_document->isLoaded() && !m_queuedAudio && m_audio->isReady() && !m_audio->isBusy(); }

void MainWindow::queueAudioTask(std::function<void()> operation) {
    const quint64 generation = ++m_audioGeneration;
    m_queuedAudio = true;
    refreshRecognitionContext();
    m_audio->cancel();
    auto timer = new QTimer(this);
    timer->setInterval(25);
    connect(timer, &QTimer::timeout, this, [this, timer, generation, operation] {
        if (generation != m_audioGeneration) { timer->stop(); timer->deleteLater(); return; }
        if (m_audio->isBusy()) return;
        timer->stop(); timer->deleteLater();
        m_queuedAudio = false;
        operation();
    });
    timer->start();
}

void MainWindow::restoreDocumentAudio() {
    if (!m_document->isLoaded()) { ++m_audioGeneration; m_queuedAudio = false; return; }
    const QString path = m_document->audioPath();
    queueAudioTask([this, path] { m_audio->loadAudio(path); });
    m_audio->setMetronome(m_metronome->isChecked(), m_document->timeMap().baseBpm(),
                         m_document->timeMap().firstBeatSeconds());
}

void MainWindow::requestRhythmAnalysis() {
    const auto document = m_document;
    const quint64 generation = ++m_analysisGeneration;
    disconnect(m_rhythmConnection);
    m_analyzeNew = true;
    m_analyzer->cancel();
    m_estimateButton->setEnabled(false);
    m_analysisLabel->setText(tr("正在分析节拍…"));
    auto timer = new QTimer(this);
    timer->setInterval(30);
    connect(timer, &QTimer::timeout, this, [this, timer, document, generation] {
        if (document != m_document || generation != m_analysisGeneration || !m_analyzeNew) {
            timer->stop(); timer->deleteLater(); return;
        }
        if (m_analyzer->isBusy()) return;
        timer->stop(); timer->deleteLater();
        m_rhythmConnection = connect(m_analyzer, &RhythmAnalyzer::analysisFinished, this,
                                     [this, document, generation](const RhythmEstimate &estimate) {
            if (document == m_document && generation == m_analysisGeneration) applyRhythm(estimate);
        });
        m_analyzer->analyze(m_audio->pcmCachePath());
    });
    timer->start();
}

bool MainWindow::runEditorCheck(const QString &outputFolder, QString *error) {
    if (!m_document->isLoaded()) { *error = tr("未加载文档"); return false; }
    const int before = m_document->objects().size();
    lmsc::BeatObject object; object.beat = 0.125; object.x = 3; object.y = 2; object.direction = 1;
    if (!m_document->addObject(object, error)) return false;
    refreshDocument();
    if (m_document->objects().size() != before + 1 || !m_document->undo()
        || m_document->objects().size() != before || !m_document->redo()
        || m_document->objects().size() != before + 1) { *error = tr("GUI 编辑/撤销/重做检查失败"); return false; }
    QDir().mkpath(outputFolder);
    if (!m_document->saveProject(QDir(outputFolder).filePath("smoke.lmsc"), error)
        || !m_document->exportSong(QDir(outputFolder).filePath("export"), error)) return false;
    lmsc::BeatmapDocument reopened;
    if (!reopened.loadProject(QDir(outputFolder).filePath("smoke.lmsc"), error)
        || reopened.objects().size() != before + 1) { if (error->isEmpty()) *error = tr("工程恢复数量不一致"); return false; }
    refreshDocument();
    return true;
}
