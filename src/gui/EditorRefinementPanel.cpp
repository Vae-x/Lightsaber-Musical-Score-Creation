#include "EditorRefinementPanel.h"
#include "TaskProgressView.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QGridLayout>
#include <QLabel>
#include <QPushButton>
#include <QProgressBar>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QVBoxLayout>
#include <cmath>

namespace lmsc {
namespace {
QLabel *textLabel(const QString &text, QWidget *parent) {
    auto label = new QLabel(text, parent);
    label->setTextFormat(Qt::PlainText);
    label->setWordWrap(true);
    return label;
}
}

EditorRefinementPanel::EditorRefinementPanel(QWidget *parent) : QWidget(parent) {
    qRegisterMetaType<Version>();
    setObjectName(QStringLiteral("editorRefinementPanel"));
    auto outer = new QVBoxLayout(this);
    outer->setContentsMargins(8, 6, 8, 6);
    outer->setSpacing(6);
    outer->setSizeConstraint(QLayout::SetMinimumSize);
    auto title = textLabel(tr("工作稿与 AI 精修"), this);
    title->setProperty("role", "title");
    outer->addWidget(title);
    auto scroll = new QScrollArea(this);
    scroll->setObjectName(QStringLiteral("editorRefinementDetailsScroll"));
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    // Details may shrink and scroll; the wrapped stage and action rows retain
    // their minimum height when generation shares this narrow editor column.
    scroll->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Ignored);
    scroll->setMinimumHeight(0);
    scroll->setMaximumHeight(245);
    auto content = new QWidget(scroll);
    auto layout = new QVBoxLayout(content);
    layout->setContentsMargins(0, 0, 6, 0);
    layout->setSpacing(6);
    layout->setSizeConstraint(QLayout::SetMinimumSize);
    m_summary = textLabel({}, content);
    m_summary->setObjectName(QStringLiteral("editorCandidateSummary"));
    layout->addWidget(m_summary);
    layout->addWidget(textLabel(tr("候选可在曲谱编辑区手动修改，确认后应用。每批精修最多 5 个重点乐句。"), content));
    m_version = new QComboBox(content);
    m_version->setObjectName(QStringLiteral("editorCandidateVersion"));
    layout->addWidget(textLabel(tr("查看版本"), content));
    layout->addWidget(m_version);
    m_selectedOnly = new QCheckBox(tr("仅精修选段（Shift 拖动时间轴）"), content);
    m_selectedOnly->setObjectName(QStringLiteral("editorRefinementSelectedOnly"));
    layout->addWidget(m_selectedOnly);
    auto range = new QGridLayout;
    range->addWidget(textLabel(tr("起点"), content), 0, 0);
    range->addWidget(textLabel(tr("终点"), content), 0, 1);
    m_start = new QDoubleSpinBox(content);
    m_end = new QDoubleSpinBox(content);
    m_start->setObjectName(QStringLiteral("editorRefinementStartSeconds"));
    m_end->setObjectName(QStringLiteral("editorRefinementEndSeconds"));
    for (auto spin : {m_start, m_end}) {
        spin->setRange(0, 0);
        spin->setDecimals(3);
        spin->setSuffix(tr(" 秒"));
        spin->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    }
    range->addWidget(m_start, 1, 0);
    range->addWidget(m_end, 1, 1);
    layout->addLayout(range);
    auto actions = new QGridLayout;
    m_refine = new QPushButton(tr("精修重点乐句"), content);
    m_resume = new QPushButton(tr("继续精修"), content);
    m_restore = new QPushButton(tr("恢复初稿"), content);
    m_refine->setObjectName(QStringLiteral("editorRefineButton"));
    m_resume->setObjectName(QStringLiteral("editorRefinementResume"));
    m_restore->setObjectName(QStringLiteral("editorRestoreInitial"));
    actions->addWidget(m_refine, 0, 0);
    actions->addWidget(m_resume, 0, 1);
    actions->addWidget(m_restore, 1, 0);
    layout->addLayout(actions);
    m_stats = textLabel({}, content);
    m_stats->setObjectName(QStringLiteral("editorRefinementStats"));
    layout->addWidget(m_stats);
    layout->addStretch();
    scroll->setWidget(content);
    outer->addWidget(scroll, 1);

    // Task state and cancellation are siblings of the scroll area. Long
    // processed-range lists cannot push their percentage below its viewport.
    m_task = new TaskProgressView(this);
    m_task->setObjectName(QStringLiteral("editorRefinementTaskProgress"));
    m_task->progressBar()->setObjectName(QStringLiteral("editorRefinementProgress"));
    m_task->stageLabel()->setObjectName(QStringLiteral("editorRefinementStatus"));
    outer->addWidget(m_task);
    m_cancel = new QPushButton(tr("取消精修"), this);
    m_cancel->setObjectName(QStringLiteral("editorRefinementCancel"));
    outer->addWidget(m_cancel);
    auto confirmation = new QGridLayout;
    m_apply = new QPushButton(tr("确认应用工作稿"), this);
    m_discard = new QPushButton(tr("舍弃工作稿"), this);
    m_apply->setObjectName(QStringLiteral("editorCandidateApply"));
    m_discard->setObjectName(QStringLiteral("editorCandidateDiscard"));
    confirmation->addWidget(m_apply, 0, 0);
    confirmation->addWidget(m_discard, 0, 1);
    outer->addLayout(confirmation);

    connect(m_refine, &QPushButton::clicked, this, [this] {
        if (!m_refine->isEnabled()) return;
        const bool selected = m_selectedOnly->isChecked();
        emit refineRequested(selected, selected ? m_start->value() : 0,
                             selected ? m_end->value() : m_state.durationSeconds);
    });
    connect(m_resume, &QPushButton::clicked, this, &EditorRefinementPanel::resumeRequested);
    connect(m_cancel, &QPushButton::clicked, this, &EditorRefinementPanel::cancelRequested);
    connect(m_restore, &QPushButton::clicked, this, &EditorRefinementPanel::restoreInitialRequested);
    connect(m_apply, &QPushButton::clicked, this, &EditorRefinementPanel::applyRequested);
    connect(m_discard, &QPushButton::clicked, this, &EditorRefinementPanel::discardRequested);
    connect(m_version, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int index) {
        if (index >= 0) emit versionRequested(static_cast<Version>(m_version->itemData(index).toInt()));
    });
    connect(m_selectedOnly, &QCheckBox::toggled, this, [this] { refreshControls(); });
    for (auto spin : {m_start, m_end})
        connect(spin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this] { refreshControls(); });
    setState({});
}

void EditorRefinementPanel::setState(const State &state) {
    m_state = state;
    m_state.durationSeconds = std::isfinite(state.durationSeconds) ? qMax(0.0, state.durationSeconds) : 0;
    const QSignalBlocker startBlock(m_start), endBlock(m_end), versionBlock(m_version);
    const bool wasFullRange = m_end->maximum() == m_end->value();
    m_start->setMaximum(m_state.durationSeconds);
    m_end->setMaximum(m_state.durationSeconds);
    if (wasFullRange && !m_selectedOnly->isChecked()) m_end->setValue(m_state.durationSeconds);
    m_version->clear();
    m_version->addItem(tr("正式谱"), Formal);
    if (state.hasCandidate) {
        m_version->addItem(tr("工作稿（可编辑）"), Working);
        m_version->addItem(tr("生成初稿（只读比较）"), Initial);
        if (state.hasRefinement) m_version->addItem(tr("精修前版本（只读比较）"), BeforeRefinement);
    }
    const int selected = m_version->findData(state.version);
    m_version->setCurrentIndex(selected >= 0 ? selected : 0);
    QString summary = state.summary;
    if (state.manualModified) summary += (summary.isEmpty() ? QString() : QStringLiteral("\n")) + tr("工作稿包含手动修改。");
    if (!state.unavailableReason.isEmpty()) summary += (summary.isEmpty() ? QString() : QStringLiteral("\n")) + state.unavailableReason;
    m_summary->setText(summary);
    refreshControls();
}

void EditorRefinementPanel::setResult(const RefinementResult &result) {
    QString details = tr("修改 %1 · 移动拍点 %2 · 新增 %3 · 删除 %4\n已检查 %5 个乐句 · 剩余 %6 个乐句")
        .arg(result.stats.changed).arg(result.stats.moved).arg(result.stats.added).arg(result.stats.removed)
        .arg(result.processedSegments.size()).arg(result.remainingSegments.size());
    if (result.source.selectedOnly) details += tr("\n选段 %1–%2 秒")
        .arg(result.source.startSeconds, 0, 'f', 3).arg(result.source.endSeconds, 0, 'f', 3);
    if (!result.processedRanges.isEmpty()) details += tr("\n实际已精修：%1").arg(result.processedRanges.join(QStringLiteral("；")));
    m_stats->setText(details);
}

void EditorRefinementPanel::clearResult() {
    m_stats->clear();
    m_task->clear();
}

void EditorRefinementPanel::setSelection(double startSeconds, double endSeconds, bool selectedOnly) {
    if (!std::isfinite(startSeconds) || !std::isfinite(endSeconds)) return;
    const QSignalBlocker startBlock(m_start), endBlock(m_end), selectedBlock(m_selectedOnly);
    m_start->setValue(qBound(0.0, startSeconds, m_state.durationSeconds));
    m_end->setValue(qBound(0.0, endSeconds, m_state.durationSeconds));
    m_selectedOnly->setChecked(selectedOnly);
    refreshControls();
}

void EditorRefinementPanel::setProgress(int percent, const QString &stage) {
    m_task->stageLabel()->setProperty("role", "status");
    m_task->setProgress(percent, stage);
}

void EditorRefinementPanel::showError(const QString &message) {
    m_task->stageLabel()->setProperty("role", "error");
    m_task->setStage(message);
}

void EditorRefinementPanel::refreshControls() {
    const bool rangeValid = !m_selectedOnly->isChecked() || m_end->value() > m_start->value();
    const bool workingView = m_state.version == Working || m_state.version == Formal;
    m_refine->setEnabled(m_state.canRefine && m_state.available && m_state.sourceValid
                         && !m_state.running && rangeValid && workingView);
    m_resume->setVisible(m_state.resumable);
    m_resume->setEnabled(m_state.resumable && m_state.available && m_state.sourceValid && !m_state.running);
    m_cancel->setVisible(m_state.running);
    m_cancel->setEnabled(m_state.running);
    m_restore->setEnabled(m_state.hasCandidate && !m_state.running);
    m_apply->setEnabled(m_state.canApply && m_state.hasCandidate && m_state.sourceValid && !m_state.running);
    m_discard->setEnabled(m_state.hasCandidate && !m_state.running);
    m_version->setEnabled(m_state.hasCandidate);
    m_selectedOnly->setEnabled(!m_state.running);
    for (auto spin : {m_start, m_end}) spin->setEnabled(m_selectedOnly->isChecked() && !m_state.running);
}

} // namespace lmsc
