#include "DiagnosticLogDialog.h"
#include "core/DiagnosticLog.h"
#include <QDateTime>
#include <QDialog>
#include <QDir>
#include <QFileDialog>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QVBoxLayout>
namespace lmsc {
void showDiagnosticLog(QWidget *parent, const QString &jobId) {
    auto dialog=new QDialog(parent);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setAttribute(Qt::WA_StyledBackground);
    dialog->setObjectName("diagnosticLogDialog"); dialog->setWindowTitle(QObject::tr("诊断日志")); dialog->resize(850,550);
    auto layout=new QVBoxLayout(dialog);
    auto title=new QLabel(jobId.isEmpty() ? QObject::tr("最近七天的诊断记录") : QObject::tr("本次任务的诊断记录"),dialog);
    title->setProperty("role", "title"); layout->addWidget(title);
    auto text=new QPlainTextEdit(dialog); text->setObjectName("diagnosticLogText"); text->setReadOnly(true);
    text->setMaximumBlockCount(1500); layout->addWidget(text,1);
    auto status=new QLabel(dialog); status->setWordWrap(true); layout->addWidget(status);
    const auto refresh=[text,status,jobId] {
        auto &log=DiagnosticLog::instance();
        const bool bottom=text->verticalScrollBar()->value()==text->verticalScrollBar()->maximum();
        text->setPlainText(QString::fromUtf8(log.read(jobId)));
        if (bottom) text->verticalScrollBar()->setValue(text->verticalScrollBar()->maximum());
        status->setText(!log.lastError().isEmpty() ? log.lastError() : log.isEnabled()
            ? QObject::tr("日志不包含密钥、账号内容、歌曲路径或模型全文。") : QObject::tr("日志已关闭，当前仅显示此前的记录。"));
    };
    auto buttons=new QHBoxLayout;
    auto exportButton=new QPushButton(QObject::tr("导出日志"),dialog); exportButton->setObjectName("exportDiagnosticLog");
    auto close=new QPushButton(QObject::tr("关闭"),dialog);
    buttons->addWidget(exportButton); buttons->addStretch(); buttons->addWidget(close); layout->addLayout(buttons);
    QObject::connect(exportButton,&QPushButton::clicked,dialog,[dialog,status,jobId] {
        const QString suggested=QDir::home().filePath(QStringLiteral("光剑曲谱制作-诊断-%1.jsonl").arg(QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss-zzz")));
        const auto file=QFileDialog::getSaveFileName(dialog,QObject::tr("导出诊断日志"),suggested,QObject::tr("诊断日志 (*.jsonl)"));
        if (file.isEmpty()) return;
        QString error;
        status->setText(DiagnosticLog::instance().exportTo(file,jobId,&error) ? QObject::tr("日志已导出。") : error);
    });
    QObject::connect(close,&QPushButton::clicked,dialog,&QDialog::close);
    QObject::connect(&DiagnosticLog::instance(),&DiagnosticLog::updated,dialog,refresh);
    refresh(); dialog->show();
}
}
