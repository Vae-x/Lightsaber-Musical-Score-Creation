#include "WinHttpModelTransport.h"

#ifdef Q_OS_WIN
#include <QThread>
#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>

namespace lmsc {

struct WinHttpRequestState {
    std::mutex mutex;
    std::condition_variable condition;
    bool cancelled = false;
    DWORD event = 0;
    DWORD errorCode = 0;
    DWORD dataSize = 0;
    int status = 0;
    QByteArray contents;
    std::array<char, 16384> buffer;
    QString error;
    std::chrono::steady_clock::time_point deadline;
};

namespace {

constexpr int maximumResponseSize = 4 * 1024 * 1024;
// MinGW 7's SDK predates this Windows 8.1 constant; the runtime is Windows 10+.
constexpr DWORD automaticProxyAccess = 4; // WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY

struct CallbackContext {
    std::shared_ptr<WinHttpRequestState> state;
};

void CALLBACK statusCallback(HINTERNET, DWORD_PTR contextValue, DWORD event,
                             LPVOID information, DWORD informationSize) {
    if (!contextValue) return;
    auto *context = reinterpret_cast<CallbackContext *>(contextValue);
    const auto state = context->state;
    if (event == WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING) {
        // This is the final callback. Until now its context must remain alive,
        // even after WinHttpCloseHandle has returned on the request worker.
        delete context;
        return;
    }
    std::lock_guard<std::mutex> lock(state->mutex);
    switch (event) {
    case WINHTTP_CALLBACK_STATUS_REQUEST_ERROR:
        if (information && informationSize >= sizeof(WINHTTP_ASYNC_RESULT))
            state->errorCode = static_cast<WINHTTP_ASYNC_RESULT *>(information)->dwError;
        else state->errorCode = ERROR_WINHTTP_INTERNAL_ERROR;
        state->event = event;
        break;
    case WINHTTP_CALLBACK_STATUS_DATA_AVAILABLE:
        state->dataSize = information && informationSize >= sizeof(DWORD)
                ? *static_cast<DWORD *>(information) : 0;
        state->event = event;
        break;
    case WINHTTP_CALLBACK_STATUS_READ_COMPLETE:
        state->dataSize = informationSize;
        state->event = event;
        break;
    case WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE:
    case WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE:
        state->event = event;
        break;
    default:
        return;
    }
    state->condition.notify_all();
}

QString nativeError(DWORD code) {
    if (code == ERROR_WINHTTP_TIMEOUT)
        return QStringLiteral("获取模型超时，请检查网络或服务地址后重试。");
    if (code == ERROR_WINHTTP_SECURE_FAILURE || code == ERROR_WINHTTP_CLIENT_AUTH_CERT_NEEDED
            || code == ERROR_WINHTTP_SECURE_CERT_DATE_INVALID
            || code == ERROR_WINHTTP_SECURE_CERT_CN_INVALID
            || code == ERROR_WINHTTP_SECURE_INVALID_CA)
        return QStringLiteral("TLS 安全连接失败，请检查系统时间、证书与服务地址。");
    return QStringLiteral("无法连接模型服务，请检查网络、系统代理与服务地址（Windows 网络错误 %1）。")
            .arg(code);
}

bool cancelled(const std::shared_ptr<WinHttpRequestState> &state) {
    std::lock_guard<std::mutex> lock(state->mutex);
    return state->cancelled;
}

bool prepareOperation(const std::shared_ptr<WinHttpRequestState> &state) {
    std::lock_guard<std::mutex> lock(state->mutex);
    if (state->cancelled) return false;
    state->event = 0;
    state->errorCode = 0;
    state->dataSize = 0;
    return true;
}

bool waitFor(const std::shared_ptr<WinHttpRequestState> &state, BOOL started,
             DWORD immediateError, DWORD expectedEvent) {
    if (!started && immediateError != ERROR_IO_PENDING) {
        state->error = nativeError(immediateError);
        return false;
    }
    std::unique_lock<std::mutex> lock(state->mutex);
    if (!state->condition.wait_until(lock, state->deadline, [&] {
        return state->cancelled || state->event == expectedEvent || state->errorCode != 0;
    })) {
        state->error = nativeError(ERROR_WINHTTP_TIMEOUT);
        return false;
    }
    if (state->cancelled) return false;
    if (state->errorCode) {
        state->error = nativeError(state->errorCode);
        return false;
    }
    return true;
}

// The worker owns every native handle and closes them exactly once. Pending
// asynchronous calls are cancelled by closing their request on this same thread.
struct Handles {
    HINTERNET session = nullptr;
    HINTERNET connection = nullptr;
    HINTERNET request = nullptr;
    ~Handles() {
        if (request) WinHttpCloseHandle(request);
        if (connection) WinHttpCloseHandle(connection);
        if (session) WinHttpCloseHandle(session);
    }
};

void performRequest(const std::shared_ptr<WinHttpRequestState> &state,
                    const QUrl &url, const QString &key,
                    const QString &providerId, int timeoutMs) {
    if (cancelled(state)) return;
    Handles handles;
    // Only loopback HTTP passed validation. Send local gateway credentials
    // directly; HTTPS uses the user's and system's automatic proxy settings.
    handles.session = WinHttpOpen(L"Lightsaber Musical Score Creation/0.2",
                                  url.scheme() == QStringLiteral("http")
                                      ? WINHTTP_ACCESS_TYPE_NO_PROXY : automaticProxyAccess,
                                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS,
                                  WINHTTP_FLAG_ASYNC);
    if (!handles.session) {
        state->error = nativeError(GetLastError());
        return;
    }
    if (!WinHttpSetTimeouts(handles.session, timeoutMs, timeoutMs, timeoutMs, timeoutMs)) {
        state->error = nativeError(GetLastError());
        return;
    }
    // TLS 1.2 is available on the Windows 10 target. Certificate verification
    // remains the operating system default; no security-ignore flags are set.
    DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
    if (!WinHttpSetOption(handles.session, WINHTTP_OPTION_SECURE_PROTOCOLS,
                          &protocols, sizeof(protocols))) {
        state->error = nativeError(GetLastError());
        return;
    }
    const auto host = url.host().toStdWString();
    handles.connection = WinHttpConnect(handles.session, host.c_str(),
            static_cast<INTERNET_PORT>(url.port(url.scheme() == QStringLiteral("https") ? 443 : 80)), 0);
    if (!handles.connection) {
        state->error = nativeError(GetLastError());
        return;
    }
    const auto path = url.path(QUrl::FullyEncoded).toStdWString();
    handles.request = WinHttpOpenRequest(handles.connection, L"GET", path.c_str(), nullptr,
                                         WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
            url.scheme() == QStringLiteral("https") ? WINHTTP_FLAG_SECURE : 0);
    if (!handles.request) {
        state->error = nativeError(GetLastError());
        return;
    }
    DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    if (!WinHttpSetOption(handles.request, WINHTTP_OPTION_REDIRECT_POLICY,
                          &redirectPolicy, sizeof(redirectPolicy))) {
        state->error = nativeError(GetLastError());
        return;
    }
    if (WinHttpSetStatusCallback(handles.request, statusCallback,
            WINHTTP_CALLBACK_FLAG_ALL_COMPLETIONS | WINHTTP_CALLBACK_FLAG_HANDLES, 0)
            == WINHTTP_INVALID_STATUS_CALLBACK) {
        state->error = nativeError(GetLastError());
        return;
    }
    auto *context = new CallbackContext{state};
    DWORD_PTR contextValue = reinterpret_cast<DWORD_PTR>(context);
    if (!WinHttpSetOption(handles.request, WINHTTP_OPTION_CONTEXT_VALUE,
                          &contextValue, sizeof(contextValue))) {
        delete context;
        state->error = nativeError(GetLastError());
        return;
    }
    QString headers = QStringLiteral("Accept: application/json\r\nAuthorization: Bearer ")
            + key + QStringLiteral("\r\n");
    if (providerId == QStringLiteral("mimo"))
        headers += QStringLiteral("api-key: ") + key + QStringLiteral("\r\n");
    const auto nativeHeaders = headers.toStdWString();
    if (!prepareOperation(state)) return;
    const BOOL sent = WinHttpSendRequest(handles.request, nativeHeaders.c_str(),
                                        static_cast<DWORD>(nativeHeaders.size()),
                                        WINHTTP_NO_REQUEST_DATA, 0, 0, contextValue);
    if (!waitFor(state, sent, GetLastError(), WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE)) return;
    if (!prepareOperation(state)) return;
    const BOOL received = WinHttpReceiveResponse(handles.request, nullptr);
    if (!waitFor(state, received, GetLastError(), WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE)) return;
    DWORD status = 0;
    DWORD statusSize = sizeof(status);
    if (!WinHttpQueryHeaders(handles.request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX)) {
        state->error = nativeError(GetLastError());
        return;
    }
    state->status = static_cast<int>(status);
    // Error bodies and redirects are never displayed or forwarded. Finish as
    // soon as their status is known rather than downloading untrusted content.
    if (status >= 300) return;
    while (true) {
        if (!prepareOperation(state)) return;
        const BOOL queried = WinHttpQueryDataAvailable(handles.request, nullptr);
        if (!waitFor(state, queried, GetLastError(), WINHTTP_CALLBACK_STATUS_DATA_AVAILABLE)) return;
        DWORD available = state->dataSize;
        if (!available) break;
        if (available > static_cast<DWORD>(maximumResponseSize - state->contents.size())) {
            state->error = QStringLiteral("模型列表响应过大，已停止读取。");
            return;
        }
        const DWORD requested = std::min(available, static_cast<DWORD>(state->buffer.size()));
        if (!prepareOperation(state)) return;
        const BOOL read = WinHttpReadData(handles.request, state->buffer.data(), requested, nullptr);
        if (!waitFor(state, read, GetLastError(), WINHTTP_CALLBACK_STATUS_READ_COMPLETE)) return;
        if (state->dataSize > state->buffer.size()
                || state->contents.size() + static_cast<int>(state->dataSize) > maximumResponseSize) {
            state->error = QStringLiteral("模型列表响应过大，已停止读取。");
            return;
        }
        state->contents.append(state->buffer.data(), static_cast<int>(state->dataSize));
        if (!state->dataSize) break;
    }
}

} // namespace

WinHttpModelTransport::WinHttpModelTransport(QObject *parent) : QObject(parent) {}

WinHttpModelTransport::~WinHttpModelTransport() {
    cancel();
    // Every worker wakes immediately on cancellation and owns an asynchronous
    // request. Join before deleting QThread objects; no worker uses this object.
    const auto tasks = m_tasks;
    for (auto it = tasks.constBegin(); it != tasks.constEnd(); ++it) {
        disconnect(it.key(), nullptr, this, nullptr);
        it.key()->wait();
        delete it.key();
    }
}

void WinHttpModelTransport::cancel() {
    ++m_generation;
    for (const auto &state : m_tasks) {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->cancelled = true;
        state->condition.notify_all();
    }
}

void WinHttpModelTransport::fetch(const QUrl &url, const QString &key,
                                const QString &providerId, int timeoutMs) {
    cancel();
    const quint64 generation = m_generation;
    const auto state = std::make_shared<WinHttpRequestState>();
    state->deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(qMax(1, timeoutMs));
    QThread *thread = QThread::create([state, url, key, providerId, timeoutMs] {
        performRequest(state, url, key, providerId, qMax(1, timeoutMs));
    });
    m_tasks.insert(thread, state);
    connect(thread, &QThread::finished, this, [this, thread, state, generation] {
        m_tasks.remove(thread);
        thread->deleteLater();
        if (generation != m_generation) return;
        emit replyReady(state->status, state->contents, state->error);
    });
    thread->start();
}

} // namespace lmsc
#endif
