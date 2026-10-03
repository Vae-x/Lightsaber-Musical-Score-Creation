#pragma once
#include "core/MtpImportService.h"
#include <QDialog>
class QFileSystemModel;
class QLineEdit;
class QPushButton;
class QTabWidget;
class QTreeView;
class QTreeWidget;
class QLabel;

// One entry point for local directories/ZIPs and read-only portable-device copies.
class SongImportDialog : public QDialog {
    Q_OBJECT
public:
    explicit SongImportDialog(MtpImportService *service, QWidget *parent = nullptr);
    QString localPath() const { return m_localPath; }
    bool fromDevice() const { return m_fromDevice; }
    MtpSongEntry selectedSong() const { return m_selected; }
    void setLocalPath(const QString &path);
    void populateSongs(const QVector<MtpSongEntry> &entries);
protected:
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;
private:
    void refresh();
    void filterTree();
    void updateLocalSelection();
    void updateDeviceSelection();
    MtpImportService *m_service;
    QFileSystemModel *m_files;
    QTreeView *m_localTree;
    QTreeWidget *m_deviceTree;
    QTabWidget *m_tabs;
    QLineEdit *m_path, *m_search;
    QLabel *m_status;
    QPushButton *m_localImport, *m_deviceImport, *m_refresh;
    QString m_localPath;
    MtpSongEntry m_selected;
    bool m_fromDevice = false;
};
