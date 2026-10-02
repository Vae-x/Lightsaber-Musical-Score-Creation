#include "EditorViews.h"
#include "ThemeManager.h"

#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QResizeEvent>
#include <QScrollBar>
#include <QToolTip>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>
#include <limits>

namespace {
QColor themedColor(const QColor &dark, const QColor &light) {
    return lmsc::ThemeManager::isDark() ? dark : light;
}
QColor background() { return themedColor(QColor(20, 21, 24), QColor(250, 251, 252)); }
QColor panel() { return themedColor(QColor(35, 37, 41), QColor(238, 240, 243)); }
QColor muted() { return themedColor(QColor(166, 171, 180), QColor(101, 110, 123)); }
QColor gridLine() { return themedColor(QColor(54, 57, 63), QColor(220, 224, 230)); }
QColor accent() { return themedColor(QColor(66, 211, 189), QColor(19, 132, 116)); }
QColor selectionColor() { return themedColor(QColor(255, 217, 94), QColor(151, 103, 0)); }
QColor foreground() { return themedColor(QColor(231, 232, 235), QColor(35, 39, 47)); }
QColor translucentAccent(int alpha) {
    QColor color = accent();
    color.setAlpha(alpha);
    return color;
}
constexpr int displayLimit = 2500;
constexpr double pi = 3.14159265358979323846;

QColor objectColor(const EditorObject &object) {
    if (object.type == EditorObject::Bomb)
        return themedColor(QColor(157, 173, 196), QColor(89, 105, 129));
    if (object.type == EditorObject::Wall)
        return QColor(222, 142, 72);
    return object.color == 0 ? QColor(245, 86, 111) : QColor(79, 156, 255);
}

void drawArrow(QPainter &painter, const QRectF &rect, int direction) {
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(250, 253, 255));
    const QPointF center = rect.center();
    const double radius = std::min(rect.width(), rect.height()) * 0.29;
    if (direction < 0 || direction >= 8) {
        painter.drawEllipse(center, radius * 0.28, radius * 0.28);
    } else {
        static const double angles[] = {-90, 90, 180, 0, -135, -45, 135, 45};
        painter.translate(center);
        painter.rotate(angles[direction]);
        QPolygonF arrow;
        arrow << QPointF(radius, 0) << QPointF(-radius * 0.35, -radius * 0.7)
              << QPointF(-radius * 0.05, 0) << QPointF(-radius * 0.35, radius * 0.7);
        painter.drawPolygon(arrow);
    }
    painter.restore();
}

void drawLock(QPainter &painter, const QRectF &rect) {
    painter.save();
    const double size = std::clamp(std::min(rect.width(), rect.height()) * 0.24, 5.0, 13.0);
    const QRectF body(rect.right() - size - 2, rect.top() + 5, size, size * 0.7);
    painter.setPen(QPen(QColor(255, 227, 138), 1.5));
    painter.setBrush(QColor(38, 37, 36));
    painter.drawRoundedRect(body, 1, 1);
    painter.setBrush(Qt::NoBrush);
    painter.drawArc(QRectF(body.left() + size * 0.2, body.top() - size * 0.45,
                          size * 0.6, size * 0.7), 0, 180 * 16);
    painter.restore();
}

void drawObject(QPainter &painter, const QRectF &rect, const EditorObject &object,
                bool selected, double opacity = 1.0) {
    painter.save();
    painter.setOpacity(opacity);
    const QColor color = objectColor(object);
    painter.setPen(QPen(selected ? selectionColor() : color.lighter(125), selected ? 2.5 : 1.0));
    if (object.type == EditorObject::Wall) {
        QColor fill = color;
        fill.setAlpha(42);
        painter.setBrush(fill);
        painter.drawRoundedRect(rect, 2, 2);
        painter.setPen(QPen(QColor(color.red(), color.green(), color.blue(), 80), 1));
        painter.drawLine(rect.topLeft(), rect.bottomRight());
    } else if (object.type == EditorObject::Bomb) {
        painter.setBrush(themedColor(QColor(53, 62, 80), QColor(204, 213, 226)));
        const double radius = std::min(rect.width(), rect.height()) * 0.38;
        painter.drawEllipse(rect.center(), radius, radius);
        for (int i = 0; i < 8; ++i) {
            const double angle = pi * i / 4;
            const QPointF delta(std::cos(angle), std::sin(angle));
            painter.drawLine(rect.center() + delta * radius * 0.7,
                             rect.center() + delta * radius * 1.22);
        }
    } else {
        painter.setBrush(color.darker(112));
        painter.drawRoundedRect(rect, 3, 3);
        drawArrow(painter, rect, object.direction);
    }
    if (object.locked)
        drawLock(painter, rect);
    painter.restore();
}

QString objectTip(const EditorObject &object) {
    QString text = QStringLiteral("第 %1 拍 · 列 %2 · 层 %3")
                       .arg(object.beat, 0, 'f', 3).arg(object.x + 1).arg(object.y + 1);
    if (object.locked)
        text += QStringLiteral("\n受保护：%1").arg(object.protectedReason.isEmpty()
                                                     ? QStringLiteral("关联高级谱面内容")
                                                     : object.protectedReason);
    return text;
}

QString timeLabel(double seconds) {
    const int value = std::max(0, static_cast<int>(seconds));
    return QStringLiteral("%1:%2").arg(value / 60).arg(value % 60, 2, 10, QLatin1Char('0'));
}
}

// Interval maxima make long walls visible even when their start precedes the
// viewport. Dense maps are queried without rescanning all objects on every frame.
class EditorObjectIndex {
public:
    QVector<EditorObject> objects;

    void set(const QVector<EditorObject> &source) {
        objects = source;
        std::stable_sort(objects.begin(), objects.end(), [](const EditorObject &a, const EditorObject &b) {
            return a.beat < b.beat;
        });
        m_maxEnd.fill(-std::numeric_limits<double>::infinity(), objects.size() * 4 + 4);
        if (!objects.isEmpty())
            build(1, 0, objects.size());
    }

    QVector<int> visible(double low, double high, bool *limited = nullptr,
                         int limit = displayLimit) const {
        QVector<int> result;
        result.reserve(std::min(objects.size(), displayLimit));
        bool truncated = false;
        if (!objects.isEmpty()) {
            const auto end = std::upper_bound(objects.begin(), objects.end(), high,
                                              [](double beat, const EditorObject &object) {
                                                  return beat < object.beat;
                                              });
            query(1, 0, objects.size(), static_cast<int>(end - objects.begin()), low,
                  result, truncated, limit);
        }
        if (limited)
            *limited = truncated;
        return result;
    }

private:
    double endOf(int index) const {
        const EditorObject &object = objects[index];
        return object.beat + (object.type == EditorObject::Wall ? std::max(0.0, object.duration) : 0.0);
    }
    double build(int node, int begin, int end) {
        if (end - begin == 1)
            return m_maxEnd[node] = endOf(begin);
        const int middle = (begin + end) / 2;
        const double left = build(node * 2, begin, middle);
        const double right = build(node * 2 + 1, middle, end);
        return m_maxEnd[node] = std::max(left, right);
    }
    void query(int node, int begin, int end, int stop, double low,
               QVector<int> &result, bool &truncated, int limit) const {
        if (begin >= stop || m_maxEnd[node] < low)
            return;
        if (limit > 0 && result.size() >= limit) {
            truncated = true;
            return;
        }
        if (end - begin == 1) {
            result.append(begin);
            return;
        }
        const int middle = (begin + end) / 2;
        query(node * 2, begin, middle, stop, low, result, truncated, limit);
        query(node * 2 + 1, middle, end, stop, low, result, truncated, limit);
    }
    QVector<double> m_maxEnd;
};

TimelineView::TimelineView(QWidget *parent)
    : QWidget(parent), m_index(new EditorObjectIndex), m_scroll(new QScrollBar(Qt::Horizontal, this)) {
    setMinimumHeight(210);
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setObjectName(QStringLiteral("timelineView"));
    connect(m_scroll, &QScrollBar::valueChanged, this, [this] { update(); });
    updateScrollRange();
}
TimelineView::~TimelineView() = default;
QSize TimelineView::sizeHint() const { return QSize(900, 250); }

void TimelineView::setObjects(const QVector<EditorObject> &objects) {
    m_index->set(objects);
    for (const EditorObject &object : objects)
        m_duration = std::max(m_duration, beatToSeconds(object.beat +
                                  (object.type == EditorObject::Wall ? object.duration : 0)) + 3.0);
    updateScrollRange();
    update();
}
void TimelineView::setWaveform(const QVector<float> &peaks, double durationSeconds) {
    m_peaks = peaks;
    m_waveDuration = std::max(0.0, durationSeconds);
    setDuration(durationSeconds);
}
void TimelineView::setDuration(double durationSeconds) {
    m_duration = std::max(1.0, durationSeconds);
    for (const EditorObject &object : m_index->objects)
        m_duration = std::max(m_duration, beatToSeconds(object.beat +
                                 (object.type == EditorObject::Wall ? object.duration : 0)) + 2.0);
    updateScrollRange();
    update();
}
void TimelineView::setTempo(double bpm, double offsetSeconds) {
    m_bpm = std::max(1.0, bpm);
    m_offset = offsetSeconds;
    m_beatToSeconds = {};
    m_secondsToBeat = {};
    updateScrollRange();
    update();
}
void TimelineView::setTimeMapping(std::function<double(double)> toSeconds,
                                std::function<double(double)> toBeat) {
    m_beatToSeconds = std::move(toSeconds);
    m_secondsToBeat = std::move(toBeat);
    update();
}
void TimelineView::setPlayheadSeconds(double seconds) {
    m_playhead = std::max(0.0, seconds);
    const QRectF rect = contentRect();
    if (m_follow && m_drag == Idle && rect.width() > 0 &&
        (xAtTime(m_playhead) > rect.right() - 24 || xAtTime(m_playhead) < rect.left()))
        m_scroll->setValue(static_cast<int>(std::max(0.0, m_playhead - rect.width() / m_pixelsPerSecond * 0.25) * 1000));
    update();
}
void TimelineView::setSelectedIds(const QSet<QString> &ids) { m_selected = ids; update(); }
void TimelineView::setSnapDivision(int division) { m_snapDivision = std::max(0, division); update(); }
void TimelineView::setLoop(double start, double end) {
    m_loopStart = start;
    m_loopEnd = end > start ? end : -1.0;
    update();
}
void TimelineView::setFollowPlayhead(bool enabled) { m_follow = enabled; }
QRectF TimelineView::contentRect() const { return QRectF(52, 0, std::max(0, width() - 52), std::max(0, height() - 20)); }
QRectF TimelineView::tracksRect() const {
    const QRectF content = contentRect();
    return QRectF(content.left(), 88, content.width(), std::max(1.0, content.height() - 88));
}
double TimelineView::timeAtX(double x) const {
    return m_scroll->value() / 1000.0 + (x - contentRect().left()) / m_pixelsPerSecond;
}
double TimelineView::xAtTime(double seconds) const {
    return contentRect().left() + (seconds - m_scroll->value() / 1000.0) * m_pixelsPerSecond;
}
double TimelineView::beatToSeconds(double beat) const {
    return m_beatToSeconds ? m_beatToSeconds(beat) : beat * 60.0 / m_bpm + m_offset;
}
double TimelineView::secondsToBeat(double seconds) const {
    return m_secondsToBeat ? m_secondsToBeat(seconds) : (seconds - m_offset) * m_bpm / 60.0;
}
int TimelineView::laneAtY(double y) const {
    return std::clamp(static_cast<int>((y - tracksRect().top()) / (tracksRect().height() / 4)), 0, 3);
}
QRectF TimelineView::objectRect(const EditorObject &object, bool preview) const {
    const bool moving = preview && m_drag == MoveObjects && m_selected.contains(object.id) && !object.locked;
    const double beat = object.beat + (moving ? m_dragBeatDelta : 0.0);
    const int lane = std::clamp(object.x + (moving ? m_dragLaneDelta : 0), 0, 3);
    const double row = tracksRect().height() / 4.0;
    const double start = xAtTime(beatToSeconds(beat));
    if (object.type == EditorObject::Wall) {
        const double end = xAtTime(beatToSeconds(beat + object.duration));
        return QRectF(start, tracksRect().top() + lane * row + 3,
                      std::max(5.0, end - start), row * std::max(1, std::min(object.width, 4 - lane)) - 6);
    }
    const double size = std::min(22.0, row * 0.60);
    const double layerShift = (1 - std::clamp(object.y, 0, 2)) * row * 0.13;
    return QRectF(start - size / 2, tracksRect().top() + lane * row + (row - size) / 2 + layerShift, size, size);
}
int TimelineView::objectAt(const QPointF &point) const {
    const auto visible = m_index->visible(secondsToBeat(timeAtX(point.x() - 26)) - 0.01,
                                          secondsToBeat(timeAtX(point.x() + 26)) + 0.01);
    for (int pass = 0; pass < 2; ++pass)
        for (int i = visible.size() - 1; i >= 0; --i) {
            const int index = visible[i];
            const EditorObject &object = m_index->objects[index];
            if ((object.type == EditorObject::Wall) != (pass == 1))
                continue;
            if (objectRect(object).adjusted(-3, -3, 3, 3).contains(point))
                return index;
        }
    return -1;
}
void TimelineView::updateScrollRange() {
    const double visibleSeconds = contentRect().width() / m_pixelsPerSecond;
    m_scroll->setRange(0, static_cast<int>(std::max(0.0, m_duration - visibleSeconds) * 1000));
    m_scroll->setPageStep(std::max(1, static_cast<int>(visibleSeconds * 1000)));
    m_scroll->setSingleStep(250);
}
void TimelineView::changeSelection(const QSet<QString> &selection) {
    if (selection != m_selected) {
        m_selected = selection;
        emit selectionChanged(m_selected);
    }
    update();
}

void TimelineView::paintEvent(QPaintEvent *) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.fillRect(rect(), background());
    const QRectF tracks = tracksRect();
    const QRectF content = contentRect();
    painter.fillRect(QRectF(content.left(), 30, content.width(), 47), panel());
    painter.setPen(muted());
    painter.drawText(QRectF(3, 28, 45, 47), Qt::AlignCenter, QStringLiteral("波形"));
    for (int lane = 0; lane < 4; ++lane) {
        const double top = tracks.top() + lane * tracks.height() / 4;
        painter.fillRect(QRectF(content.left(), top, content.width(), tracks.height() / 4),
                         lane % 2 ? panel() : background());
        painter.setPen(muted());
        painter.drawText(QRectF(2, top, 46, tracks.height() / 4), Qt::AlignCenter,
                         QStringLiteral("列 %1").arg(lane + 1));
        painter.setPen(gridLine());
        painter.drawLine(QPointF(content.left(), top), QPointF(content.right(), top));
    }
    painter.save();
    painter.setClipRect(content);
    if (m_loopEnd > m_loopStart && m_loopStart >= 0) {
        QColor loop = accent();
        loop.setAlpha(25);
        const QRectF loopRect(xAtTime(m_loopStart), 0,
                              (m_loopEnd - m_loopStart) * m_pixelsPerSecond, content.height());
        painter.fillRect(loopRect, loop);
        painter.setPen(QPen(accent(), 1, Qt::DashLine));
        painter.drawLine(loopRect.topLeft(), loopRect.bottomLeft());
        painter.drawLine(loopRect.topRight(), loopRect.bottomRight());
    }
    const double beginSeconds = timeAtX(content.left());
    const double endSeconds = timeAtX(content.right());
    const double beginBeat = secondsToBeat(beginSeconds);
    const double endBeat = secondsToBeat(endSeconds);
    double beatStep = m_snapDivision > 0 ? 1.0 / m_snapDivision : 1.0;
    const double pixelsPerBeat = std::max(1.0, (beatToSeconds(beginBeat + 1) - beatToSeconds(beginBeat)) * m_pixelsPerSecond);
    while (beatStep * pixelsPerBeat < 12)
        beatStep *= 2;
    const double firstBeat = std::floor(beginBeat / beatStep) * beatStep;
    const int count = std::min(3000, static_cast<int>((endBeat - firstBeat) / beatStep) + 2);
    for (int i = 0; i < count; ++i) {
        const double beat = firstBeat + i * beatStep;
        if (beat < 0)
            continue;
        const double x = xAtTime(beatToSeconds(beat));
        const bool major = std::abs(beat / 4 - std::round(beat / 4)) < 0.001;
        const bool whole = std::abs(beat - std::round(beat)) < 0.001;
        painter.setPen(themedColor(QColor(major ? 90 : 51, major ? 112 : 63, major ? 133 : 80),
                                  major ? QColor(121, 145, 174) : QColor(207, 218, 232)));
        painter.drawLine(QPointF(x, whole ? 79 : tracks.top()), QPointF(x, content.bottom()));
        if (whole && pixelsPerBeat * std::max(1.0, beatStep) > 38) {
            painter.setPen(major ? foreground() : muted());
            painter.drawText(QRectF(x + 3, 76, 70, 12), Qt::AlignLeft | Qt::AlignVCenter,
                             QString::number(beat, 'f', 0));
        }
    }
    double secondsStep = 1;
    while (secondsStep * m_pixelsPerSecond < 85)
        secondsStep *= 2;
    for (double second = std::ceil(beginSeconds / secondsStep) * secondsStep; second <= endSeconds; second += secondsStep) {
        const double x = xAtTime(second);
        painter.setPen(muted());
        painter.drawText(QRectF(x + 3, 2, 74, 23), Qt::AlignLeft | Qt::AlignVCenter, timeLabel(second));
        painter.setPen(gridLine());
        painter.drawLine(QPointF(x, 25), QPointF(x, 30));
    }
    if (!m_peaks.isEmpty() && m_waveDuration > 0) {
        painter.setPen(QPen(accent(), 1));
        for (int x = static_cast<int>(content.left()); x <= content.right(); ++x) {
            const double seconds = timeAtX(x);
            if (seconds < 0 || seconds >= m_waveDuration)
                continue;
            const int from = std::clamp(static_cast<int>(seconds / m_waveDuration * m_peaks.size()), 0, m_peaks.size() - 1);
            const int to = std::clamp(static_cast<int>(timeAtX(x + 1) / m_waveDuration * m_peaks.size()), from, m_peaks.size() - 1);
            float peak = 0;
            for (int i = from; i <= to; ++i)
                peak = std::max(peak, std::abs(m_peaks[i]));
            const double amplitude = std::clamp(static_cast<double>(peak), 0.0, 1.0) * 21;
            painter.drawLine(QPointF(x, 53 - amplitude), QPointF(x, 53 + amplitude));
        }
    } else {
        painter.setPen(muted());
        painter.drawText(QRectF(content.left() + 14, 31, content.width() - 28, 43), Qt::AlignVCenter,
                         QStringLiteral("导入歌曲后显示波形 · 单击定位 · 滚轮缩放 · Shift 拖动设循环"));
    }
    bool limited = false;
    const auto visible = m_index->visible(beginBeat - 1, endBeat + 1, &limited);
    // Walls are behind notes, so ordinary objects remain selectable in dense maps.
    for (int pass = 0; pass < 2; ++pass)
        for (int index : visible) {
            const EditorObject &object = m_index->objects[index];
            if ((object.type == EditorObject::Wall) != (pass == 0))
                continue;
            drawObject(painter, objectRect(object, true), object, m_selected.contains(object.id));
        }
    if (m_drag == BoxSelection) {
        painter.setPen(QPen(accent(), 1));
        painter.setBrush(translucentAccent(28));
        painter.drawRect(QRectF(m_press, m_current).normalized());
    }
    if (m_drag == MakeLoop) {
        painter.fillRect(QRectF(QPointF(std::min(m_press.x(), m_current.x()), 0),
                                 QPointF(std::max(m_press.x(), m_current.x()), content.bottom())),
                         translucentAccent(40));
    }
    const double playheadX = xAtTime(m_playhead);
    const QColor playheadColor = themedColor(QColor(252, 240, 175), QColor(151, 103, 0));
    painter.setPen(QPen(playheadColor, 1.5));
    painter.drawLine(QPointF(playheadX, 0), QPointF(playheadX, content.bottom()));
    painter.setBrush(playheadColor);
    QPolygonF marker;
    marker << QPointF(playheadX - 5, 0) << QPointF(playheadX + 5, 0) << QPointF(playheadX, 8);
    painter.drawPolygon(marker);
    if (limited) {
        painter.setPen(selectionColor());
        painter.drawText(QRectF(content.left() + 10, content.bottom() - 20, content.width() - 20, 17),
                         Qt::AlignRight, QStringLiteral("密集区仅显示前 %1 个物件，请放大查看；完整数据保留").arg(displayLimit));
    }
    painter.restore();
}

void TimelineView::resizeEvent(QResizeEvent *) {
    m_scroll->setGeometry(52, std::max(0, height() - 18), std::max(0, width() - 52), 18);
    updateScrollRange();
}
void TimelineView::mousePressEvent(QMouseEvent *event) {
    setFocus();
    if (!contentRect().contains(event->localPos()))
        return;
    m_press = m_current = event->localPos();
    m_pressSelection = m_selected;
    m_additive = event->modifiers().testFlag(Qt::ControlModifier);
    if (event->button() == Qt::RightButton || event->button() == Qt::MiddleButton) {
        m_drag = Pan;
        m_pressScroll = m_scroll->value();
        setCursor(Qt::ClosedHandCursor);
    } else if (event->button() == Qt::LeftButton && event->modifiers().testFlag(Qt::ShiftModifier)) {
        m_drag = MakeLoop;
    } else if (event->button() == Qt::LeftButton) {
        const int index = tracksRect().contains(event->localPos()) ? objectAt(event->localPos()) : -1;
        if (index >= 0) {
            const EditorObject &object = m_index->objects[index];
            QSet<QString> selected = m_selected;
            if (m_additive) {
                if (selected.contains(object.id))
                    selected.remove(object.id);
                else
                    selected.insert(object.id);
            } else if (!selected.contains(object.id)) {
                selected = {object.id};
            }
            changeSelection(selected);
            m_drag = MoveObjects;
            m_dragBeatDelta = 0;
            m_dragLaneDelta = 0;
        } else {
            m_drag = BoxSelection;
        }
    }
    update();
}
void TimelineView::mouseMoveEvent(QMouseEvent *event) {
    m_current = event->localPos();
    if (m_drag == Pan) {
        m_scroll->setValue(m_pressScroll - static_cast<int>((m_current.x() - m_press.x()) / m_pixelsPerSecond * 1000));
    } else if (m_drag == MoveObjects) {
        if ((m_current - m_press).manhattanLength() > 4) {
            m_dragBeatDelta = secondsToBeat(timeAtX(m_current.x())) - secondsToBeat(timeAtX(m_press.x()));
            if (m_snapDivision > 0)
                m_dragBeatDelta = std::round(m_dragBeatDelta * m_snapDivision) / m_snapDivision;
            m_dragLaneDelta = laneAtY(m_current.y()) - laneAtY(m_press.y());
        }
        update();
    } else if (m_drag != Idle) {
        update();
    } else {
        const int index = objectAt(event->localPos());
        if (index >= 0)
            QToolTip::showText(event->globalPos(), objectTip(m_index->objects[index]), this);
        else
            QToolTip::hideText();
        setCursor(index >= 0 ? Qt::PointingHandCursor : Qt::ArrowCursor);
    }
}
void TimelineView::mouseReleaseEvent(QMouseEvent *event) {
    if (m_drag == Idle)
        return;
    m_current = event->localPos();
    const bool moved = (m_current - m_press).manhattanLength() > 4;
    if (m_drag == BoxSelection) {
        if (moved && tracksRect().intersects(QRectF(m_press, m_current).normalized())) {
            const QRectF box = QRectF(m_press, m_current).normalized();
            QSet<QString> selection = m_additive ? m_pressSelection : QSet<QString>();
            const auto visible = m_index->visible(secondsToBeat(timeAtX(box.left())) - 1,
                                                  secondsToBeat(timeAtX(box.right())) + 1, nullptr, 0);
            for (int index : visible)
                if (box.intersects(objectRect(m_index->objects[index])))
                    selection.insert(m_index->objects[index].id);
            changeSelection(selection);
        } else {
            if (!m_additive)
                changeSelection({});
            emit seekRequested(std::clamp(timeAtX(m_current.x()), 0.0, m_duration));
        }
    } else if (m_drag == MakeLoop && moved) {
        const double first = std::clamp(timeAtX(std::min(m_press.x(), m_current.x())), 0.0, m_duration);
        const double last = std::clamp(timeAtX(std::max(m_press.x(), m_current.x())), 0.0, m_duration);
        if (last - first > 0.03) {
            setLoop(first, last);
            emit loopChanged(first, last);
        }
    } else if (m_drag == MoveObjects && moved &&
               (std::abs(m_dragBeatDelta) > 1e-8 || m_dragLaneDelta != 0)) {
        // The document validates the complete transaction, including protected
        // objects. Filtering here would silently move only part of a selection.
        if (!m_selected.isEmpty())
            emit objectsMoveRequested(m_selected, m_dragBeatDelta, m_dragLaneDelta, 0);
    }
    m_drag = Idle;
    unsetCursor();
    update();
}
void TimelineView::mouseDoubleClickEvent(QMouseEvent *event) {
    if (event->button() == Qt::LeftButton && contentRect().contains(event->localPos())) {
        m_drag = Idle;
        emit seekRequested(std::clamp(timeAtX(event->localPos().x()), 0.0, m_duration));
    }
}
void TimelineView::wheelEvent(QWheelEvent *event) {
    if (event->modifiers().testFlag(Qt::ShiftModifier)) {
        m_scroll->setValue(m_scroll->value() - event->angleDelta().y() * 3);
    } else {
        const double anchorX = std::clamp(static_cast<double>(event->pos().x()), contentRect().left(), contentRect().right());
        const double anchor = timeAtX(anchorX);
        const double factor = std::pow(1.2, event->angleDelta().y() / 120.0);
        m_pixelsPerSecond = std::clamp(m_pixelsPerSecond * factor, 4.0, 1800.0);
        updateScrollRange();
        m_scroll->setValue(static_cast<int>(std::max(0.0, anchor - (anchorX - contentRect().left()) / m_pixelsPerSecond) * 1000));
    }
    event->accept();
    update();
}
void TimelineView::keyPressEvent(QKeyEvent *event) {
    if (event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace) {
        emit deleteRequested();
        event->accept();
    } else if (event->key() == Qt::Key_Escape) {
        m_drag = Idle;
        changeSelection({});
    } else {
        QWidget::keyPressEvent(event);
    }
}

GridEditor::GridEditor(QWidget *parent) : QWidget(parent), m_index(new EditorObjectIndex) {
    setMinimumSize(260, 240);
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setObjectName(QStringLiteral("gridEditor"));
    setToolTip(QStringLiteral("左键点击空格放置、点击物件选择 · Ctrl 追加选择 · 方向键选格，Enter 放置"));
}
GridEditor::~GridEditor() = default;
QSize GridEditor::sizeHint() const { return QSize(350, 300); }
void GridEditor::setPlacement(int type, int color, int direction) {
    m_type = type;
    m_color = color;
    m_direction = direction;
    update();
}
void GridEditor::setBeat(double beat) { m_beat = std::max(0.0, beat); update(); }
void GridEditor::setObjects(const QVector<EditorObject> &objects) { m_index->set(objects); update(); }
void GridEditor::setSelectedIds(const QSet<QString> &ids) { m_selected = ids; update(); }
QRectF GridEditor::gridRect() const {
    const double cell = std::max(10.0, std::min((width() - 38) / 4.0, (height() - 78) / 3.0));
    return QRectF((width() - cell * 4) / 2, 39 + (height() - 78 - cell * 3) / 2, cell * 4, cell * 3);
}
QPoint GridEditor::cellAt(const QPointF &point) const {
    const QRectF grid = gridRect();
    if (!grid.contains(point))
        return QPoint(-1, -1);
    return QPoint(std::clamp(static_cast<int>((point.x() - grid.left()) / (grid.width() / 4)), 0, 3),
                  2 - std::clamp(static_cast<int>((point.y() - grid.top()) / (grid.height() / 3)), 0, 2));
}
QRectF GridEditor::cellRect(int x, int y) const {
    const QRectF grid = gridRect();
    const double cell = grid.width() / 4;
    return QRectF(grid.left() + x * cell + 4, grid.top() + (2 - y) * cell + 4, cell - 8, cell - 8);
}
void GridEditor::paintEvent(QPaintEvent *) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.fillRect(rect(), background());
    painter.setPen(foreground());
    painter.drawText(QRectF(14, 8, width() - 28, 23), Qt::AlignLeft | Qt::AlignVCenter,
                     QStringLiteral("放置面板  ·  第 %1 拍").arg(m_beat, 0, 'f', 3));
    for (int x = 0; x < 4; ++x)
        for (int y = 0; y < 3; ++y) {
            const QRectF cell = cellRect(x, y);
            painter.setBrush(QPoint(x, y) == m_hover
                                 ? themedColor(QColor(43, 59, 76), QColor(214, 229, 242)) : panel());
            painter.setPen(gridLine());
            painter.drawRoundedRect(cell, 5, 5);
            painter.setPen(themedColor(QColor(97, 114, 138), QColor(111, 129, 151)));
            painter.drawText(cell.adjusted(5, 2, -5, -2), Qt::AlignLeft | Qt::AlignBottom,
                             QStringLiteral("%1,%2").arg(x + 1).arg(y + 1));
        }
    const auto visible = m_index->visible(m_beat - 0.035, m_beat + 0.035);
    bool hoverOccupied = false;
    for (int index : visible) {
        const EditorObject &object = m_index->objects[index];
        if (object.x < 0 || object.x > 3 || object.y < 0 || object.y > 2)
            continue;
        QRectF display = cellRect(object.x, object.y).adjusted(5, 5, -5, -5);
        if (object.type == EditorObject::Wall) {
            const QRectF end = cellRect(std::min(3, object.x + std::max(1, object.width) - 1),
                                        std::min(2, object.y + std::max(1, object.height) - 1));
            display = display.united(end.adjusted(5, 5, -5, -5));
        }
        if (m_hover.x() >= object.x && m_hover.y() >= object.y &&
            m_hover.x() < object.x + (object.type == EditorObject::Wall ? std::max(1, object.width) : 1) &&
            m_hover.y() < object.y + (object.type == EditorObject::Wall ? std::max(1, object.height) : 1))
            hoverOccupied = true;
        drawObject(painter, display, object, m_selected.contains(object.id));
    }
    if (m_hover.x() >= 0 && !hoverOccupied) {
        EditorObject ghost;
        ghost.type = m_type;
        ghost.color = m_color;
        ghost.direction = m_direction;
        drawObject(painter, cellRect(m_hover.x(), m_hover.y()).adjusted(6, 6, -6, -6), ghost, false, 0.55);
    } else if (hoverOccupied) {
        painter.setPen(QPen(accent(), 1.5, Qt::DashLine));
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(cellRect(m_hover.x(), m_hover.y()), 5, 5);
    }
    painter.setPen(muted());
    painter.drawText(QRectF(8, height() - 32, width() - 16, 24), Qt::AlignCenter,
                     hoverOccupied ? QStringLiteral("此格已有物件 · 点击选择")
                                   : QStringLiteral("左键放置 · 右键选择 · 顶部为第 3 层"));
}
void GridEditor::mousePressEvent(QMouseEvent *event) {
    setFocus();
    m_hover = cellAt(event->localPos());
    if (m_hover.x() < 0)
        return;
    const auto visible = m_index->visible(m_beat - 0.035, m_beat + 0.035);
    int hit = -1;
    for (int pass = 0; pass < 2 && hit < 0; ++pass)
        for (int index : visible) {
            const EditorObject &object = m_index->objects[index];
            if ((object.type == EditorObject::Wall) != (pass == 1))
                continue;
            if (m_hover.x() >= object.x && m_hover.y() >= object.y &&
                m_hover.x() < object.x + (object.type == EditorObject::Wall ? std::max(1, object.width) : 1) &&
                m_hover.y() < object.y + (object.type == EditorObject::Wall ? std::max(1, object.height) : 1)) {
                hit = index;
                break;
            }
        }
    if (event->button() == Qt::LeftButton && hit < 0) {
        emit addRequested(m_beat, m_hover.x(), m_hover.y());
    } else if (event->button() == Qt::RightButton || (event->button() == Qt::LeftButton && hit >= 0)) {
        QSet<QString> selected = event->modifiers().testFlag(Qt::ControlModifier) ? m_selected : QSet<QString>();
        if (hit >= 0) {
            const EditorObject &object = m_index->objects[hit];
            if (selected.contains(object.id))
                selected.remove(object.id);
            else
                selected.insert(object.id);
        }
        m_selected = selected;
        emit selectionChanged(selected);
    }
    update();
}
void GridEditor::mouseMoveEvent(QMouseEvent *event) { m_hover = cellAt(event->localPos()); update(); }
void GridEditor::leaveEvent(QEvent *) { m_hover = QPoint(-1, -1); update(); }
void GridEditor::keyPressEvent(QKeyEvent *event) {
    if (m_hover.x() < 0)
        m_hover = QPoint(1, 1);
    switch (event->key()) {
    case Qt::Key_Left: m_hover.setX(std::max(0, m_hover.x() - 1)); break;
    case Qt::Key_Right: m_hover.setX(std::min(3, m_hover.x() + 1)); break;
    case Qt::Key_Up: m_hover.setY(std::min(2, m_hover.y() + 1)); break;
    case Qt::Key_Down: m_hover.setY(std::max(0, m_hover.y() - 1)); break;
    case Qt::Key_Return:
    case Qt::Key_Enter: emit addRequested(m_beat, m_hover.x(), m_hover.y()); break;
    default: QWidget::keyPressEvent(event); return;
    }
    event->accept();
    update();
}

TrackView::TrackView(QWidget *parent) : QOpenGLWidget(parent), m_index(new EditorObjectIndex) {
    setMinimumSize(350, 250);
    setMouseTracking(true);
    setObjectName(QStringLiteral("trackView"));
    setToolTip(QStringLiteral("点击物件选择 · Ctrl 点击多选 · 滚轮调整预览距离"));
}
TrackView::~TrackView() = default;
QSize TrackView::sizeHint() const { return QSize(650, 420); }
void TrackView::setObjects(const QVector<EditorObject> &objects) {
    m_hits.clear();
    m_index->set(objects);
    update();
}
void TrackView::setTempo(double bpm, double offsetSeconds) {
    m_bpm = std::max(1.0, bpm);
    m_offset = offsetSeconds;
    m_beatToSeconds = {};
    m_secondsToBeat = {};
    update();
}
void TrackView::setTimeMapping(std::function<double(double)> toSeconds,
                             std::function<double(double)> toBeat) {
    m_beatToSeconds = std::move(toSeconds);
    m_secondsToBeat = std::move(toBeat);
    update();
}
void TrackView::setPlayheadSeconds(double seconds) { m_playhead = std::max(0.0, seconds); update(); }
void TrackView::setSelectedIds(const QSet<QString> &ids) { m_selected = ids; update(); }
double TrackView::beatToSeconds(double beat) const {
    return m_beatToSeconds ? m_beatToSeconds(beat) : beat * 60.0 / m_bpm + m_offset;
}
double TrackView::secondsToBeat(double seconds) const {
    return m_secondsToBeat ? m_secondsToBeat(seconds) : (seconds - m_offset) * m_bpm / 60.0;
}
QPointF TrackView::project(double x, double y, double seconds) const {
    const double depth = 1.0 + std::max(-0.45, seconds) * 0.80;
    const double unit = std::min(width() / 5.3, height() / 4.0) * m_cameraScale / depth;
    const double horizon = height() * 0.22;
    const double floor = horizon + height() * 0.70 / depth;
    return QPointF(width() * 0.5 + x * unit, floor - y * unit);
}
QRectF TrackView::frontRect(const EditorObject &object, double seconds) const {
    const double inset = object.type == EditorObject::Wall ? 0.015 : 0.14;
    const double left = object.x - 2 + inset;
    const double bottom = object.y + inset;
    const double right = object.x - 2 + (object.type == EditorObject::Wall ? std::max(1, object.width) : 1) - inset;
    const double top = object.y + (object.type == EditorObject::Wall ? std::max(1, object.height) : 1) - inset;
    return QRectF(project(left, top, seconds), project(right, bottom, seconds)).normalized();
}
void TrackView::paintGL() {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    QLinearGradient gradient(0, 0, 0, height());
    gradient.setColorAt(0, themedColor(QColor(20, 21, 24), QColor(250, 251, 252)));
    gradient.setColorAt(1, themedColor(QColor(39, 42, 48), QColor(233, 237, 242)));
    painter.fillRect(rect(), gradient);
    painter.setPen(QPen(themedColor(QColor(63, 91, 122), QColor(160, 184, 209)), 1));
    for (int x = -2; x <= 2; ++x)
        painter.drawLine(project(x, 0, -0.1), project(x, 0, m_lookAhead));
    const double currentBeat = secondsToBeat(m_playhead);
    const double endBeat = secondsToBeat(m_playhead + m_lookAhead);
    const double firstBeat = std::floor(currentBeat);
    for (double beat = firstBeat; beat < endBeat && beat < firstBeat + 256; ++beat) {
        const double seconds = beatToSeconds(beat) - m_playhead;
        if (seconds < -0.2)
            continue;
        const bool measure = std::fmod(beat, 4.0) == 0;
        painter.setPen(QPen(measure
                               ? themedColor(QColor(77, 116, 146), QColor(111, 146, 179))
                               : themedColor(QColor(48, 69, 91), QColor(181, 202, 223)),
                            measure ? 1.4 : 1));
        painter.drawLine(project(-2, 0, seconds), project(2, 0, seconds));
    }
    bool limited = false;
    auto visible = m_index->visible(secondsToBeat(m_playhead - 0.35), endBeat, &limited);
    std::stable_sort(visible.begin(), visible.end(), [this](int a, int b) {
        return m_index->objects[a].beat > m_index->objects[b].beat;
    });
    m_hits.clear();
    for (int index : visible) {
        const EditorObject &object = m_index->objects[index];
        const double seconds = beatToSeconds(object.beat) - m_playhead;
        if (object.type == EditorObject::Wall) {
            const double end = beatToSeconds(object.beat + std::max(0.0, object.duration)) - m_playhead;
            if (end < -0.30)
                continue;
            const double nearTime = std::max(-0.25, seconds);
            const double farTime = std::min(m_lookAhead, end);
            const QRectF nearFace = frontRect(object, nearTime);
            const QRectF farFace = frontRect(object, farTime);
            QColor wall = objectColor(object);
            QColor fill = wall;
            fill.setAlpha(22);
            painter.setPen(QPen(m_selected.contains(object.id) ? selectionColor() : wall, 1));
            painter.setBrush(fill);
            QPolygonF top;
            top << nearFace.topLeft() << farFace.topLeft() << farFace.topRight() << nearFace.topRight();
            painter.drawPolygon(top);
            QPolygonF left;
            left << nearFace.topLeft() << farFace.topLeft() << farFace.bottomLeft() << nearFace.bottomLeft();
            painter.drawPolygon(left);
            QPolygonF right;
            right << nearFace.topRight() << farFace.topRight() << farFace.bottomRight() << nearFace.bottomRight();
            painter.drawPolygon(right);
            drawObject(painter, nearFace, object, m_selected.contains(object.id));
            m_hits.append(qMakePair(nearFace, index));
        } else {
            if (seconds < -0.30 || seconds > m_lookAhead)
                continue;
            const QRectF face = frontRect(object, seconds);
            if (face.width() < 2)
                continue;
            const QRectF rear = frontRect(object, seconds + 0.14);
            const QColor color = objectColor(object);
            painter.setPen(QPen(color.darker(115), 1));
            painter.setBrush(color.darker(175));
            QPolygonF top;
            top << face.topLeft() << rear.topLeft() << rear.topRight() << face.topRight();
            painter.drawPolygon(top);
            QPolygonF side;
            side << face.topRight() << rear.topRight() << rear.bottomRight() << face.bottomRight();
            painter.drawPolygon(side);
            drawObject(painter, face, object, m_selected.contains(object.id));
            m_hits.append(qMakePair(face, index));
        }
    }
    painter.setPen(QPen(translucentAccent(105), 1, Qt::DashLine));
    for (int x = -2; x <= 2; ++x)
        painter.drawLine(project(x, 0, 0), project(x, 3, 0));
    for (int y = 0; y <= 3; ++y)
        painter.drawLine(project(-2, y, 0), project(2, y, 0));
    painter.setPen(foreground());
    painter.drawText(QRectF(14, 10, width() - 28, 24), Qt::AlignLeft | Qt::AlignVCenter,
                     QStringLiteral("轨道预览  ·  %1  ·  第 %2 拍")
                         .arg(timeLabel(m_playhead)).arg(currentBeat, 0, 'f', 2));
    painter.setPen(muted());
    painter.drawText(QRectF(14, height() - 29, width() - 28, 20), Qt::AlignRight | Qt::AlignVCenter,
                     limited ? QStringLiteral("密集区显示上限 %1 · 完整数据保留").arg(displayLimit)
                             : QStringLiteral("点击选择 · Ctrl 多选 · 滚轮调整预览距离"));
}
void TrackView::mousePressEvent(QMouseEvent *event) {
    if (event->button() != Qt::LeftButton)
        return;
    QSet<QString> selection = event->modifiers().testFlag(Qt::ControlModifier) ? m_selected : QSet<QString>();
    for (int i = m_hits.size() - 1; i >= 0; --i) {
        if (m_hits[i].first.contains(event->localPos())) {
            const QString id = m_index->objects[m_hits[i].second].id;
            if (selection.contains(id))
                selection.remove(id);
            else
                selection.insert(id);
            break;
        }
    }
    m_selected = selection;
    emit selectionChanged(selection);
    update();
}
void TrackView::mouseMoveEvent(QMouseEvent *event) {
    for (int i = m_hits.size() - 1; i >= 0; --i) {
        if (m_hits[i].first.contains(event->localPos())) {
            setCursor(Qt::PointingHandCursor);
            QToolTip::showText(event->globalPos(), objectTip(m_index->objects[m_hits[i].second]), this);
            return;
        }
    }
    unsetCursor();
    QToolTip::hideText();
}
void TrackView::wheelEvent(QWheelEvent *event) {
    m_lookAhead = std::clamp(m_lookAhead * std::pow(1.16, -event->angleDelta().y() / 120.0), 2.0, 20.0);
    event->accept();
    update();
}
