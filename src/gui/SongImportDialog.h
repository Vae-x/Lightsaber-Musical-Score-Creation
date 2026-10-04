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
class QCloseEvent;
class MtpDeleteService;

// Local import and explicit management of individual portable-device songs.
class SongImportDialog : public QDialog {
    Q_OBJECT
public:
    explicit SongImportDialog(MtpImportService *service, QWidget *parent = nullptr);
    QString localPath() const { return m_localPath; }
    bool fromDevice() const { return m_fromDevice; }
    MtpSongEntry selectedSong() const { return m_selected; }
    void setLocalPath(const QString &path);
    void populateSongs(const QVector<MtpSongEntry> &entries);
    MtpDeleteService *deleteService() const { return m_deleteService; }
public slots:
    void done(int result) override;
    void reject() override;
    void accept() override;
protected:
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    void closeEvent(QCloseEvent *event) override;
private:
    void refresh();
    void filterTree();
    void updateLocalSelection();
    void updateDeviceSelection();
    void deleteSelectedSong();
    void confirmPreparedDeletion(const MtpSongEntry &song, int files, qint64 bytes);
    void updateOperationState();
    bool operationsBusy() const;
    MtpImportService *m_service;
    MtpDeleteService *m_deleteService;
    QFileSystemModel *m_files;
    QTreeView *m_localTree;
    QTreeWidget *m_deviceTree;
    QTabWidget *m_tabs;
    QLineEdit *m_path, *m_search;
    QLabel *m_status;
    QPushButton *m_localImport, *m_deviceImport, *m_refresh, *m_deviceDelete, *m_cancelButton;
    QString m_localPath;
    QString m_deviceNotice, m_deletingSongName;
    MtpSongEntry m_selected;
    bool m_fromDevice = false;
    bool m_confirmingDelete = false;
    bool m_deletionCommitted = false;
};
