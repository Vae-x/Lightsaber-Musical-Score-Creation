#pragma once

#include <QWidget>

class QLabel;
class QProgressBar;
class QResizeEvent;

namespace lmsc {

// Task state stays outside parameter/result scroll areas. Percent text has its
// own label so the progress fill never determines the text's visible height.
class TaskProgressView : public QWidget {
    Q_OBJECT
public:
    explicit TaskProgressView(QWidget *parent = nullptr);
    void setProgress(int percent, const QString &stage);
    void setStage(const QString &stage);
    void setProgressVisible(bool visible);
    void clear();
    QLabel *stageLabel() const { return m_stage; }
    QLabel *percentageLabel() const { return m_percentage; }
    QProgressBar *progressBar() const { return m_bar; }
    bool hasHeightForWidth() const override;
    int heightForWidth(int width) const override;
protected:
    void resizeEvent(QResizeEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;
private:
    void updateMinimumHeight();
    QLabel *m_stage;
    QLabel *m_percentage;
    QProgressBar *m_bar;
};

} // namespace lmsc
