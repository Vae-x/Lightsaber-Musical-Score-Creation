#include "SongExportDialog.h"
#include "core/WorkspacePaths.h"
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QTimer>
#include <QVBoxLayout>

SongExportDialog::SongExportDialog(MtpExportService *service, const QString &title, QWidget *parent)
    : QDialog(parent), m_service(service) {
    setObjectName("songExportDialog"); setWindowTitle(tr("导出歌曲")); resize(760, 560); setMinimumSize(540, 400);
    auto outer = new QVBoxLayout(this);
    auto scroll = new QScrollArea(this); scroll->setObjectName("songExportScroll");
    scroll->setWidgetResizable(true); scroll->setFrameShape(QFrame::NoFrame);
    auto content = new QWidget(scroll); auto layout = new QVBoxLayout(content); layout->setContentsMargins(0, 0, 0, 0);
    scroll->setWidget(content); outer->addWidget(scroll, 1);
    auto song = new QLabel(tr("歌曲：%1").arg(title), this); song->setWordWrap(true); layout->addWidget(song);
    auto form = new QFormLayout;
    m_mode = new QComboBox(this); m_mode->setObjectName("songExportMode");
    m_mode->addItems({tr("导出到电脑，手动复制"), tr("导出到 PICO 头显")}); form->addRow(tr("导出目标"), m_mode);
    layout->addLayout(form);
    auto local = new QGroupBox(tr("电脑保存位置（头显导出也会保留完整副本）"), this); auto localLayout = new QVBoxLayout(local);
    auto row = new QHBoxLayout;
    m_directory = new QLineEdit(local); m_directory->setObjectName("songExportParentDirectory");
    m_directory->setText(QDir::toNativeSeparators(lmsc::WorkspacePaths::projectsDirectory()));
    auto browse = new QPushButton(tr("选择目录…"), local); row->addWidget(m_directory, 1); row->addWidget(browse); localLayout->addLayout(row);
    auto naming = new QLabel(tr("新建：光剑曲谱制作 / 歌曲名-by光剑曲谱\n重名追加 -2、-3；完成后可打开歌曲目录。"), local); naming->setWordWrap(true); naming->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Minimum); localLayout->addWidget(naming); layout->addWidget(local);
    auto device = new QGroupBox(tr("已连接的设备与游戏"), this); auto deviceLayout = new QVBoxLayout(device);
    auto deviceRow = new QHBoxLayout;
    m_destinations = new QComboBox(device); m_destinations->setObjectName("songExportDestinations");
    m_refresh = new QPushButton(tr("刷新设备"), device); m_refresh->setObjectName("refreshExportDestinations");
    deviceRow->addWidget(m_destinations, 1); deviceRow->addWidget(m_refresh); deviceLayout->addLayout(deviceRow);
    m_status = new QLabel(tr("正在检测 PICO…电脑导出可继续使用。"), device); m_status->setObjectName("mtpExportStatus"); m_status->setWordWrap(true); deviceLayout->addWidget(m_status);
    auto compatibility = new QLabel(tr("设备端只新增“光剑曲谱制作”分类中的歌曲，不覆盖已有内容。\n星穹绿洲的分类目录识别，以及新版歌曲游玩效果，仍需在游戏中验证。"), device); compatibility->setWordWrap(true); compatibility->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Minimum); deviceLayout->addWidget(compatibility); layout->addWidget(device);
    layout->addStretch();
    auto buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this); buttons->button(QDialogButtonBox::Cancel)->setText(tr("取消"));
    m_export = buttons->addButton(tr("导出歌曲"), QDialogButtonBox::AcceptRole); m_export->setObjectName("confirmSongExport"); outer->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept); connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(this, &QDialog::finished, this, [this] { if (m_service->isBusy()) m_service->cancel(); });
    connect(browse, &QPushButton::clicked, this, [this] { const QString path = QFileDialog::getExistingDirectory(this, tr("选择电脑导出位置"), m_directory->text()); if (!path.isEmpty()) m_directory->setText(QDir::toNativeSeparators(path)); });
    connect(m_directory, &QLineEdit::textChanged, this, &SongExportDialog::updateSelection);
    connect(m_mode, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &SongExportDialog::updateSelection);
    connect(m_destinations, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &SongExportDialog::updateSelection);
    connect(m_refresh, &QPushButton::clicked, this, &SongExportDialog::refresh);
    connect(m_service, &MtpExportService::destinationsListed, this, &SongExportDialog::populateDestinations);
    connect(m_service, &MtpExportService::errorOccurred, this, [this](const QString &error) { m_status->setText(error); m_refresh->setEnabled(true); updateSelection(); });
    connect(m_service, &MtpExportService::cancelled, this, [this] { m_refresh->setEnabled(true); updateSelection(); });
    updateSelection(); QTimer::singleShot(0, this, &SongExportDialog::refresh);
}
QString SongExportDialog::parentDirectory() const { return QFileInfo(m_directory->text().trimmed()).absoluteFilePath(); }
bool SongExportDialog::toDevice() const { return m_mode->currentIndex() == 1; }
MtpExportDestination SongExportDialog::destination() const { const int index = m_destinations->currentIndex(); return index >= 0 && index < m_entries.size() ? m_entries.at(index) : MtpExportDestination(); }
void SongExportDialog::updateSelection() {
    const bool localValid = !m_directory->text().trimmed().isEmpty() && QFileInfo(m_directory->text().trimmed()).isDir();
    m_destinations->setEnabled(toDevice() && !m_entries.isEmpty() && !m_service->isBusy());
    m_export->setEnabled(localValid && (!toDevice() || (!m_service->isBusy() && m_destinations->currentIndex() >= 0)));
}
void SongExportDialog::refresh() {
    if (m_service->isBusy()) return;
    m_entries.clear(); m_destinations->clear(); m_refresh->setEnabled(false);
    m_status->setText(tr("正在检测设备与已存在的游戏歌曲目录…")); m_service->listDestinations(); updateSelection();
}
void SongExportDialog::populateDestinations(const QVector<MtpExportDestination> &destinations) {
    m_entries = destinations; m_destinations->clear();
    for (const auto &entry : destinations) m_destinations->addItem(entry.deviceName + QStringLiteral(" · ") + entry.gameName + QStringLiteral(" · ") + entry.storageName);
    m_status->setText(destinations.isEmpty() ? tr("没有可用的头显游戏目录。请连接 PICO、允许 USB 文件传输，并先运行对应游戏。电脑导出可继续使用。") : tr("请选择游戏；点击“导出歌曲”后才开始上传。"));
    m_refresh->setEnabled(true); updateSelection();
}
