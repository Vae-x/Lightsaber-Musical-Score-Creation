#pragma once
#include "core/MtpExportService.h"
#include <QDialog>
class QComboBox;
class QLineEdit;
class QPushButton;
class QLabel;
class SongExportDialog : public QDialog {
    Q_OBJECT
public:
    explicit SongExportDialog(MtpExportService *service, const QString &title, QWidget *parent = nullptr);
    QString parentDirectory() const;
    bool toDevice() const;
    MtpExportDestination destination() const;
    void populateDestinations(const QVector<MtpExportDestination> &destinations);
private:
    void updateSelection();
    void refresh();
    MtpExportService *m_service;
    QComboBox *m_mode, *m_destinations;
    QLineEdit *m_directory;
    QLabel *m_status;
    QPushButton *m_refresh, *m_export;
    QVector<MtpExportDestination> m_entries;
};
