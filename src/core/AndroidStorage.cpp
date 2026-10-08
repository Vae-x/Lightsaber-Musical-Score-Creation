#include "AndroidStorage.h"
#include "ProjectStore.h"

#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QThread>
#include <functional>
#include <memory>

#ifdef Q_OS_ANDROID
#include <QtAndroid>
#include <QAndroidJniEnvironment>
#include <QAndroidJniObject>
#include <QFutureWatcher>
#include <QtConcurrent>
#endif

namespace lmsc {
namespace {
#ifdef Q_OS_ANDROID
constexpr const char *bridgeClass = "org/lmsc/StorageBridge";

bool clearJniException() {
    QAndroidJniEnvironment environment;
    if (!environment->ExceptionCheck()) return false;
    environment->ExceptionClear();
    return true;
}

QString decodeResult(const QAndroidJniObject &result, QString *error) {
    if (error) error->clear();
    if (clearJniException() || !result.isValid()) {
        if (error) *error = QStringLiteral("无法调用 Android 文件服务。");
        return {};
    }
    const auto document = QJsonDocument::fromJson(result.toString().toUtf8());
    if (!document.isObject()) {
        if (error) *error = QStringLiteral("Android 文件服务返回了无效结果。");
        return {};
    }
    const auto json = document.object();
    if (error) *error = json.value(QStringLiteral("error")).toString();
    return json.value(QStringLiteral("value")).toString();
}

// File providers can fetch from the cloud. Keep the Qt event loop responsive
// while copying without changing the caller's synchronous dialog flow.
QString storageOperation(const std::function<QAndroidJniObject()> &operation, QString *error) {
    if (!QCoreApplication::instance()
        || QThread::currentThread() != QCoreApplication::instance()->thread())
        return decodeResult(operation(), error);
    QFutureWatcher<QString> watcher;
    QEventLoop loop;
    QObject::connect(&watcher, &QFutureWatcher<QString>::finished, &loop, &QEventLoop::quit);
    watcher.setFuture(QtConcurrent::run([operation] {
        const auto result = operation();
        if (clearJniException() || !result.isValid()) return QString();
        return result.toString();
    }));
    if (!watcher.isFinished()) loop.exec();
    return decodeResult(QAndroidJniObject::fromString(watcher.result()), error);
}

QString pickUri(bool directory, const QStringList &mimeTypes, QString *error) {
    if (error) error->clear();
    if (!QCoreApplication::instance()
        || QThread::currentThread() != QCoreApplication::instance()->thread()) {
        if (error) *error = QStringLiteral("请从应用界面选择 Android 文件。");
        return {};
    }
    static bool selecting = false;
    if (selecting) {
        if (error) *error = QStringLiteral("文件选择器已打开，请先完成当前选择。");
        return {};
    }
    const auto types = QAndroidJniObject::fromString(
        QString::fromUtf8(QJsonDocument(QJsonArray::fromStringList(mimeTypes)).toJson(QJsonDocument::Compact)));
    const auto intent = QAndroidJniObject::callStaticObjectMethod(
        bridgeClass, "pickerIntent", "(Landroid/content/Context;ZLjava/lang/String;)Landroid/content/Intent;",
        QtAndroid::androidContext().object(), jboolean(directory), types.object<jstring>());
    if (clearJniException() || !intent.isValid()) {
        if (error) *error = QStringLiteral("无法打开系统文件选择器，请检查设备是否提供文件选择应用。");
        return {};
    }
    selecting = true;
    QEventLoop loop;
    struct Selection {
        QPointer<QEventLoop> loop;
        QString uri;
        bool finished = false;
    };
    const auto selection = std::make_shared<Selection>();
    selection->loop = &loop;
    QtAndroid::startActivity(intent, 9417, [selection](int, int resultCode, const QAndroidJniObject &data) {
        QString selectedUri;
        if (resultCode == -1 && data.isValid()) {
            const auto selected = data.callObjectMethod("getData", "()Landroid/net/Uri;");
            if (selected.isValid()) selectedUri = selected.callObjectMethod("toString", "()Ljava/lang/String;").toString();
        }
        clearJniException();
        if (!selection->loop) return;
        QMetaObject::invokeMethod(selection->loop.data(), [selection, selectedUri] {
            if (!selection->loop) return;
            selection->uri = selectedUri;
            selection->finished = true;
            selection->loop->quit();
        }, Qt::QueuedConnection);
    });
    if (clearJniException()) {
        selecting = false;
        if (error) *error = QStringLiteral("设备没有可用的系统文件选择器。");
        return {};
    }
    if (!selection->finished) loop.exec();
    selecting = false;
    const QString uri = selection->uri;
    if (!uri.isEmpty() && !uri.startsWith(QStringLiteral("content://"))) {
        if (error) *error = QStringLiteral("系统选择器返回的文件地址无效。");
        return {};
    }
    return uri;
}

QString importUri(const QString &uri, bool directory, QString *error) {
    if (uri.isEmpty()) return {};
    const auto javaUri = QAndroidJniObject::fromString(uri);
    return storageOperation([javaUri, directory] { return QAndroidJniObject::callStaticObjectMethod(
        bridgeClass, "importUri", "(Landroid/content/Context;Ljava/lang/String;Z)Ljava/lang/String;",
        QtAndroid::androidContext().object(), javaUri.object<jstring>(), jboolean(directory)); }, error);
}

QString exportDirectory(const QString &sourceFolder, const QString &treeUri, const QString &category, QString *error) {
    QString validationError;
    const auto files = ProjectStore::files(sourceFolder, &validationError);
    if (!validationError.isEmpty() || files.isEmpty() || !treeUri.startsWith(QStringLiteral("content://"))) {
        if (error) *error = validationError.isEmpty() ? QStringLiteral("导出目录或所选位置无效。") : validationError;
        return {};
    }
    const auto source = QAndroidJniObject::fromString(QFileInfo(sourceFolder).canonicalFilePath());
    const auto uri = QAndroidJniObject::fromString(treeUri);
    const auto javaCategory = QAndroidJniObject::fromString(category);
    return storageOperation([source, uri, javaCategory] { return QAndroidJniObject::callStaticObjectMethod(
        bridgeClass, "exportDirectory", "(Landroid/content/Context;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;",
        QtAndroid::androidContext().object(), source.object<jstring>(), uri.object<jstring>(), javaCategory.object<jstring>()); }, error);
}
#else
QString unavailable(QString *error) {
    if (error) *error = QStringLiteral("Android 文件服务仅在安卓应用中可用。");
    return {};
}
#endif
}

QString AndroidStorage::pickInputFile(const QStringList &mimeTypes, QString *error) {
#ifdef Q_OS_ANDROID
    return importUri(pickUri(false, mimeTypes, error), false, error);
#else
    Q_UNUSED(mimeTypes)
    return unavailable(error);
#endif
}

QString AndroidStorage::pickDirectory(QString *error) {
#ifdef Q_OS_ANDROID
    return pickUri(true, {}, error);
#else
    return unavailable(error);
#endif
}

QString AndroidStorage::pickSongDirectory(QString *error) {
#ifdef Q_OS_ANDROID
    return importUri(pickUri(true, {}, error), true, error);
#else
    return unavailable(error);
#endif
}

QString AndroidStorage::copyDirectoryToTree(const QString &sourceFolder, const QString &treeUri, QString *error) {
#ifdef Q_OS_ANDROID
    return exportDirectory(sourceFolder, treeUri, QStringLiteral("光剑曲谱制作"), error);
#else
    Q_UNUSED(sourceFolder)
    Q_UNUSED(treeUri)
    return unavailable(error);
#endif
}

QString AndroidStorage::copyProjectDirectoryToTree(const QString &sourceFolder, const QString &treeUri, QString *error) {
#ifdef Q_OS_ANDROID
    return exportDirectory(sourceFolder, treeUri, QStringLiteral("光剑曲谱制作工程"), error);
#else
    Q_UNUSED(sourceFolder)
    Q_UNUSED(treeUri)
    return unavailable(error);
#endif
}

bool AndroidStorage::extractZip(const QString &archive, const QString &destination, QString *error) {
#ifdef Q_OS_ANDROID
    const auto zip = QAndroidJniObject::fromString(QFileInfo(archive).absoluteFilePath());
    const auto target = QAndroidJniObject::fromString(QFileInfo(destination).absoluteFilePath());
    return storageOperation([zip, target] { return QAndroidJniObject::callStaticObjectMethod(
        bridgeClass, "extractZip", "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;",
        zip.object<jstring>(), target.object<jstring>()); }, error) == QStringLiteral("ok");
#else
    Q_UNUSED(archive)
    Q_UNUSED(destination)
    unavailable(error);
    return false;
#endif
}

bool AndroidStorage::protectKey(const QString &key, QString *protectedKey) {
    protectedKey->clear();
    if (key.isEmpty()) return true;
#ifdef Q_OS_ANDROID
    const auto value = QAndroidJniObject::fromString(key);
    QString error;
    *protectedKey = decodeResult(QAndroidJniObject::callStaticObjectMethod(
        bridgeClass, "protectKey", "(Ljava/lang/String;)Ljava/lang/String;", value.object<jstring>()), &error);
    return error.isEmpty() && !protectedKey->isEmpty();
#else
    return false;
#endif
}

bool AndroidStorage::unprotectKey(const QString &protectedKey, QString *key) {
    key->clear();
    if (protectedKey.isEmpty()) return true;
#ifdef Q_OS_ANDROID
    const auto value = QAndroidJniObject::fromString(protectedKey);
    QString error;
    *key = decodeResult(QAndroidJniObject::callStaticObjectMethod(
        bridgeClass, "unprotectKey", "(Ljava/lang/String;)Ljava/lang/String;", value.object<jstring>()), &error);
    return error.isEmpty() && !key->isEmpty();
#else
    return false;
#endif
}
} // namespace lmsc
