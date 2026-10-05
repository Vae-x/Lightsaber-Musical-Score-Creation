#pragma once

#include "core/RefinementTypes.h"
#include <QWidget>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QPushButton;

namespace lmsc {
class TaskProgressView;

class EditorRefinementPanel : public QWidget {
    Q_OBJECT
public:
    enum Version { Formal, Working, Initial, BeforeRefinement };
    Q_ENUM(Version)
    struct State {
        bool available = false;
        bool sourceValid = false;
        bool hasCandidate = false;
        bool running = false;
        bool resumable = false;
        bool hasRefinement = false;
        bool canRefine = false;
        bool canApply = false;
        bool manualModified = false;
        double durationSeconds = 0;
        Version version = Working;
        QString unavailableReason;
        QString summary;
    };
    explicit EditorRefinementPanel(QWidget *parent = nullptr);
    void setState(const State &state);
    void setResult(const RefinementResult &result);
    void clearResult();
    void setSelection(double startSeconds, double endSeconds, bool selectedOnly = true);
    void setProgress(int percent, const QString &stage);
    void showError(const QString &message);
    TaskProgressView *taskProgress() const { return m_task; }
    const State &state() const { return m_state; }
signals:
    void refineRequested(bool selectedOnly, double startSeconds, double endSeconds);
    void resumeRequested();
    void cancelRequested();
    void versionRequested(lmsc::EditorRefinementPanel::Version version);
    void restoreInitialRequested();
    void applyRequested();
    void discardRequested();
private:
    void refreshControls();
    State m_state;
    TaskProgressView *m_task;
    QCheckBox *m_selectedOnly;
    QDoubleSpinBox *m_start;
    QDoubleSpinBox *m_end;
    QComboBox *m_version;
    QLabel *m_summary;
    QLabel *m_stats;
    QPushButton *m_refine;
    QPushButton *m_resume;
    QPushButton *m_cancel;
    QPushButton *m_restore;
    QPushButton *m_apply;
    QPushButton *m_discard;
};
} // namespace lmsc

Q_DECLARE_METATYPE(lmsc::EditorRefinementPanel::Version)
