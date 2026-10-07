#pragma once

#include "core/AiGenerationService.h"
#include "core/RefinementTypes.h"
#include <QJsonValue>
#include <QObject>
#include <memory>

namespace lmsc {

// Owns editable candidates, never the formal document or its audio resources.
class EditorSessionController final : public QObject {
    Q_OBJECT
public:
    enum class View { Formal, Working, Initial, BeforeRefinement };
    Q_ENUM(View)

    explicit EditorSessionController(QObject *parent = nullptr);
    ~EditorSessionController() override;

    // Attaching also restores editorDraftRecords, including read-only stale rows.
    void setFormalDocument(BeatmapDocument *document, const QString &documentId = {});
    BeatmapDocument *formalDocument() const;
    BeatmapDocument *activeDocument() const;
    BeatmapDocument *workingDocument() const;
    const BeatmapDocument *initialDocument() const;
    const BeatmapDocument *beforeRefinementDocument() const;

    QString currentTargetKey() const;
    QStringList targetKeys() const;
    bool selectTarget(const QString &difficultyName, QString *error = nullptr);
    View view() const;
    bool setView(View view, QString *error = nullptr);
    bool viewReadOnly() const;
    bool hasDraft() const;
    bool hasBeforeRefinement() const;
    bool isApplied() const;
    bool isManualModified() const;
    bool isApplicable(QString *reason = nullptr) const;
    bool canApplyDraft(QString *reason = nullptr) const;
    QStringList warnings() const;
    GenerationDraft generationDraft() const;

    bool acceptGenerationDraft(const GenerationDraft &draft, QString *error = nullptr);
    // Copies the selected formal chart as a candidate before starting refinement.
    bool beginRefinementDraft(const GenerationRequest &source, QString *error = nullptr);
    // A continuation keeps the first before-refinement snapshot and source hash.
    bool beginRefinement(bool continuation = false, QString *error = nullptr);
    bool mergeRefinementResult(const RefinementResult &result, QString *error = nullptr);
    bool restoreInitial(QString *error = nullptr);
    bool applyDraft(QString *error = nullptr);
    bool discardDraft(QString *error = nullptr);

    bool canUndo() const;
    bool canRedo() const;
    bool undo();
    bool redo();
    // Call after GridEditor/TimelineView modifies workingDocument directly.
    void notifyWorkingChanged();

    // Version 2 stores object snapshots and resource fingerprints, never PCM or
    // network resume state. Malformed records are retained without being applied.
    QJsonValue toJson() const;
    bool restoreFromJson(const QJsonValue &records, QStringList *warnings = nullptr);
    bool synchronizeRecords(QString *error = nullptr);
    bool isDirty() const;
    void markSaved();
    QJsonValue serializedCheckpoint() const;

signals:
    void changed();

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace lmsc
