#include "SongImportDialog.h"
#include "core/MtpDeleteService.h"
#include <QCloseEvent>
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
#include <QMessageBox>
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
    : QDialog(parent), m_service(service), m_deleteService(new MtpDeleteService(this)) {
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
    m_localTree->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Ignored);
    m_localTree->setSelectionMode(QAbstractItemView::SingleSelection); m_localTree->setSortingEnabled(true);
    m_localTree->sortByColumn(0, Qt::AscendingOrder); m_localTree->setColumnWidth(0, 370);
    m_localTree->setHeaderHidden(true);
    m_localTree->hideColumn(1); m_localTree->hideColumn(2); m_localTree->hideColumn(3);
    localLayout->addWidget(m_localTree, 1);
    m_localImport = new QPushButton(tr("导入选中目录或 ZIP"), computer); m_localImport->setObjectName("importComputerSong");
    m_localImport->setEnabled(false); localLayout->addWidget(m_localImport);
    m_tabs->addTab(computer, tr("电脑"));

    auto device = new QWidget(m_tabs); auto deviceLayout = new QVBoxLayout(device);
    auto deviceHint = new QLabel(tr("连接并解锁 PICO，允许 USB 文件传输。选择歌曲可导入电脑，也可确认后删除头显歌曲。读取期间可以取消。"), device);
    deviceHint->setWordWrap(true); deviceLayout->addWidget(deviceHint);
    m_search = new QLineEdit(device); m_search->setObjectName("mtpSongSearch"); m_search->setPlaceholderText(tr("搜索游戏、分类或歌曲")); deviceLayout->addWidget(m_search);
    m_deviceTree = new QTreeWidget(device); m_deviceTree->setObjectName("mtpSongTree");
    m_deviceTree->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Ignored);
    m_deviceTree->setHeaderLabels({tr("设备 / 游戏 / 分类 / 歌曲")}); m_deviceTree->setSelectionMode(QAbstractItemView::SingleSelection);
    m_deviceTree->setHeaderHidden(true);
    deviceLayout->addWidget(m_deviceTree, 1);
    m_status = new QLabel(tr("切换到本页或点击刷新，读取头显歌曲目录。"), device); m_status->setObjectName("mtpImportStatus");
    m_status->setTextFormat(Qt::PlainText); m_status->setWordWrap(true); deviceLayout->addWidget(m_status);
    auto deviceRow = new QHBoxLayout;
    m_refresh = new QPushButton(tr("刷新头显歌曲"), device); m_refresh->setObjectName("refreshHeadsetSongs");
    m_deviceImport = new QPushButton(tr("导入选中歌曲"), device); m_deviceImport->setObjectName("importHeadsetSong"); m_deviceImport->setEnabled(false);
    m_deviceDelete = new QPushButton(tr("删除头显歌曲"), device); m_deviceDelete->setObjectName("deleteHeadsetSong"); m_deviceDelete->setEnabled(false);
    deviceRow->addWidget(m_refresh); deviceRow->addWidget(m_deviceImport); deviceRow->addWidget(m_deviceDelete); deviceLayout->addLayout(deviceRow);
    m_tabs->addTab(device, tr("PICO 头显"));
    auto buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this); m_cancelButton = buttons->button(QDialogButtonBox::Cancel); m_cancelButton->setText(tr("取消")); layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, this, &SongImportDialog::reject);
    connect(this, &QDialog::finished, this, [this] {
        if (m_service->isBusy()) m_service->cancel();
        if (!m_deletionCommitted) m_deleteService->cancel();
    });
    connect(up, &QPushButton::clicked, this, [this] { QDir dir(m_path->text()); if (dir.cdUp()) setLocalPath(dir.absolutePath()); else { m_localTree->setRootIndex({}); m_path->clear(); } });
    connect(drives, QOverload<int>::of(&QComboBox::activated), this, [this, drives](int index) { const QString path = drives->itemData(index).toString(); if (path.isEmpty()) { m_localTree->setRootIndex({}); m_path->clear(); } else setLocalPath(path); });
    connect(m_path, &QLineEdit::returnPressed, this, [this] { setLocalPath(m_path->text()); });
    connect(m_path, &QLineEdit::textChanged, this, &SongImportDialog::updateLocalSelection);
    connect(m_localTree->selectionModel(), &QItemSelectionModel::currentChanged, this, [this](const QModelIndex &index) { m_path->setText(QDir::toNativeSeparators(m_files->filePath(index))); });
    connect(m_localTree, &QTreeView::doubleClicked, this, [this](const QModelIndex &index) { const QString path = m_files->filePath(index); if (QFileInfo(path).isDir()) setLocalPath(path); else if (!operationsBusy() && localSong(path)) { m_localPath = path; accept(); } });
    connect(m_localImport, &QPushButton::clicked, this, [this] { if (!operationsBusy() && localSong(m_path->text())) { m_localPath = QFileInfo(m_path->text()).absoluteFilePath(); accept(); } });
    connect(m_search, &QLineEdit::textChanged, this, &SongImportDialog::filterTree);
    connect(m_deviceTree, &QTreeWidget::itemSelectionChanged, this, &SongImportDialog::updateDeviceSelection);
    connect(m_deviceImport, &QPushButton::clicked, this, [this] { auto item = m_deviceTree->currentItem(); if (!item || item->isHidden() || operationsBusy() || !item->data(0, Qt::UserRole).canConvert<MtpSongEntry>()) return; m_selected = item->data(0, Qt::UserRole).value<MtpSongEntry>(); if (!m_selected.isSong) return; m_fromDevice = true; accept(); });
    connect(m_deviceDelete, &QPushButton::clicked, this, &SongImportDialog::deleteSelectedSong);
    connect(m_refresh, &QPushButton::clicked, this, &SongImportDialog::refresh);
    connect(m_tabs, &QTabWidget::currentChanged, this, [this](int index) { if (index == 1 && !m_deviceTree->topLevelItemCount()) refresh(); });
    connect(m_service, &MtpImportService::songsListed, this, &SongImportDialog::populateSongs);
    connect(m_service, &MtpImportService::taskProgress, this, [this](const QString &text, int) { m_status->setText(text); updateOperationState(); });
    connect(m_service, &MtpImportService::errorOccurred, this, [this](const QString &text) { m_status->setText(text); updateOperationState(); });
    connect(m_service, &MtpImportService::cancelled, this, [this] { m_status->setText(tr("读取已取消，可重新刷新。")); updateOperationState(); });
    connect(m_deleteService, &MtpDeleteService::taskProgress, this, [this](const QString &text, int) { m_status->setText(text); updateOperationState(); });
    connect(m_deleteService, &MtpDeleteService::deletionPrepared, this, &SongImportDialog::confirmPreparedDeletion);
    connect(m_deleteService, &MtpDeleteService::songDeleted, this, [this] {
        m_deletionCommitted = false;
        m_deviceNotice = tr("已删除头显歌曲“%1”。").arg(m_deletingSongName);
        refresh();
    });
    connect(m_deleteService, &MtpDeleteService::errorOccurred, this, [this](const QString &text) {
        m_deviceTree->clear();
        m_deviceNotice = m_deletionCommitted
            ? tr("删除未完成，设备歌曲目录可能残留部分内容。请重新刷新头显歌曲。\n%1").arg(text)
            : tr("删除前检查未通过，请重新刷新头显歌曲。\n%1").arg(text);
        m_deletionCommitted = false;
        m_status->setText(m_deviceNotice); updateOperationState();
    });
    connect(m_deleteService, &MtpDeleteService::cancelled, this, [this] {
        if (m_deletionCommitted) {
            m_deviceTree->clear();
            m_deviceNotice = tr("删除操作已停止，请重新刷新并核对设备歌曲目录。已删除的文件无法恢复。");
        } else m_deviceNotice = tr("已取消删除，头显歌曲未改动。");
        m_deletionCommitted = false;
        m_status->setText(m_deviceNotice); updateOperationState();
    });
    setLocalPath(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation));
}

void SongImportDialog::setLocalPath(const QString &path) {
    const QFileInfo info(QDir::fromNativeSeparators(path));
    const QString absolute = info.absoluteFilePath(); m_path->setText(QDir::toNativeSeparators(absolute));
    if (info.isDir()) m_localTree->setRootIndex(m_files->index(absolute));
    else if (info.isFile()) { m_localTree->setRootIndex(m_files->index(info.absolutePath())); m_localTree->setCurrentIndex(m_files->index(absolute)); }
    updateLocalSelection();
}
void SongImportDialog::updateLocalSelection() { m_localImport->setEnabled(!operationsBusy() && localSong(m_path->text())); }
void SongImportDialog::refresh() {
    if (operationsBusy()) return;
    m_deviceTree->clear();
    m_status->setText(tr("正在扫描游戏和分类目录…")); m_service->listSongs();
    updateOperationState();
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
    const QString summary = tr("完整扫描找到 %1 首歌曲；选择歌曲节点导入或删除。空游戏目录可用于后续导出。").arg(count);
    m_status->setText(m_deviceNotice.isEmpty() ? summary : m_deviceNotice + QLatin1Char('\n') + summary);
    filterTree(); updateOperationState();
}
void SongImportDialog::updateDeviceSelection() {
    auto item = m_deviceTree->currentItem();
    const bool selected = item && !item->isHidden() && item->data(0, Qt::UserRole).canConvert<MtpSongEntry>()
        && item->data(0, Qt::UserRole).value<MtpSongEntry>().isSong && !operationsBusy();
    m_deviceImport->setEnabled(selected); m_deviceDelete->setEnabled(selected);
}
bool SongImportDialog::operationsBusy() const { return m_confirmingDelete || m_service->isBusy() || m_deleteService->isBusy(); }
void SongImportDialog::updateOperationState() {
    m_refresh->setEnabled(!operationsBusy());
    m_cancelButton->setEnabled(!m_deletionCommitted && !m_confirmingDelete);
    m_deviceTree->setEnabled(!m_deleteService->isBusy() && !m_confirmingDelete);
    m_search->setEnabled(!m_deleteService->isBusy() && !m_confirmingDelete);
    updateLocalSelection(); updateDeviceSelection();
}
void SongImportDialog::deleteSelectedSong() {
    auto item = m_deviceTree->currentItem();
    if (!item || item->isHidden() || operationsBusy() || !item->data(0, Qt::UserRole).canConvert<MtpSongEntry>()) return;
    const auto song = item->data(0, Qt::UserRole).value<MtpSongEntry>();
    if (!song.isSong) return;
    m_deviceNotice.clear(); m_deletingSongName = song.name;
    m_deleteService->prepareSong(song); updateOperationState();
}
void SongImportDialog::confirmPreparedDeletion(const MtpSongEntry &song, int files, qint64 bytes) {
    if (!isVisible()) { m_deleteService->cancel(); return; }
    const QString location = QStringLiteral("此电脑\\") + song.deviceName + QLatin1Char('\\') + QDir::toNativeSeparators(song.location);
    QMessageBox confirmation(QMessageBox::Warning, tr("删除头显歌曲"),
        tr("确定永久删除这首头显歌曲及其文件吗？\n\n设备：%1\n游戏：%2\n歌曲：%3\n路径：%4\n文件：%5 个；大小：%6 字节\n\n删除无法撤销。外层分类目录和电脑上的工程保留。")
            .arg(song.deviceName, song.gameName, song.name, location, QString::number(files), QString::number(bytes)), QMessageBox::Cancel, this);
    confirmation.setObjectName("confirmHeadsetSongDeletion");
    confirmation.setTextFormat(Qt::PlainText);
    confirmation.button(QMessageBox::Cancel)->setText(tr("取消"));
    auto remove = confirmation.addButton(tr("永久删除这首歌曲"), QMessageBox::DestructiveRole);
    remove->setObjectName("confirmDeleteHeadsetSong");
    confirmation.setDefaultButton(QMessageBox::Cancel); confirmation.setEscapeButton(QMessageBox::Cancel);
    m_confirmingDelete = true; updateOperationState(); confirmation.exec();
    m_confirmingDelete = false; updateOperationState();
    if (confirmation.clickedButton() != remove || m_service->isBusy() || m_deleteService->isBusy()) {
        m_deleteService->cancel();
        m_deviceNotice = tr("已取消删除，头显歌曲未改动。"); m_status->setText(m_deviceNotice);
        updateOperationState(); return;
    }
    m_deletionCommitted = true;
    m_deleteService->deletePrepared(); updateOperationState();
}
void SongImportDialog::done(int result) {
    if (m_deletionCommitted || m_confirmingDelete) {
        m_status->setText(tr("正在处理头显歌曲删除，请保持连接并等待完成。")); return;
    }
    QDialog::done(result);
}
void SongImportDialog::reject() { done(QDialog::Rejected); }
void SongImportDialog::accept() { done(QDialog::Accepted); }
void SongImportDialog::closeEvent(QCloseEvent *event) {
    if (m_deletionCommitted || m_confirmingDelete) {
        event->ignore(); m_status->setText(tr("正在处理头显歌曲删除，请保持连接并等待完成。")); return;
    }
    QDialog::closeEvent(event);
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
    if (operationsBusy()) return;
    if (event->mimeData()->hasUrls() && event->mimeData()->urls().size() == 1 && event->mimeData()->urls().first().isLocalFile()) event->acceptProposedAction();
}
void SongImportDialog::dropEvent(QDropEvent *event) {
    if (operationsBusy()) return;
    if (!event->mimeData()->hasUrls() || event->mimeData()->urls().size() != 1) return;
    const auto url = event->mimeData()->urls().first(); if (!url.isLocalFile()) return;
    m_tabs->setCurrentIndex(0); setLocalPath(url.toLocalFile()); event->acceptProposedAction();
}
