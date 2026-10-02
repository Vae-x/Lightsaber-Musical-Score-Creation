#pragma once

#include <QMainWindow>
#include <QFutureWatcher>
#include <QSet>
#include <memory>
#include <functional>
#include "core/BeatmapDocument.h"

namespace Ui { class MainWindow; }
class AudioService;
class RhythmAnalyzer;
class MtpImportService;
struct MediaInfo;
struct RhythmEstimate;
class TrackView;
class GridEditor;
class TimelineView;
class QLabel;
class QListWidget;
class QComboBox;
class QDoubleSpinBox;
class QSpinBox;
class QPushButton;
class QCheckBox;
class QProgressBar;
class QTimer;
class QTemporaryDir;
class QSlider;
class QAction;
class QStackedWidget;
class QToolBar;
namespace lmsc {
class NavigationSidebar;
class SettingsPanel;
class AiRecognitionPage;
class AiRecognitionService;
}
struct DocumentLoadResult {
    std::shared_ptr<lmsc::BeatmapDocument> document;
    QString error;
};

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr, const QString &settingsFile = {});
    ~MainWindow() override;
    void openPath(const QString &path);
    void setTestMode(bool enabled) { m_testMode = enabled; }
    bool isAudioReady() const;
    bool runEditorCheck(const QString &outputFolder, QString *error);
    // The caller owns the recognition backend and keeps it in the GUI thread.
    // A backend may dispatch its analysis to workers; none is connected by default.
    void setAiRecognitionService(lmsc::AiRecognitionService *service);
signals:
    void documentReady();
    void loadFailed(const QString &error);
protected:
    void closeEvent(QCloseEvent *event) override;
private:
    void buildEditor();
    void buildWorkspace();
    void selectWorkspacePage(int row);
    void refreshRecognitionContext();
    void updateWorkspaceActions();
    void buildActions();
    void connectAudio();
    void refreshDocument();
    void refreshSelection();
    void replaceDocument(std::shared_ptr<lmsc::BeatmapDocument> document);
    void selectObjects(const QSet<QString> &ids);
    void seek(double seconds);
    double currentBeat() const;
    QStringList selectedIds() const;
    void newSong();
    void importSongFolder();
    void showAbout();
    void showSettings();
    void showNewSongDialog(const MediaInfo &info);
    void finishNewSong(const QString &output);
    void applyRhythm(const RhythmEstimate &estimate);
    void addObject(double beat, int x, int y);
    void applyProperties();
    void moveObjects(const QSet<QString> &ids, double beats, int x, int y);
    void deleteObjects();
    void copyObjects();
    void pasteObjects();
    void mirrorObjects();
    void calibrateTempo();
    bool saveProject(bool saveAs = false);
    void exportSong();
    bool confirmDocumentChange();
    void showError(const QString &message);
    void setBusy(bool busy, const QString &message = {});
    bool editorCommandAllowed() const;
    void queueAudioTask(std::function<void()> operation);
    void requestRhythmAnalysis();
    void restoreDocumentAudio();

    Ui::MainWindow *ui;
    QString m_settingsFile;
    QWidget *m_editorPage = nullptr;
    QStackedWidget *m_workspacePages = nullptr;
    lmsc::NavigationSidebar *m_sidebar = nullptr;
    lmsc::SettingsPanel *m_settingsPanel = nullptr;
    lmsc::AiRecognitionPage *m_aiPage = nullptr;
    QToolBar *m_editorToolbar = nullptr;
    std::shared_ptr<lmsc::BeatmapDocument> m_document;
    QVector<lmsc::BeatObject> m_clipboard;
    QSet<QString> m_selection;
    AudioService *m_audio;
    RhythmAnalyzer *m_analyzer;
    MtpImportService *m_mtp;
    QFutureWatcher<DocumentLoadResult> *m_loader;
    TrackView *m_track;
    GridEditor *m_grid;
    TimelineView *m_timeline;
    QLabel *m_songLabel, *m_projectLabel, *m_summaryLabel, *m_protectionLabel;
    QLabel *m_positionLabel, *m_analysisLabel;
    QListWidget *m_difficulties;
    QComboBox *m_placeType, *m_placeColor, *m_placeDirection;
    QComboBox *m_editColor, *m_editDirection, *m_snap, *m_speed;
    QDoubleSpinBox *m_bpm, *m_offset, *m_editBeat, *m_editDuration;
    QSpinBox *m_editX, *m_editY, *m_editWidth, *m_editHeight;
    QPushButton *m_playButton, *m_applyButton, *m_cancelButton, *m_calibrateButton, *m_estimateButton, *m_recropButton;
    QCheckBox *m_loop, *m_metronome;
    QProgressBar *m_progress;
    QSlider *m_seekSlider;
    QTimer *m_autosave;
    QAction *m_saveAction, *m_exportAction, *m_undoAction, *m_redoAction;
    std::unique_ptr<QTemporaryDir> m_mediaTemp;
    struct NewSongSettings {
        QString title, artist, mapper, cover, source;
        int track = 0;
        double start = 0, end = 0;
    } m_newSettings;
    bool m_busy = false, m_refreshing = false, m_testMode = false;
    bool m_newPending = false, m_analyzeNew = false, m_discardLoad = false;
    bool m_previewPending = false, m_inMediaDialog = false, m_creationLoad = false, m_recropPending = false, m_initializeAudio = true;
    bool m_queuedAudio = false;
    bool m_storageBusy = false, m_mediaFlow = false;
    bool m_mtpImportPending = false;
    quint64 m_audioGeneration = 0;
    quint64 m_analysisGeneration = 0;
    QMetaObject::Connection m_rhythmConnection;
    lmsc::ImportSource m_recropPreset;
    double m_loopStart = 0, m_loopEnd = 0, m_previewStart = 0, m_previewEnd = 0;
};
