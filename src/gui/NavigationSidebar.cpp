#include "NavigationSidebar.h"
#include "ThemeManager.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QIconEngine>
#include <QKeyEvent>
#include <QLabel>
#include <QListWidget>
#include <QPainter>
#include <QPainterPath>
#include <QStyleOptionToolButton>
#include <QStylePainter>
#include <QStyledItemDelegate>
#include <QToolButton>
#include <QVBoxLayout>
#include <cstdlib>

namespace lmsc {
namespace {
constexpr int iconRole = Qt::UserRole + 1;

class NavigationIconEngine final : public QIconEngine {
public:
    explicit NavigationIconEngine(NavigationIcon icon) : m_icon(icon) {}
    QIconEngine *clone() const override { return new NavigationIconEngine(m_icon); }
    void paint(QPainter *painter, const QRect &rect, QIcon::Mode mode, QIcon::State) override {
        const auto group = mode == QIcon::Disabled ? QPalette::Disabled : QPalette::Active;
        drawNavigationIcon(*painter, m_icon, rect,
                           QApplication::palette().color(group, QPalette::Text));
    }
    QPixmap pixmap(const QSize &size, QIcon::Mode mode, QIcon::State state) override {
        QPixmap result(size);
        result.fill(Qt::transparent);
        QPainter painter(&result);
        paint(&painter, result.rect(), mode, state);
        return result;
    }

private:
    NavigationIcon m_icon;
};

class NavigationToggle final : public QToolButton {
public:
    explicit NavigationToggle(QWidget *parent) : QToolButton(parent) {
        setToolButtonStyle(Qt::ToolButtonIconOnly);
    }

protected:
    void keyPressEvent(QKeyEvent *event) override {
        if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter)
            && (event->modifiers() == Qt::NoModifier || event->modifiers() == Qt::KeypadModifier)) {
            event->accept();
            click();
            return;
        }
        QToolButton::keyPressEvent(event);
    }

    void paintEvent(QPaintEvent *) override {
        QStyleOptionToolButton option;
        initStyleOption(&option);
        option.text.clear();
        option.icon = QIcon();
        QStylePainter painter(this);
        painter.drawComplexControl(QStyle::CC_ToolButton, option);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QPen(palette().color(QPalette::ButtonText), 1.8,
                            Qt::SolidLine, Qt::RoundCap));
        const QPointF center = rect().center();
        for (int offset : {-6, 0, 6})
            painter.drawLine(QPointF(center.x() - 9, center.y() + offset),
                             QPointF(center.x() + 9, center.y() + offset));
        if (hasFocus()) {
            painter.setBrush(Qt::NoBrush);
            painter.setPen(QPen(palette().color(QPalette::Link), 2));
            painter.drawRoundedRect(QRectF(rect()).adjusted(3, 3, -3, -3), 7, 7);
        }
    }
};

class NavigationDelegate final : public QStyledItemDelegate {
public:
    explicit NavigationDelegate(NavigationSidebar *sidebar)
        : QStyledItemDelegate(sidebar), m_sidebar(sidebar) {}

    QSize sizeHint(const QStyleOptionViewItem &, const QModelIndex &) const override {
        return QSize(48, 48);
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override {
        QStyleOptionViewItem styled(option);
        initStyleOption(&styled, index);
        painter->save();
        painter->setClipRect(styled.rect);
        painter->setRenderHint(QPainter::Antialiasing);
        const QRectF bounds = QRectF(styled.rect).adjusted(1, 1, -1, -1);
        const bool selected = styled.state.testFlag(QStyle::State_Selected);
        const bool hovered = styled.state.testFlag(QStyle::State_MouseOver);
        const QColor accent = styled.palette.color(QPalette::Link);
        if (selected || hovered) {
            QColor background = selected ? accent : styled.palette.color(QPalette::Text);
            background.setAlpha(selected ? (ThemeManager::isDark() ? 42 : 26)
                                         : (ThemeManager::isDark() ? 18 : 10));
            painter->setPen(Qt::NoPen);
            painter->setBrush(background);
            painter->drawRoundedRect(bounds, 8, 8);
        }
        if (selected) {
            painter->setPen(Qt::NoPen);
            painter->setBrush(accent);
            painter->drawRoundedRect(QRectF(bounds.left(), bounds.center().y() - 10,
                                            3, 20), 1.5, 1.5);
        }
        const QPalette::ColorGroup group = styled.state.testFlag(QStyle::State_Enabled)
                                              ? QPalette::Active : QPalette::Disabled;
        const QColor foreground = selected ? accent : styled.palette.color(group, QPalette::Text);
        const qreal iconLeft = m_sidebar->isCollapsed() ? bounds.center().x() - 11
                                                       : bounds.left() + 14;
        drawNavigationIcon(*painter, static_cast<NavigationIcon>(index.data(iconRole).toInt()),
                           QRectF(iconLeft, bounds.center().y() - 11, 22, 22), foreground);
        if (!m_sidebar->isCollapsed()) {
            painter->setPen(styled.palette.color(group, QPalette::Text));
            QFont font = styled.font;
            font.setWeight(selected ? QFont::DemiBold : QFont::Normal);
            painter->setFont(font);
            const QRect textRect = bounds.adjusted(48, 0, -10, 0).toRect();
            painter->drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter,
                              QFontMetrics(font).elidedText(styled.text, Qt::ElideRight,
                                                           textRect.width()));
        }
        if (styled.state.testFlag(QStyle::State_HasFocus)) {
            painter->setBrush(Qt::NoBrush);
            painter->setPen(QPen(accent, 2));
            painter->drawRoundedRect(bounds.adjusted(1, 1, -1, -1), 7, 7);
        }
        painter->restore();
    }

private:
    NavigationSidebar *m_sidebar;
};
} // namespace

void drawNavigationIcon(QPainter &painter, NavigationIcon icon,
                        const QRectF &rect, const QColor &color) {
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    painter.translate(rect.topLeft());
    painter.scale(rect.width() / 24.0, rect.height() / 24.0);
    painter.setPen(QPen(color, 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.setBrush(Qt::NoBrush);
    switch (icon) {
    case NavigationIcon::Appearance: {
        painter.drawRoundedRect(QRectF(3, 3, 18, 18), 5, 5);
        QPainterPath half;
        half.moveTo(12, 3);
        half.lineTo(16, 3);
        half.quadTo(21, 3, 21, 8);
        half.lineTo(21, 16);
        half.quadTo(21, 21, 16, 21);
        half.lineTo(12, 21);
        half.closeSubpath();
        painter.fillPath(half, color);
        painter.drawLine(QPointF(12, 3), QPointF(12, 21));
        break;
    }
    case NavigationIcon::Model: {
        painter.drawRoundedRect(QRectF(5, 5, 14, 14), 3, 3);
        painter.drawRoundedRect(QRectF(9, 9, 6, 6), 1, 1);
        for (int point : {9, 15}) {
            painter.drawLine(QPointF(point, 2), QPointF(point, 5));
            painter.drawLine(QPointF(point, 19), QPointF(point, 22));
            painter.drawLine(QPointF(2, point), QPointF(5, point));
            painter.drawLine(QPointF(19, point), QPointF(22, point));
        }
        break;
    }
    case NavigationIcon::Account: {
        painter.drawEllipse(QRectF(8, 3, 8, 8));
        QPainterPath shoulders;
        shoulders.moveTo(4, 21);
        shoulders.lineTo(4, 19);
        shoulders.cubicTo(4, 12, 20, 12, 20, 19);
        shoulders.lineTo(20, 21);
        painter.drawPath(shoulders);
        break;
    }
    case NavigationIcon::Network: {
        painter.drawEllipse(QRectF(3, 3, 18, 18));
        painter.drawEllipse(QRectF(8, 3, 8, 18));
        painter.drawLine(QPointF(3, 12), QPointF(21, 12));
        break;
    }
    case NavigationIcon::About: {
        painter.drawEllipse(QRectF(3, 3, 18, 18));
        painter.drawLine(QPointF(12, 11), QPointF(12, 17));
        painter.setPen(Qt::NoPen);
        painter.setBrush(color);
        painter.drawEllipse(QPointF(12, 7.5), 1.1, 1.1);
        break;
    }
    case NavigationIcon::Editor: {
        painter.drawRoundedRect(QRectF(3, 3, 18, 18), 3, 3);
        painter.drawLine(QPointF(9, 3), QPointF(9, 21));
        painter.drawLine(QPointF(15, 3), QPointF(15, 21));
        painter.drawLine(QPointF(3, 9), QPointF(21, 9));
        painter.drawLine(QPointF(3, 15), QPointF(21, 15));
        painter.fillRect(QRectF(10, 10, 4, 4), color);
        break;
    }
    case NavigationIcon::Recognition: {
        painter.drawRoundedRect(QRectF(2, 3, 20, 18), 4, 4);
        for (int bar = 0; bar < 5; ++bar) {
            const int height = 4 + (2 - std::abs(2 - bar)) * 3;
            painter.drawLine(QPointF(6 + bar * 3, 12 - height / 2.0),
                             QPointF(6 + bar * 3, 12 + height / 2.0));
        }
        break;
    }
    }
    painter.restore();
}

NavigationSidebar::NavigationSidebar(QWidget *parent)
    : QWidget(parent), m_list(new QListWidget(this)),
      m_toggle(new NavigationToggle(this)), m_heading(new QLabel(tr("设置"), this)) {
    setAttribute(Qt::WA_StyledBackground, true);
    setFixedWidth(208);
    auto layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 12, 8, 12);
    layout->setSpacing(14);
    auto header = new QHBoxLayout;
    header->setContentsMargins(0, 0, 0, 0);
    header->setSpacing(10);
    m_toggle->setObjectName(QStringLiteral("navigationToggle"));
    m_toggle->setFixedSize(40, 40);
    m_toggle->setText(tr("收起导航"));
    m_toggle->setToolTip(m_toggle->text());
    m_toggle->setAccessibleName(m_toggle->text());
    m_toggle->setFocusPolicy(Qt::StrongFocus);
    m_heading->setProperty("role", "sidebarTitle");
    header->addWidget(m_toggle);
    header->addWidget(m_heading, 1);
    layout->addLayout(header);
    m_list->setFrameShape(QFrame::NoFrame);
    m_list->setSelectionMode(QAbstractItemView::SingleSelection);
    m_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_list->setSpacing(4);
    m_list->setMouseTracking(true);
    m_list->setFocusPolicy(Qt::StrongFocus);
    m_list->setAccessibleName(tr("设置导航"));
    m_list->setAccessibleDescription(tr("使用上下方向键切换设置页面。"));
    m_list->setItemDelegate(new NavigationDelegate(this));
    layout->addWidget(m_list, 1);
    connect(m_toggle, &QToolButton::clicked, this, [this] { setCollapsed(!m_collapsed); });
    setTabOrder(m_toggle, m_list);
}

void NavigationSidebar::addItem(const QString &text, NavigationIcon icon) {
    auto item = new QListWidgetItem(text, m_list);
    item->setIcon(QIcon(new NavigationIconEngine(icon)));
    item->setData(iconRole, static_cast<int>(icon));
    item->setToolTip(text);
    item->setData(Qt::AccessibleTextRole, text);
}

void NavigationSidebar::setCollapsed(bool collapsed) {
    if (m_collapsed == collapsed)
        return;
    m_collapsed = collapsed;
    setFixedWidth(collapsed ? 64 : 208);
    m_heading->setVisible(!collapsed);
    const QString action = collapsed ? tr("展开导航") : tr("收起导航");
    m_toggle->setText(action);
    m_toggle->setToolTip(action);
    m_toggle->setAccessibleName(action);
    m_list->viewport()->update();
    emit collapsedChanged(collapsed);
}

void NavigationSidebar::setHeadingText(const QString &text) { m_heading->setText(text); }

} // namespace lmsc
