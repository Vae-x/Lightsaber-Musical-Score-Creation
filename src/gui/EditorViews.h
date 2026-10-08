#pragma once

#include <QOpenGLWidget>
#include <QSet>
#include <QVector>
#include <QWidget>
#include <functional>
#include <memory>

class QScrollBar;
class EditorObjectIndex;

// A small display model keeps the drawing widgets independent of file formats.
struct EditorObject {
    enum Type { Note = 0, Bomb = 1, Wall = 2 };
    QString id;
    double beat = 0.0;
    double duration = 1.0;
    int x = 0;
    int y = 0;
    int width = 1;
    int height = 3;
    int type = Note;
    int color = 0;
    int direction = 8;
    bool locked = false;
    QString protectedReason;
};

class TimelineView : public QWidget {
    Q_OBJECT
public:
    explicit TimelineView(QWidget *parent = nullptr);
    ~TimelineView() override;
    QSize sizeHint() const override;
    void setObjects(const QVector<EditorObject> &objects);
    void setWaveform(const QVector<float> &peaks, double durationSeconds);
    void setDuration(double durationSeconds);
    void setTempo(double bpm, double offsetSeconds = 0.0);
    void setTimeMapping(std::function<double(double)> beatToSeconds,
                        std::function<double(double)> secondsToBeat);
    void setPlayheadSeconds(double seconds);
    void setSelectedIds(const QSet<QString> &ids);
    void setSnapDivision(int subdivisionsPerBeat);
    void setLoop(double startSeconds, double endSeconds);
    void setFollowPlayhead(bool enabled);
    void setReadOnly(bool enabled);
    // Explicit tools give touch users the same operations as modifier keys.
    void setInteractionMode(int mode) { m_interactionMode = mode; } // 0 edit, 1 pan, 2 loop
    void setAdditiveSelection(bool enabled) { m_touchAdditive = enabled; }
    void zoomBy(double factor);

signals:
    void seekRequested(double seconds);
    void selectionChanged(QSet<QString> ids);
    void loopChanged(double startSeconds, double endSeconds);
    void objectsMoveRequested(QSet<QString> ids, double beatDelta,
                              int laneDelta, int layerDelta);
    void deleteRequested();

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

private:
    enum DragMode { Idle, BoxSelection, MoveObjects, MakeLoop, Pan };
    QRectF contentRect() const;
    QRectF tracksRect() const;
    double timeAtX(double x) const;
    double xAtTime(double seconds) const;
    double beatToSeconds(double beat) const;
    double secondsToBeat(double seconds) const;
    int laneAtY(double y) const;
    QRectF objectRect(const EditorObject &object, bool preview = false) const;
    int objectAt(const QPointF &point) const;
    void updateScrollRange();
    void changeSelection(const QSet<QString> &selection);

    std::unique_ptr<EditorObjectIndex> m_index;
    QVector<float> m_peaks;
    QSet<QString> m_selected;
    QScrollBar *m_scroll = nullptr;
    std::function<double(double)> m_beatToSeconds;
    std::function<double(double)> m_secondsToBeat;
    double m_duration = 180.0;
    double m_waveDuration = 0.0;
    double m_bpm = 120.0;
    double m_offset = 0.0;
    double m_playhead = 0.0;
    double m_pixelsPerSecond = 90.0;
    double m_loopStart = -1.0;
    double m_loopEnd = -1.0;
    double m_dragBeatDelta = 0.0;
    int m_dragLaneDelta = 0;
    int m_snapDivision = 4;
    bool m_follow = true;
    bool m_additive = false;
    bool m_readOnly = false;
    int m_interactionMode = 0;
    bool m_touchAdditive = false;
    DragMode m_drag = Idle;
    QPointF m_press;
    QPointF m_current;
    int m_pressScroll = 0;
    QSet<QString> m_pressSelection;
    QString m_touchToggleId;
};

class GridEditor : public QWidget {
    Q_OBJECT
public:
    explicit GridEditor(QWidget *parent = nullptr);
    ~GridEditor() override;
    QSize sizeHint() const override;
    void setPlacement(int type, int color, int direction);
    void setBeat(double beat);
    void setObjects(const QVector<EditorObject> &objects);
    void setSelectedIds(const QSet<QString> &ids);
    void setSelectionOnly(bool enabled) { m_selectionOnly = enabled; }
    void setAdditiveSelection(bool enabled) { m_touchAdditive = enabled; }

signals:
    void addRequested(double beat, int x, int y);
    void selectionChanged(QSet<QString> ids);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

private:
    QRectF gridRect() const;
    QPoint cellAt(const QPointF &point) const;
    QRectF cellRect(int x, int y) const;
    std::unique_ptr<EditorObjectIndex> m_index;
    QSet<QString> m_selected;
    QPoint m_hover = QPoint(-1, -1);
    double m_beat = 0.0;
    int m_type = EditorObject::Note;
    int m_color = 0;
    int m_direction = 8;
    bool m_selectionOnly = false;
    bool m_touchAdditive = false;
};

class TrackView : public QOpenGLWidget {
    Q_OBJECT
public:
    explicit TrackView(QWidget *parent = nullptr);
    ~TrackView() override;
    QSize sizeHint() const override;
    void setObjects(const QVector<EditorObject> &objects);
    void setTempo(double bpm, double offsetSeconds = 0.0);
    void setTimeMapping(std::function<double(double)> beatToSeconds,
                        std::function<double(double)> secondsToBeat);
    void setPlayheadSeconds(double seconds);
    void setSelectedIds(const QSet<QString> &ids);
    void setAdditiveSelection(bool enabled) { m_touchAdditive = enabled; }

signals:
    void selectionChanged(QSet<QString> ids);

protected:
    void paintGL() override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;

private:
    QPointF project(double x, double y, double distanceSeconds) const;
    QRectF frontRect(const EditorObject &object, double distanceSeconds) const;
    double beatToSeconds(double beat) const;
    double secondsToBeat(double seconds) const;
    std::unique_ptr<EditorObjectIndex> m_index;
    QSet<QString> m_selected;
    QVector<QPair<QRectF, int>> m_hits;
    std::function<double(double)> m_beatToSeconds;
    std::function<double(double)> m_secondsToBeat;
    double m_playhead = 0.0;
    double m_bpm = 120.0;
    double m_offset = 0.0;
    double m_lookAhead = 6.0;
    double m_cameraScale = 1.0;
    bool m_touchAdditive = false;
};
