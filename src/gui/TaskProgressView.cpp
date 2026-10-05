#include "TaskProgressView.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QProgressBar>
#include <QResizeEvent>
#include <QStyle>
#include <QTimer>
#include <QVBoxLayout>

namespace lmsc {

TaskProgressView::TaskProgressView(QWidget *parent) : QWidget(parent) {
    setObjectName(QStringLiteral("taskProgressView"));
    auto layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
    auto line = new QHBoxLayout;
    line->setSpacing(8);
    m_stage = new QLabel(this);
    m_stage->setObjectName(QStringLiteral("taskProgressStage"));
    m_stage->setTextFormat(Qt::PlainText);
    m_stage->setWordWrap(true);
    m_stage->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    m_stage->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    m_percentage = new QLabel(this);
    m_percentage->setObjectName(QStringLiteral("taskProgressPercentage"));
    m_percentage->setTextFormat(Qt::PlainText);
    m_percentage->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_percentage->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Preferred);
    line->addWidget(m_stage, 1);
    line->addWidget(m_percentage);
    layout->addLayout(line);
    m_bar = new QProgressBar(this);
    m_bar->setObjectName(QStringLiteral("taskProgressBar"));
    m_bar->setTextVisible(false);
    m_bar->setRange(0, 100);
    layout->addWidget(m_bar);
    for (auto child : {static_cast<QWidget *>(m_stage), static_cast<QWidget *>(m_percentage),
                       static_cast<QWidget *>(m_bar)}) child->installEventFilter(this);
    connect(m_bar, &QProgressBar::valueChanged, this, [this](int value) {
        if (m_bar->maximum() > m_bar->minimum()) m_percentage->setText(tr("%1%").arg(value));
    });
    clear();
}

void TaskProgressView::setProgress(int percent, const QString &stage) {
    setStage(stage);
    m_bar->setRange(0, percent < 0 ? 0 : 100);
    if (percent < 0) m_percentage->setText(tr("处理中"));
    else {
        m_bar->setValue(qBound(0, percent, 100));
        m_percentage->setText(tr("%1%").arg(qBound(0, percent, 100)));
    }
    setProgressVisible(true);
}

void TaskProgressView::setStage(const QString &stage) {
    m_stage->setText(stage);
    m_stage->setVisible(!stage.isEmpty());
    m_stage->style()->unpolish(m_stage);
    m_stage->style()->polish(m_stage);
    m_stage->update();
    updateMinimumHeight();
}

void TaskProgressView::setProgressVisible(bool visible) {
    m_bar->setVisible(visible);
    m_percentage->setVisible(visible);
    updateMinimumHeight();
}

bool TaskProgressView::hasHeightForWidth() const { return true; }

int TaskProgressView::heightForWidth(int width) const {
    const auto margins = layout()->contentsMargins();
    const bool stageVisible = !m_stage->isHidden();
    const bool percentVisible = !m_percentage->isHidden();
    const int percentWidth = percentVisible ? m_percentage->sizeHint().width() : 0;
    const int textWidth = qMax(1, width - margins.left() - margins.right() - percentWidth
                              - (stageVisible && percentVisible ? 8 : 0));
    const int stageHeight = stageVisible
        ? qMax(m_stage->fontMetrics().height(), m_stage->heightForWidth(textWidth)) : 0;
    const int lineHeight = qMax(stageHeight, percentVisible ? m_percentage->sizeHint().height() : 0);
    const int barHeight = m_bar->isHidden() ? 0 : m_bar->sizeHint().height();
    return margins.top() + margins.bottom() + lineHeight + barHeight
        + (lineHeight && barHeight ? layout()->spacing() : 0);
}

void TaskProgressView::updateMinimumHeight() {
    // QLabel's minimumSizeHint only protects one line. Reserve its wrapped
    // height at the actual column width before details consume the remaining
    // space; this also adapts to theme fonts and desktop scaling.
    const int required = heightForWidth(width());
    if (minimumHeight() != required) setMinimumHeight(required);
    updateGeometry();
}

void TaskProgressView::resizeEvent(QResizeEvent *event) {
    QWidget::resizeEvent(event);
    updateMinimumHeight();
}

bool TaskProgressView::eventFilter(QObject *watched, QEvent *event) {
    if (event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange
            || event->type() == QEvent::ApplicationFontChange)
        QTimer::singleShot(0, this, [this] { updateMinimumHeight(); });
    return QWidget::eventFilter(watched, event);
}

void TaskProgressView::clear() {
    setStage({});
    m_percentage->clear();
    m_bar->setRange(0, 100);
    m_bar->setValue(0);
    setProgressVisible(false);
}

} // namespace lmsc
