#include "SongImportDialog.h"
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileInfo>
#include <QFileSystemModel>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMimeData>
#include <QPushButton>
#include <QStandardPaths>
#include <QTabWidget>
#include <QTimer>
#include <QTreeView>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>
#include <functional>

namespace {
bool localSong(const QString &path) {
    QFileInfo info(path);
    if (info.isFile()) return info.suffix().compare("zip", Qt::CaseInsensitive) == 0;
    if (!info.isDir()) return false;
    for (const auto &file : QDir(path).entryList(QDir::Files))
        if (file.compare("Info.dat", Qt::CaseInsensitive) == 0) return true;
    return false;
}
QTreeWidgetItem *node(QTreeWidgetItem *parent, const QString &key, const QString &text) {
    for (int i = 0; i < parent->childCount(); ++i)
        if (parent->child(i)->data(0, Qt::UserRole + 1).toString() == key) return parent->child(i);
    auto child = new QTreeWidgetItem(parent, {text});
    child->setData(0, Qt::UserRole + 1, key);
    return child;
}
}

SongImportDialog::SongImportDialog(MtpImportService *service, QWidget *parent)
    : QDialog(parent), m_service(service) {
    setObjectName("songImportDialog"); setWindowTitle(tr("导入歌曲"));
    resize(880, 610); setMinimumSize(600, 430); setAcceptDrops(true);
    auto layout = new QVBoxLayout(this);
    auto hint = new QLabel(tr("选择含 Info.dat 的歌曲文件夹或 ZIP；也可把它拖入窗口。头显歌曲会先复制到电脑。"), this);
    hint->setWordWrap(true); layout->addWidget(hint);
    m_tabs = new QTabWidget(this); m_tabs->setObjectName("songImportTabs"); layout->addWidget(m_tabs, 1);
    auto computer = new QWidget(m_tabs); auto localLayout = new QVBoxLayout(computer);
    auto pathRow = new QHBoxLayout;
    auto up = new QPushButton(tr("上一级"), computer);
    m_path = new QLineEdit(computer); m_path->setObjectName("localSongPath");
    m_path->setPlaceholderText(tr("输入歌曲文件夹或 ZIP 路径，按回车浏览"));
    auto drives = new QComboBox(computer); drives->setObjectName("localSongDrives");
    drives->addItem(tr("此电脑"), QString());
    for (const auto &drive : QDir::drives()) drives->addItem(QDir::toNativeSeparators(drive.absoluteFilePath()), drive.absoluteFilePath());
    pathRow->addWidget(drives); pathRow->addWidget(up); pathRow->addWidget(m_path, 1); localLayout->addLayout(pathRow);
    m_files = new QFileSystemModel(this); m_files->setFilter(QDir::AllDirs | QDir::Files | QDir::NoDotAndDotDot);
    m_files->setNameFilters({"*.zip"}); m_files->setNameFilterDisables(false); m_files->setRootPath(QString());
    m_localTree = new QTreeView(computer); m_localTree->setObjectName("localSongTree"); m_localTree->setModel(m_files);
    m_localTree->setSelectionMode(QAbstractItemView::SingleSelection); m_localTree->setSortingEnabled(true);
    m_localTree->sortByColumn(0, Qt::AscendingOrder); m_localTree->setColumnWidth(0, 370);
    m_localTree->setHeaderHidden(true);
    m_localTree->hideColumn(1); m_localTree->hideColumn(2); m_localTree->hideColumn(3);
    localLayout->addWidget(m_localTree, 1);
    m_localImport = new QPushButton(tr("导入选中目录或 ZIP"), computer); m_localImport->setObjectName("importComputerSong");
    m_localImport->setEnabled(false); localLayout->addWidget(m_localImport);
    m_tabs->addTab(computer, tr("电脑"));

    auto device = new QWidget(m_tabs); auto deviceLayout = new QVBoxLayout(device);
    auto deviceHint = new QLabel(tr("连接并解锁 PICO，允许 USB 文件传输。展开游戏与分类文件夹，选择歌曲；读取期间可以取消。"), device);
    deviceHint->setWordWrap(true); deviceLayout->addWidget(deviceHint);
    m_search = new QLineEdit(device); m_search->setObjectName("mtpSongSearch"); m_search->setPlaceholderText(tr("搜索游戏、分类或歌曲")); deviceLayout->addWidget(m_search);
    m_deviceTree = new QTreeWidget(device); m_deviceTree->setObjectName("mtpSongTree");
    m_deviceTree->setHeaderLabels({tr("设备 / 游戏 / 分类 / 歌曲")}); m_deviceTree->setSelectionMode(QAbstractItemView::SingleSelection);
    m_deviceTree->setHeaderHidden(true);
    deviceLayout->addWidget(m_deviceTree, 1);
    m_status = new QLabel(tr("切换到本页或点击刷新，读取头显歌曲目录。"), device); m_status->setObjectName("mtpImportStatus");
    m_status->setWordWrap(true); deviceLayout->addWidget(m_status);
    auto deviceRow = new QHBoxLayout;
    m_refresh = new QPushButton(tr("刷新头显歌曲"), device); m_refresh->setObjectName("refreshHeadsetSongs");
    m_deviceImport = new QPushButton(tr("导入选中歌曲"), device); m_deviceImport->setObjectName("importHeadsetSong"); m_deviceImport->setEnabled(false);
    deviceRow->addWidget(m_refresh); deviceRow->addWidget(m_deviceImport); deviceLayout->addLayout(deviceRow);
    m_tabs->addTab(device, tr("PICO 头显"));
    auto buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this); buttons->button(QDialogButtonBox::Cancel)->setText(tr("取消")); layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(this, &QDialog::finished, this, [this] { if (m_service->isBusy()) m_service->cancel(); });
    connect(up, &QPushButton::clicked, this, [this] { QDir dir(m_path->text()); if (dir.cdUp()) setLocalPath(dir.absolutePath()); else { m_localTree->setRootIndex({}); m_path->clear(); } });
    connect(drives, QOverload<int>::of(&QComboBox::activated), this, [this, drives](int index) { const QString path = drives->itemData(index).toString(); if (path.isEmpty()) { m_localTree->setRootIndex({}); m_path->clear(); } else setLocalPath(path); });
    connect(m_path, &QLineEdit::returnPressed, this, [this] { setLocalPath(m_path->text()); });
    connect(m_path, &QLineEdit::textChanged, this, &SongImportDialog::updateLocalSelection);
    connect(m_localTree->selectionModel(), &QItemSelectionModel::currentChanged, this, [this](const QModelIndex &index) { m_path->setText(QDir::toNativeSeparators(m_files->filePath(index))); });
    connect(m_localTree, &QTreeView::doubleClicked, this, [this](const QModelIndex &index) { const QString path = m_files->filePath(index); if (QFileInfo(path).isDir()) setLocalPath(path); else if (localSong(path)) { m_localPath = path; accept(); } });
    connect(m_localImport, &QPushButton::clicked, this, [this] { if (localSong(m_path->text())) { m_localPath = QFileInfo(m_path->text()).absoluteFilePath(); accept(); } });
    connect(m_search, &QLineEdit::textChanged, this, &SongImportDialog::filterTree);
    connect(m_deviceTree, &QTreeWidget::itemSelectionChanged, this, &SongImportDialog::updateDeviceSelection);
    connect(m_deviceImport, &QPushButton::clicked, this, [this] { auto item = m_deviceTree->currentItem(); if (!item || m_service->isBusy() || !item->data(0, Qt::UserRole).isValid()) return; m_selected = item->data(0, Qt::UserRole).value<MtpSongEntry>(); m_fromDevice = true; accept(); });
    connect(m_refresh, &QPushButton::clicked, this, &SongImportDialog::refresh);
    connect(m_tabs, &QTabWidget::currentChanged, this, [this](int index) { if (index == 1 && !m_deviceTree->topLevelItemCount()) refresh(); });
    connect(m_service, &MtpImportService::songsListed, this, &SongImportDialog::populateSongs);
    connect(m_service, &MtpImportService::taskProgress, this, [this](const QString &text, int) { m_status->setText(text); });
    connect(m_service, &MtpImportService::errorOccurred, this, [this](const QString &text) { m_status->setText(text); m_refresh->setEnabled(true); updateDeviceSelection(); });
    connect(m_service, &MtpImportService::cancelled, this, [this] { m_status->setText(tr("读取已取消，可重新刷新。")); m_refresh->setEnabled(true); updateDeviceSelection(); });
    setLocalPath(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation));
}

void SongImportDialog::setLocalPath(const QString &path) {
    const QFileInfo info(QDir::fromNativeSeparators(path));
    const QString absolute = info.absoluteFilePath(); m_path->setText(QDir::toNativeSeparators(absolute));
    if (info.isDir()) m_localTree->setRootIndex(m_files->index(absolute));
    else if (info.isFile()) { m_localTree->setRootIndex(m_files->index(info.absolutePath())); m_localTree->setCurrentIndex(m_files->index(absolute)); }
    updateLocalSelection();
}
void SongImportDialog::updateLocalSelection() { m_localImport->setEnabled(localSong(m_path->text())); }
void SongImportDialog::refresh() {
    if (m_service->isBusy()) return;
    m_deviceTree->clear(); m_deviceImport->setEnabled(false); m_refresh->setEnabled(false);
    m_status->setText(tr("正在扫描游戏和分类目录…")); m_service->listSongs();
}
void SongImportDialog::populateSongs(const QVector<MtpSongEntry> &entries) {
    m_deviceTree->clear(); auto invisible = m_deviceTree->invisibleRootItem(); int count = 0;
    for (const auto &entry : entries) {
        auto device = node(invisible, entry.deviceName, entry.deviceName);
        const QString game = entry.gameName.isEmpty() ? tr("歌曲目录") : entry.gameName;
        auto gameNode = node(device, entry.gameId + "/" + game + "/" + entry.storageName,
            game + (entry.storageName.isEmpty() ? QString() : QStringLiteral(" · ") + entry.storageName));
        gameNode->setToolTip(0, entry.location);
        auto parent = gameNode;
        for (const auto &category : entry.categorySegments) parent = node(parent, category, category);
        if (!entry.isSong) continue;
        auto song = new QTreeWidgetItem(parent, {entry.name}); song->setData(0, Qt::UserRole, QVariant::fromValue(entry)); song->setToolTip(0, entry.location); ++count;
    }
    m_deviceTree->sortItems(0, Qt::AscendingOrder); m_deviceTree->expandToDepth(2);
    m_status->setText(tr("完整扫描找到 %1 首歌曲；选择歌曲节点导入。空游戏目录可用于后续导出。").arg(count));
    m_refresh->setEnabled(true); filterTree(); updateDeviceSelection();
}
void SongImportDialog::updateDeviceSelection() {
    auto item = m_deviceTree->currentItem();
    m_deviceImport->setEnabled(item && !item->isHidden() && item->data(0, Qt::UserRole).isValid() && !m_service->isBusy());
}
void SongImportDialog::filterTree() {
    const QString text = m_search->text().trimmed();
    std::function<bool(QTreeWidgetItem *, bool)> filter = [&](QTreeWidgetItem *item, bool parentMatches) {
        const bool matches = parentMatches || text.isEmpty() || item->text(0).contains(text, Qt::CaseInsensitive);
        bool visible = matches;
        for (int i = 0; i < item->childCount(); ++i) visible = filter(item->child(i), matches) || visible;
        item->setHidden(!visible); if (!text.isEmpty() && visible) item->setExpanded(true); return visible;
    };
    for (int i = 0; i < m_deviceTree->topLevelItemCount(); ++i) filter(m_deviceTree->topLevelItem(i), false);
    updateDeviceSelection();
}
void SongImportDialog::dragEnterEvent(QDragEnterEvent *event) {
    if (event->mimeData()->hasUrls() && event->mimeData()->urls().size() == 1 && event->mimeData()->urls().first().isLocalFile()) event->acceptProposedAction();
}
void SongImportDialog::dropEvent(QDropEvent *event) {
    if (!event->mimeData()->hasUrls() || event->mimeData()->urls().size() != 1) return;
    const auto url = event->mimeData()->urls().first(); if (!url.isLocalFile()) return;
    m_tabs->setCurrentIndex(0); setLocalPath(url.toLocalFile()); event->acceptProposedAction();
}
