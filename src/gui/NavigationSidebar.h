#pragma once

#include <QColor>
#include <QRectF>
#include <QWidget>

class QLabel;
class QListWidget;
class QPainter;
class QToolButton;

namespace lmsc {

enum class NavigationIcon { Appearance, Model, Account, Network, About, Editor, Recognition };

// Vector icons share the caller's palette and remain sharp at any display scale.
void drawNavigationIcon(QPainter &painter, NavigationIcon icon,
                        const QRectF &rect, const QColor &color);

class NavigationSidebar : public QWidget {
    Q_OBJECT
public:
    explicit NavigationSidebar(QWidget *parent = nullptr);
    QListWidget *listWidget() const { return m_list; }
    void addItem(const QString &text, NavigationIcon icon);
    void setCollapsed(bool collapsed);
    void setHeadingText(const QString &text);
    bool isCollapsed() const { return m_collapsed; }

signals:
    void collapsedChanged(bool collapsed);

private:
    QListWidget *m_list;
    QToolButton *m_toggle;
    QLabel *m_heading;
    bool m_collapsed = false;
};

} // namespace lmsc
