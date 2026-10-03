#include "GenerationPreviewDialog.h"
#include "EditorViews.h"
#include "core/AudioService.h"
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QSplitter>
#include <QVBoxLayout>
#include <QVariant>

namespace lmsc {
GenerationPreviewDialog::GenerationPreviewDialog(const GenerationDraft &draft, int replacedObjectCount,
                                                 AudioService *audio, QWidget *parent, bool addsDifficulty)
    : QDialog(parent), m_draft(draft), m_audio(audio), m_timeline(new TimelineView(this)),
      m_track(new TrackView(this)), m_status(new QLabel(this)), m_play(new QPushButton(this)) {
    setObjectName(QStringLiteral("generationPreviewDialog"));
    setWindowTitle(tr("候选曲谱预览"));
    setWindowModality(Qt::WindowModal);
    setAttribute(Qt::WA_DeleteOnClose);
    resize(1080, 760);
    auto layout = new QVBoxLayout(this);
    layout->setContentsMargins(14, 12, 14, 12);
    layout->setSpacing(8);
    auto title = new QLabel(tr("%1 · 候选曲谱").arg(draft.source.profile.name), this);
    title->setProperty("role", "title");
    layout->addWidget(title);
    auto details = new QScrollArea(this);
    details->setObjectName(QStringLiteral("generationPreviewDetails"));
    details->setFrameShape(QFrame::NoFrame);
    details->setWidgetResizable(true);
    details->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    details->setMinimumHeight(72);
    details->setMaximumHeight(125);
    auto detailsContent = new QWidget(details);
    auto detailsLayout = new QVBoxLayout(detailsContent);
    detailsLayout->setContentsMargins(0, 0, 6, 0);
    detailsLayout->setSpacing(6);
    details->setWidget(detailsContent);
    layout->addWidget(details);
    int directional = 0, dots = 0, bombs = 0, walls = 0;
    QVector<EditorObject> objects;
    for (const auto &source : draft.objects) {
        EditorObject object;
        object.id = QStringLiteral("candidate:%1").arg(objects.size());
        object.beat = source.beat; object.type = static_cast<int>(source.kind);
        object.x = source.x; object.y = source.y; object.color = source.color; object.direction = source.direction;
        object.duration = source.duration; object.width = source.width; object.height = source.height;
        object.locked = source.isProtected(); object.protectedReason = source.protectedReason;
        objects.append(object);
        if (source.kind == ObjectKind::Bomb) ++bombs;
        else if (source.kind == ObjectKind::Wall) ++walls;
        else if (source.direction == 8) ++dots;
        else ++directional;
    }
    auto counts = new QLabel(tr("方向 %1 · 无方向 %2 · 炸弹 %3 · 墙 %4\n平均每秒 %5 个音符 · 峰值 %6")
        .arg(directional).arg(dots).arg(bombs).arg(walls)
        .arg(draft.metrics.averageNps, 0, 'f', 2).arg(draft.metrics.peakNps, 0, 'f', 2), detailsContent);
    counts->setObjectName(QStringLiteral("generationPreviewCounts"));
    counts->setWordWrap(true);
    detailsLayout->addWidget(counts);
    const QString application = addsDifficulty
        ? tr("应用后将添加 %1 难度，保留其他难度；全部变化可一次撤销。").arg(draft.source.profile.name)
        : tr("应用后将替换 %1 难度的 %2 个物件，保留其他难度；全部变化可一次撤销。")
            .arg(draft.source.profile.name).arg(replacedObjectCount);
    auto notice = new QLabel(application+tr("\n工程音频和时间保持原样，开场缓冲仍在导出时添加。"), detailsContent);
    notice->setObjectName(QStringLiteral("generationReplaceNotice"));
    notice->setWordWrap(true);
    detailsLayout->addWidget(notice);
    if (!draft.warnings.isEmpty()) {
        auto warnings = new QLabel(draft.warnings.join(QStringLiteral("\n")), detailsContent);
        warnings->setTextFormat(Qt::PlainText);
        warnings->setObjectName(QStringLiteral("generationPreviewWarnings"));
        warnings->setWordWrap(true);
        warnings->setProperty("role", "warning");
        detailsLayout->addWidget(warnings);
    }
    m_track->setObjectName(QStringLiteral("generationPreviewTrack"));
    m_timeline->setObjectName(QStringLiteral("generationPreviewTimeline"));
    m_timeline->setReadOnly(true);
    // Preview panes can shrink without changing the full editor's minimums.
    m_track->setMinimumSize(300, 160);
    m_timeline->setMinimumHeight(190);
    const auto time = draft.source.timeMap;
    const auto toSeconds = [time](double beat) { return time.beatToSeconds(beat); };
    const auto toBeat = [time](double seconds) { return time.secondsToBeat(seconds); };
    m_track->setTimeMapping(toSeconds, toBeat);
    m_timeline->setTimeMapping(toSeconds, toBeat);
    m_track->setObjects(objects);
    m_timeline->setObjects(objects);
    m_timeline->setDuration(draft.source.audio.durationSeconds);
    if (audio) m_timeline->setWaveform(audio->waveform(), draft.source.audio.durationSeconds);
    auto split = new QSplitter(Qt::Vertical, this);
    split->addWidget(m_track);
    split->addWidget(m_timeline);
    split->setStretchFactor(0, 2);
    split->setStretchFactor(1, 1);
    layout->addWidget(split, 1);
    auto transport = new QHBoxLayout;
    m_play->setObjectName(QStringLiteral("generationPreviewPlay"));
    m_play->setText(audio && audio->isPlaying() ? tr("暂停") : tr("试听"));
    m_play->setEnabled(audio && audio->isReady());
    transport->addWidget(m_play);
    transport->addWidget(new QLabel(tr("点击时间轴跳转 · 滚轮缩放 · 中键拖动平移"), this));
    transport->addStretch();
    layout->addLayout(transport);
    if (audio) {
        m_initialPosition = audio->position();
        m_initialPlaying = audio->isPlaying();
        m_initialLoopEnabled = audio->loopEnabled();
        m_initialLoopStart = audio->loopStartSeconds();
        m_initialLoopEnd = audio->loopEndSeconds();
        audio->setLoop(0.0, draft.source.audio.durationSeconds, false);
        m_timeline->setPlayheadSeconds(m_initialPosition);
        m_track->setPlayheadSeconds(m_initialPosition);
        connect(audio, &AudioService::positionChanged, this, [this](double seconds) {
            m_timeline->setPlayheadSeconds(seconds); m_track->setPlayheadSeconds(seconds);
        });
        connect(audio, &AudioService::playbackChanged, this, [this](bool playing) {
            m_play->setText(playing ? tr("暂停") : tr("试听"));
        });
        connect(m_play, &QPushButton::clicked, this, [this] {
            if (!m_audio || !m_audio->isReady()) return;
            m_changedPlayback = true;
            if (m_audio->isPlaying()) m_audio->pause(); else m_audio->play();
        });
        connect(m_timeline, &TimelineView::seekRequested, this, [this](double seconds) {
            if (!m_audio || !m_audio->isReady()) return;
            m_changedPlayback = true;
            m_audio->seek(seconds);
        });
    }
    connect(m_track, &TrackView::selectionChanged, m_timeline, &TimelineView::setSelectedIds);
    connect(m_timeline, &TimelineView::selectionChanged, m_track, &TrackView::setSelectedIds);
    m_status->setWordWrap(true);
    m_status->setObjectName(QStringLiteral("generationPreviewStatus"));
    m_status->hide();
    layout->addWidget(m_status);
    auto buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Cancel)->setText(tr("取消"));
    buttons->button(QDialogButtonBox::Cancel)->setObjectName(QStringLiteral("generationPreviewCancel"));
    auto apply = buttons->addButton(tr("应用候选谱"), QDialogButtonBox::AcceptRole);
    auto regenerate = buttons->addButton(tr("重新生成"), QDialogButtonBox::ActionRole);
    regenerate->setObjectName(QStringLiteral("generationPreviewRegenerate"));
    connect(regenerate, &QPushButton::clicked, this, &GenerationPreviewDialog::regenerateRequested);
    apply->setObjectName(QStringLiteral("generationPreviewApply"));
    apply->setProperty("role", "primary");
    apply->setEnabled(!draft.objects.isEmpty());
    connect(apply, &QPushButton::clicked, this, &GenerationPreviewDialog::applyRequested);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}

GenerationPreviewDialog::~GenerationPreviewDialog() {
    if (m_audio && m_audio->pcmSnapshot().revision == m_draft.source.audioRevision) {
        m_audio->setLoop(m_initialLoopStart, m_initialLoopEnd, m_initialLoopEnabled);
        if (m_changedPlayback) {
            m_audio->pause();
            m_audio->seek(m_initialPosition);
            if (m_initialPlaying) m_audio->play();
        }
    }
}

void GenerationPreviewDialog::showApplicationError(const QString &message) {
    m_status->setText(message);
    m_status->setProperty("role", "error");
    m_status->show();
}
}
