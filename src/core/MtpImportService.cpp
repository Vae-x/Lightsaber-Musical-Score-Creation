#include "MtpImportService.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcessEnvironment>
#include <QUuid>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

MtpImportService::MtpImportService(QObject *parent) : QObject(parent) {
    qRegisterMetaType<MtpSongEntry>();
    qRegisterMetaType<QVector<MtpSongEntry>>();
#ifdef Q_OS_WIN
    m_process.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *arguments) {
        arguments->flags |= CREATE_NO_WINDOW;
        arguments->startupInfo->dwFlags |= STARTF_USESHOWWINDOW;
        arguments->startupInfo->wShowWindow = SW_HIDE;
    });
#endif
    m_timeout.setSingleShot(true);
    connect(&m_process, &QProcess::readyReadStandardOutput, this, &MtpImportService::readOutput);
    connect(&m_process, &QProcess::readyReadStandardError, this, [this] {
        m_stderr += m_process.readAllStandardError();
        if (m_stderr.size() > 16384) m_stderr = m_stderr.right(16384);
    });
    connect(&m_process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &MtpImportService::finish);
    connect(&m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart && isBusy())
            fail(QStringLiteral("无法启动 Windows 便携设备导入工具：%1").arg(m_process.errorString()));
    });
    connect(&m_timeout, &QTimer::timeout, this, [this] {
        m_timedOut = true;
        m_error = QStringLiteral("读取头显超时。请确认头显已解锁、USB 文件传输已允许，再重新连接。");
        m_process.kill();
    });
}

MtpImportService::~MtpImportService() {
    m_timeout.stop();
    m_process.disconnect(this);
    if (m_process.state() != QProcess::NotRunning) {
        m_process.kill();
        m_process.waitForFinished(3000);
    }
}

void MtpImportService::setScriptPath(const QString &path) { m_script = path; }

QString MtpImportService::scriptPath() const {
    if (!m_script.isEmpty()) return m_script;
    const QString bundled = QCoreApplication::applicationDirPath() + QStringLiteral("/tools/mtp/mtp-import.ps1");
    if (QFileInfo::exists(bundled)) return bundled;
    QDir directory(QCoreApplication::applicationDirPath());
    for (int level = 0; level < 8; ++level) {
        const QString development = directory.filePath(QStringLiteral("scripts/windows/mtp-import.ps1"));
        if (QFileInfo::exists(development)) return development;
        if (!directory.cdUp()) break;
    }
    return QString();
}

QString MtpImportService::powershellPath() const {
    const QString windows = qEnvironmentVariable("SystemRoot", QStringLiteral("C:/Windows"));
    // 32 位 Qt 在 64 位 Windows 下通过 Sysnative 使用与资源管理器一致的 Shell。
#if defined(Q_OS_WIN) && !defined(_WIN64)
    const QString native = windows + QStringLiteral("/Sysnative/WindowsPowerShell/v1.0/powershell.exe");
    if (QFileInfo::exists(native)) return native;
#endif
    return windows + QStringLiteral("/System32/WindowsPowerShell/v1.0/powershell.exe");
}

bool MtpImportService::isBusy() const { return m_operation != Operation::None; }

void MtpImportService::listSongs() { start(Operation::List); }

void MtpImportService::importSong(const MtpSongEntry &entry) {
    const QJsonDocument locator = QJsonDocument::fromJson(entry.locator.toUtf8());
    if (!locator.isObject() || locator.object().value(QStringLiteral("device")).toString().isEmpty()
            || locator.object().value(QStringLiteral("segments")).toArray().isEmpty()) {
        emit errorOccurred(QStringLiteral("歌曲的设备定位信息无效，请重新刷新头显歌曲列表。"));
        return;
    }
    start(Operation::Import, entry.locator);
}

void MtpImportService::start(Operation operation, const QString &locator) {
    if (isBusy()) {
        emit errorOccurred(QStringLiteral("正在读取头显，请等待完成或先取消。"));
        return;
    }
#ifndef Q_OS_WIN
    Q_UNUSED(operation)
    Q_UNUSED(locator)
    emit errorOccurred(QStringLiteral("从头显读取歌曲目前仅支持 Windows。"));
    return;
#endif
    const QString script = scriptPath();
    if (script.isEmpty() || !QFileInfo(script).isFile()) {
        emit errorOccurred(QStringLiteral("缺少设备导入脚本 tools/mtp/mtp-import.ps1，请重新解压完整的软件包。"));
        return;
    }
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("LMSC_MTP_MODE"), operation == Operation::List ? QStringLiteral("list") : QStringLiteral("import"));
    environment.insert(QStringLiteral("LMSC_MTP_LOCATOR"), locator);
    m_target.clear();
    if (operation == Operation::Import) {
        std::unique_ptr<QTemporaryDir> session(new QTemporaryDir(QDir::tempPath() + QStringLiteral("/lmsc-mtp-XXXXXX")));
        if (!session->isValid()) {
            emit errorOccurred(QStringLiteral("无法创建头显歌曲的本地临时目录。"));
            return;
        }
        const QString token = QUuid::createUuid().toString(QUuid::WithoutBraces);
        QFile marker(session->path() + QStringLiteral("/.lmsc-mtp-session"));
        if (!marker.open(QIODevice::WriteOnly) || marker.write(token.toUtf8()) != token.toUtf8().size()
                || !QDir(session->path()).mkdir(QStringLiteral("song"))) {
            emit errorOccurred(QStringLiteral("无法准备头显歌曲的本地临时副本。"));
            return;
        }
        marker.close();
        m_target = session->path() + QStringLiteral("/song");
        environment.insert(QStringLiteral("LMSC_MTP_SESSION"), QDir::toNativeSeparators(session->path()));
        environment.insert(QStringLiteral("LMSC_MTP_TARGET"), QDir::toNativeSeparators(m_target));
        environment.insert(QStringLiteral("LMSC_MTP_TOKEN"), token);
        m_sessions.emplace_back(std::move(session));
    }
    m_operation = operation;
    m_cancelled = m_done = m_timedOut = false;
    m_error.clear();
    m_stdout.clear();
    m_stderr.clear();
    m_songs.clear();
    m_process.setProcessEnvironment(environment);
    m_process.setProcessChannelMode(QProcess::SeparateChannels);
    m_process.setProgram(powershellPath());
    m_process.setArguments({QStringLiteral("-NoProfile"), QStringLiteral("-NonInteractive"), QStringLiteral("-STA"),
                           QStringLiteral("-ExecutionPolicy"), QStringLiteral("Bypass"), QStringLiteral("-File"),
                           QDir::toNativeSeparators(script)});
    m_timeout.start(operation == Operation::List ? 120000 : 900000);
    emit taskProgress(operation == Operation::List ? QStringLiteral("读取头显歌曲列表") : QStringLiteral("复制头显歌曲到本机"), -1);
    // 进度槽可能同步取消任务；不要在取消之后重新启动子进程。
    if (m_operation != operation || m_cancelled) return;
    m_process.start();
}

void MtpImportService::cancel() {
    if (!isBusy()) return;
    m_cancelled = true;
    m_timeout.stop();
    if (m_process.state() != QProcess::NotRunning) m_process.kill();
    else {
        m_operation = Operation::None;
        emit cancelled();
    }
}

void MtpImportService::readOutput() {
    m_stdout += m_process.readAllStandardOutput();
    int newline = -1;
    while ((newline = m_stdout.indexOf('\n')) >= 0) {
        const QByteArray line = m_stdout.left(newline).trimmed();
        m_stdout.remove(0, newline + 1);
        consumeLine(line);
    }
    if (m_stdout.size() > 2 * 1024 * 1024) {
        m_error = QStringLiteral("设备返回的数据超过限制，已中止读取。");
        m_process.kill();
    }
}

void MtpImportService::consumeLine(const QByteArray &line) {
    if (line.isEmpty()) return;
    const QJsonDocument document = QJsonDocument::fromJson(line);
    if (!document.isObject()) {
        m_error = QStringLiteral("设备导入工具返回了无效的数据。");
        return;
    }
    const QJsonObject object = document.object();
    const QString type = object.value(QStringLiteral("type")).toString();
    if (type == QStringLiteral("progress")) {
        emit taskProgress(object.value(QStringLiteral("message")).toString(), object.value(QStringLiteral("percent")).toInt(-1));
    } else if (type == QStringLiteral("error")) {
        m_error = object.value(QStringLiteral("message")).toString();
    } else if (type == QStringLiteral("songs")) {
        const auto songs = object.value(QStringLiteral("songs")).toArray();
        for (const auto &value : songs) {
            const auto song = value.toObject();
            MtpSongEntry entry;
            entry.name = song.value(QStringLiteral("name")).toString();
            entry.deviceName = song.value(QStringLiteral("deviceName")).toString();
            entry.location = song.value(QStringLiteral("location")).toString();
            entry.locator = QString::fromUtf8(QJsonDocument(song.value(QStringLiteral("locator")).toObject()).toJson(QJsonDocument::Compact));
            if (entry.name.isEmpty() || entry.locator.isEmpty()) {
                m_error = QStringLiteral("设备歌曲列表不完整，请刷新后重试。");
                return;
            }
            m_songs.append(entry);
        }
        m_done = true;
    } else if (type == QStringLiteral("imported")) {
        const QString returnedPath = QDir::cleanPath(object.value(QStringLiteral("path")).toString());
        if (returnedPath.compare(QDir::cleanPath(m_target), Qt::CaseInsensitive) != 0) {
            m_error = QStringLiteral("设备导入工具返回的本地目录不匹配。");
            return;
        }
        m_done = true;
    }
}

void MtpImportService::finish(int exitCode, QProcess::ExitStatus status) {
    if (!isBusy()) return;
    readOutput();
    if (!m_stdout.trimmed().isEmpty()) consumeLine(m_stdout.trimmed());
    m_stdout.clear();
    m_timeout.stop();
    const Operation operation = m_operation;
    m_operation = Operation::None;
    if (m_cancelled) {
        emit cancelled();
        return;
    }
    if (m_timedOut || status != QProcess::NormalExit || exitCode != 0 || !m_error.isEmpty() || !m_done) {
        QString message = m_error;
        if (message.isEmpty()) message = QStringLiteral("无法读取头显。请确认 Pico Neo 3 已解锁，并允许 USB 文件传输。 Windows PowerShell 返回代码：%1").arg(exitCode);
        emit errorOccurred(message);
        return;
    }
    emit taskProgress(QStringLiteral("头显读取完成"), 100);
    if (operation == Operation::List) emit songsListed(m_songs);
    else emit songImported(m_target);
}

void MtpImportService::fail(const QString &message) {
    m_timeout.stop();
    m_operation = Operation::None;
    emit errorOccurred(message);
}
