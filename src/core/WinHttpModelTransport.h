#pragma once

#include <QObject>

#ifdef Q_OS_WIN
#include <QByteArray>
#include <QMap>
#include <QUrl>
#include <memory>

class QThread;

namespace lmsc {
struct WinHttpRequestState;

// Uses Windows' certificate store and TLS implementation. Request workers own
// asynchronous WinHTTP handles, so cancellation never races a synchronous call.
class WinHttpModelTransport : public QObject {
    Q_OBJECT
public:
    explicit WinHttpModelTransport(QObject *parent = nullptr);
    ~WinHttpModelTransport() override;
    void fetch(const QUrl &url, const QString &key, const QString &providerId, int timeoutMs);
    void cancel();

signals:
    void replyReady(int status, const QByteArray &contents, const QString &error);

private:
    quint64 m_generation = 0;
    QMap<QThread *, std::shared_ptr<WinHttpRequestState>> m_tasks;
};

} // namespace lmsc
#endif
