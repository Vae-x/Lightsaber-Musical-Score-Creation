#pragma once

#include <QColor>
#include <QRectF>
#include <QWidget>

class QLabel;
class QListWidget;
class QMenu;
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
    void addSubItem(int parentRow, const QString &text, NavigationIcon icon);
    int parentRow(int row) const;
    bool isSubmenuExpanded(int row) const;
    void setSubmenuExpanded(int row, bool expanded);
    void setCollapsed(bool collapsed);
    void setHeadingText(const QString &text);
    bool isCollapsed() const { return m_collapsed; }

signals:
    void collapsedChanged(bool collapsed);

private:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void updateSubmenuVisibility();
    void showSubmenu(int row);
    QListWidget *m_list;
    QMenu *m_submenu = nullptr;
    QToolButton *m_toggle;
    QLabel *m_heading;
    bool m_collapsed = false;
};

} // namespace lmsc
