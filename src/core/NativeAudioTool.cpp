#include "NativeAudioTool.h"

#ifdef Q_OS_ANDROID
#include <QAndroidJniEnvironment>
#include <QAndroidJniObject>
#include <QElapsedTimer>
#include <QThread>
#include "WorkerThread.h"
#include <cmath>

namespace {
QString takeJavaError(QAndroidJniEnvironment &environment) {
    if (!environment->ExceptionCheck()) return {};
    jthrowable exception = environment->ExceptionOccurred();
    environment->ExceptionClear();
    QAndroidJniObject object(exception);
    environment->DeleteLocalRef(exception);
    const QString message = object.callObjectMethod("toString", "()Ljava/lang/String;").toString();
    if (environment->ExceptionCheck()) environment->ExceptionClear();
    return message;
}
}
#endif

namespace lmsc {

bool NativeAudioTool::available() {
#ifdef Q_OS_ANDROID
    QAndroidJniEnvironment environment;
    const auto version = QAndroidJniObject::callStaticObjectMethod(
        "com/arthenica/ffmpegkit/FFmpegKitConfig", "getFFmpegVersion", "()Ljava/lang/String;");
    return takeJavaError(environment).isEmpty() && version.isValid() && !version.toString().isEmpty();
#else
    return false;
#endif
}

NativeAudioResult NativeAudioTool::run(bool probe, const QStringList &arguments,
    const std::shared_ptr<NativeAudioControl> &control,
    const std::function<void(double)> &progress, int timeoutMs) {
    NativeAudioResult result;
#ifdef Q_OS_ANDROID
    QAndroidJniEnvironment environment;
    if (control && control->cancelled.load()) { result.exitCode = 255; return result; }
    if (!available()) { result.error = QStringLiteral("APK 缺少可用的 Android 音频组件。"); return result; }

    // Native statistics supply progress; pipe:1 would write into the app's
    // process stdout and cannot represent a per-session output stream.
    QStringList nativeArguments;
    for (int i = 0; i < arguments.size(); ++i) {
        if (arguments.at(i) == QStringLiteral("-progress") && i + 1 < arguments.size()) { ++i; continue; }
        nativeArguments.append(arguments.at(i));
    }
    jclass stringClass = environment->FindClass("java/lang/String");
    if (!stringClass) { result.error = takeJavaError(environment); return result; }
    jobjectArray array = environment->NewObjectArray(nativeArguments.size(), stringClass, nullptr);
    environment->DeleteLocalRef(stringClass);
    if (!array) { result.error = takeJavaError(environment); return result; }
    for (int i = 0; i < nativeArguments.size(); ++i) {
        const auto argument = QAndroidJniObject::fromString(nativeArguments.at(i));
        environment->SetObjectArrayElement(array, i, argument.object<jstring>());
    }
    const char *sessionClass = probe ? "com/arthenica/ffmpegkit/FFprobeSession" : "com/arthenica/ffmpegkit/FFmpegSession";
    const char *signature = probe
        ? "([Ljava/lang/String;)Lcom/arthenica/ffmpegkit/FFprobeSession;"
        : "([Ljava/lang/String;)Lcom/arthenica/ffmpegkit/FFmpegSession;";
    // Create the session without starting Java's asynchronous executor. No
    // native work exists yet, so allocation/JNI failures here can return safely.
    const auto session = QAndroidJniObject::callStaticObjectMethod(sessionClass, "create", signature, array);
    environment->DeleteLocalRef(array);
    result.error = takeJavaError(environment);
    if (!result.error.isEmpty() || !session.isValid()) {
        if (result.error.isEmpty()) result.error = QStringLiteral("无法创建 Android 音频任务。");
        return result;
    }

    const jlong sessionId = session.callMethod<jlong>("getSessionId", "()J");
    result.error = takeJavaError(environment);
    if (sessionId <= 0 || !result.error.isEmpty()) {
        if (result.error.isEmpty()) result.error = QStringLiteral("Android 音频任务标识无效。");
        return result;
    }
    if (control && control->cancelled.load()) { result.exitCode = 255; return result; }

    std::atomic<bool> finished{false};
    std::atomic<bool> timedOut{false};
    QString monitorError;
    QElapsedTimer timer;
    timer.start();
    const auto monitor = std::unique_ptr<QThread>(createWorkerThread([&] {
        QAndroidJniEnvironment monitorEnvironment;
        double lastProgress = -1;
        while (!finished.load()) {
            if (timeoutMs > 0 && timer.elapsed() >= timeoutMs) timedOut.store(true);
            if ((control && control->cancelled.load()) || timedOut.load() || !monitorError.isEmpty()) {
                // The identifier is strictly positive: zero would cancel ALL
                // sessions. Repeat to cover a cancel/start race inside native.
                QAndroidJniObject::callStaticMethod<void>("com/arthenica/ffmpegkit/FFmpegKit",
                    "cancel", "(J)V", sessionId);
                const QString error = takeJavaError(monitorEnvironment);
                if (!error.isEmpty()) monitorError = error;
            }
            if (!probe && progress && monitorError.isEmpty()) {
                const auto statistics = session.callObjectMethod("getLastReceivedStatistics", "()Lcom/arthenica/ffmpegkit/Statistics;");
                if (statistics.isValid()) {
                    const double seconds = statistics.callMethod<jdouble>("getTime", "()D") / 1000.0;
                    if (std::isfinite(seconds) && seconds >= 0 && seconds > lastProgress) { lastProgress = seconds; progress(seconds); }
                }
                monitorError = takeJavaError(monitorEnvironment);
            }
            QThread::msleep(50);
        }
    }));
    monitor->start();
    QAndroidJniObject::callStaticMethod<void>("com/arthenica/ffmpegkit/FFmpegKitConfig",
        probe ? "ffprobeExecute" : "ffmpegExecute", probe
            ? "(Lcom/arthenica/ffmpegkit/FFprobeSession;)V"
            : "(Lcom/arthenica/ffmpegkit/FFmpegSession;)V", session.object<jobject>());
    const QString executeError = takeJavaError(environment);
    finished.store(true);
    monitor->wait();
    // Every path from this point runs after the synchronous native entry point
    // returned and the monitor joined. A cancelled/failed export cannot leave a
    // background writer using a staging directory after its owner destroys it.
    result.timedOut = timedOut.load();
    const auto code = session.callObjectMethod("getReturnCode", "()Lcom/arthenica/ffmpegkit/ReturnCode;");
    if (code.isValid()) result.exitCode = code.callMethod<jint>("getValue", "()I");
    result.output = session.callObjectMethod("getOutput", "()Ljava/lang/String;").toString().toUtf8();
    if (result.exitCode != 0) {
        result.error = session.callObjectMethod("getFailStackTrace", "()Ljava/lang/String;").toString();
        if (result.error.isEmpty()) result.error = QString::fromUtf8(result.output).trimmed().right(3000);
    }
    const QString javaError = takeJavaError(environment);
    if (!javaError.isEmpty()) { result.error = javaError; result.exitCode = -1; }
    if (!executeError.isEmpty()) { result.error = executeError; result.exitCode = -1; }
    if (!monitorError.isEmpty()) { result.error = monitorError; result.exitCode = -1; }
    if (result.timedOut) result.error = QStringLiteral("Android 音频处理超时，任务已停止。");
#else
    Q_UNUSED(probe)
    Q_UNUSED(arguments)
    Q_UNUSED(control)
    Q_UNUSED(progress)
    Q_UNUSED(timeoutMs)
    result.error = QStringLiteral("当前平台未启用 Android 音频后端。");
#endif
    return result;
}

}
