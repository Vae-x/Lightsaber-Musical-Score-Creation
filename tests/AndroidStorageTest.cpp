#include "core/AndroidStorage.h"
#include "core/AppSettings.h"
#include "core/BeatmapDocument.h"
#include "core/ProjectStore.h"
#include "core/WorkspacePaths.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>
#include <QSslSocket>

#if defined(Q_OS_ANDROID) && defined(LMSC_ANDROID_PLATFORM_TESTS)
#include <QAndroidJniEnvironment>
#include <QAndroidJniObject>
#include <QtAndroid>
#endif

using namespace lmsc;

namespace {
QByteArray readBytes(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

bool writeBytes(const QString &path, const QByteArray &bytes) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}

QByteArray digest(const QString &path) {
    return QCryptographicHash::hash(readBytes(path), QCryptographicHash::Sha256);
}

#if defined(Q_OS_ANDROID) && defined(LMSC_ANDROID_PLATFORM_TESTS)
QString importSyntheticSafTree(const QString &uri, const QString &temporaryRoot, QString *error) {
    const auto javaUri = QAndroidJniObject::fromString(uri);
    const auto javaRoot = QAndroidJniObject::fromString(temporaryRoot);
    const auto result = QAndroidJniObject::callStaticObjectMethod("org/lmsc/PlatformStorageHarness", "importTree",
        "(Landroid/content/Context;Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;",
        QtAndroid::androidContext().object(), javaUri.object<jstring>(), javaRoot.object<jstring>());
    QAndroidJniEnvironment environment;
    if (environment->ExceptionCheck() || !result.isValid()) {
        environment->ExceptionClear();
        *error = QStringLiteral("PlatformStorageHarness is missing or failed to load.");
        return {};
    }
    const auto document = QJsonDocument::fromJson(result.toString().toUtf8());
    if (!document.isObject()) {
        *error = QStringLiteral("Synthetic SAF harness returned invalid JSON.");
        return {};
    }
    *error = document.object().value(QStringLiteral("error")).toString();
    return document.object().value(QStringLiteral("value")).toString();
}
#endif

void append16(QByteArray *bytes, quint16 value) {
    bytes->append(char(value & 255));
    bytes->append(char((value >> 8) & 255));
}

void append32(QByteArray *bytes, quint32 value) {
    append16(bytes, quint16(value));
    append16(bytes, quint16(value >> 16));
}

quint32 crc32(const QByteArray &payload) {
    quint32 crc = 0xffffffffu;
    for (const char byte : payload) {
        crc ^= quint8(byte);
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320u : 0);
    }
    return crc ^ 0xffffffffu;
}

// A standards-compliant single-entry stored ZIP, entirely synthetic. Keeping
// generation in C++ exercises the packaged Java bridge without host fixtures.
QByteArray storedZip(const QString &entryName, const QByteArray &payload) {
    const auto name = entryName.toUtf8();
    const auto crc = crc32(payload);
    QByteArray bytes;
    append32(&bytes, 0x04034b50); append16(&bytes, 20);
    append16(&bytes, 0x0800); append16(&bytes, 0); // UTF-8, stored
    append16(&bytes, 0); append16(&bytes, 0);
    append32(&bytes, crc); append32(&bytes, quint32(payload.size()));
    append32(&bytes, quint32(payload.size()));
    append16(&bytes, quint16(name.size())); append16(&bytes, 0);
    bytes.append(name); bytes.append(payload);
    const quint32 centralOffset = quint32(bytes.size());
    append32(&bytes, 0x02014b50); append16(&bytes, 20); append16(&bytes, 20);
    append16(&bytes, 0x0800); append16(&bytes, 0);
    append16(&bytes, 0); append16(&bytes, 0);
    append32(&bytes, crc); append32(&bytes, quint32(payload.size()));
    append32(&bytes, quint32(payload.size()));
    append16(&bytes, quint16(name.size())); append16(&bytes, 0); append16(&bytes, 0);
    append16(&bytes, 0); append16(&bytes, 0); append32(&bytes, 0); append32(&bytes, 0);
    bytes.append(name);
    const quint32 centralSize = quint32(bytes.size()) - centralOffset;
    append32(&bytes, 0x06054b50); append16(&bytes, 0); append16(&bytes, 0);
    append16(&bytes, 1); append16(&bytes, 1);
    append32(&bytes, centralSize); append32(&bytes, centralOffset); append16(&bytes, 0);
    return bytes;
}
}

class AndroidStorageTest final : public QObject {
    Q_OBJECT
private slots:
    void bundledTlsIsAvailable() {
        QVERIFY2(QSslSocket::supportsSsl(), "Bundled Android HTTPS runtime failed to load");
        QVERIFY(QSslSocket::sslLibraryVersionString().startsWith(QStringLiteral("OpenSSL 1.1.1w")));
        QVERIFY2(!QSslSocket::systemCaCertificates().isEmpty(), "Android trusted roots were not loaded");
    }
    void initTestCase();
    void keystoreRoundTrip();
    void keystoreRejectsCorruptedCiphertext();
    void applicationSettingsProtectKeys();
    void privateWorkspaceAndCompleteProjectRoundTrip();
    void packagedZipExtractionAndTraversalRejection();
#if defined(Q_OS_ANDROID) && defined(LMSC_ANDROID_PLATFORM_TESTS)
    void realDocumentsProviderSyntheticRoundTrip();
#endif
};

void AndroidStorageTest::initTestCase() {
#ifndef Q_OS_ANDROID
    QFAIL("These tests require the packaged Android JNI bridge and Android Keystore.");
#endif
}

void AndroidStorageTest::keystoreRoundTrip() {
    const QString dummyKey = QStringLiteral("synthetic-android-key-测试-only-20261007");
    QString protectedKey, secondCiphertext, decoded;
    QVERIFY(AndroidStorage::protectKey(dummyKey, &protectedKey));
    QVERIFY(!protectedKey.isEmpty());
    QVERIFY(!protectedKey.contains(dummyKey));
    QVERIFY(!QByteArray::fromBase64(protectedKey.toLatin1()).contains(dummyKey.toUtf8()));
    QVERIFY(AndroidStorage::protectKey(dummyKey, &secondCiphertext));
    QVERIFY(secondCiphertext != protectedKey); // GCM must use fresh randomized IVs.
    QVERIFY(AndroidStorage::unprotectKey(protectedKey, &decoded));
    QCOMPARE(decoded, dummyKey);
    QString blank = QStringLiteral("old content");
    QVERIFY(AndroidStorage::protectKey({}, &blank));
    QVERIFY(blank.isEmpty());
    blank = QStringLiteral("old content");
    QVERIFY(AndroidStorage::unprotectKey({}, &blank));
    QVERIFY(blank.isEmpty());
}

void AndroidStorageTest::keystoreRejectsCorruptedCiphertext() {
    QString protectedKey;
    QVERIFY(AndroidStorage::protectKey(QStringLiteral("synthetic-corruption-test-only"), &protectedKey));
    auto corrupted = QByteArray::fromBase64(protectedKey.toLatin1());
    QVERIFY(corrupted.size() > 30);
    corrupted[corrupted.size() - 1] = char(quint8(corrupted.back()) ^ 0x20);
    QString decoded = QStringLiteral("old secret must be cleared");
    QVERIFY(!AndroidStorage::unprotectKey(QString::fromLatin1(corrupted.toBase64()), &decoded));
    QVERIFY(decoded.isEmpty());
    for (const QString &invalid : {QStringLiteral("not base64!"), QStringLiteral("YWJj"),
                                  protectedKey + QStringLiteral(" ")}) {
        decoded = QStringLiteral("old secret must be cleared");
        QVERIFY(!AndroidStorage::unprotectKey(invalid, &decoded));
        QVERIFY(decoded.isEmpty());
    }
}

void AndroidStorageTest::applicationSettingsProtectKeys() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    AppSettings settings(QDir(temporary.path()).filePath(QStringLiteral("preferences.json")));
    auto preferences = settings.load();
    const QString dummyKey = QStringLiteral("synthetic-settings-api-key-only");
    preferences.providers[QStringLiteral("deepseek")].apiKey = dummyKey;
    preferences.providers[QStringLiteral("deepseek")].model = QStringLiteral("synthetic-model");
    QString error;
    QVERIFY2(settings.save(preferences, &error), qPrintable(error));
    const auto stored = readBytes(settings.filePath());
    QVERIFY(!stored.contains(dummyKey.toUtf8()));
    QVERIFY(stored.contains("android-keystore-aes-gcm-v1"));
    const auto loaded = settings.load(&error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(loaded.providers.value(QStringLiteral("deepseek")).apiKey, dummyKey);
    QCOMPARE(loaded.providers.value(QStringLiteral("deepseek")).model, QStringLiteral("synthetic-model"));
    auto json = QJsonDocument::fromJson(stored).object();
    auto providers = json.value(QStringLiteral("providers")).toObject();
    auto provider = providers.value(QStringLiteral("deepseek")).toObject();
    provider.insert(QStringLiteral("apiKeyProtected"), QStringLiteral("YWJj"));
    providers.insert(QStringLiteral("deepseek"), provider);
    json.insert(QStringLiteral("providers"), providers);
    QVERIFY(writeBytes(settings.filePath(), QJsonDocument(json).toJson()));
    const auto corrupted = settings.load(&error);
    QVERIFY(!error.isEmpty());
    QVERIFY(corrupted.providers.value(QStringLiteral("deepseek")).apiKey.isEmpty());
    QCOMPARE(corrupted.providers.value(QStringLiteral("deepseek")).model, QStringLiteral("synthetic-model"));
}

void AndroidStorageTest::privateWorkspaceAndCompleteProjectRoundTrip() {
    const QString privateData = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    const QString projects = WorkspacePaths::projectsDirectory();
    QVERIFY(!privateData.isEmpty());
    QVERIFY(QDir::isAbsolutePath(projects));
    QCOMPARE(QFileInfo(projects).canonicalFilePath(),
             QFileInfo(QDir(privateData).filePath(QStringLiteral("projects"))).canonicalFilePath());
    // Only this newly created synthetic subdirectory is removed by QTemporaryDir.
    QTemporaryDir temporary(QDir(projects).filePath(QStringLiteral("synthetic-storage-test-XXXXXX")));
    QVERIFY(temporary.isValid());
    const auto root = QDir(temporary.path());
    const QString audio = root.filePath(QStringLiteral("synthetic.ogg"));
    const QString media = root.filePath(QStringLiteral("synthetic-source.mp4"));
    const QString cover = root.filePath(QStringLiteral("synthetic-cover.png"));
    QVERIFY(writeBytes(audio, "OggS synthetic audio snapshot, not a playback fixture"));
    QVERIFY(writeBytes(media, "synthetic source-media snapshot only"));
    QVERIFY(writeBytes(cover, QByteArray::fromBase64(
        "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+jFioAAAAASUVORK5CYII=")));
    QString error;
    BeatmapDocument document;
    QVERIFY2(document.createNew(audio, QStringLiteral("安卓合成工程"), 120, 0, cover, &error), qPrintable(error));
    QVERIFY2(document.setImportSource(media, 1, 1.5, 4.0, &error), qPrintable(error));
    BeatObject note;
    note.beat = 4; note.x = 1; note.y = 1; note.color = 1; note.direction = 1;
    QVERIFY2(document.addObject(note, &error), qPrintable(error));
    const QString project = WorkspacePaths::suggestedProjectFile(QStringLiteral("安卓合成工程"), temporary.path());
    QVERIFY(!project.isEmpty());
    QVERIFY2(document.saveProject(project, &error), qPrintable(error));
    QJsonObject manifest;
    QVERIFY2(ProjectStore::readJson(project, &manifest, &error), qPrintable(error));
    QVERIFY(manifest.value(QStringLiteral("assets")).toString().startsWith(QStringLiteral("assets-")));
    QVERIFY(manifest.value(QStringLiteral("sourceAssets")).toString().startsWith(QStringLiteral("source-")));
    BeatmapDocument reopened;
    QVERIFY2(reopened.loadProject(project, &error), qPrintable(error));
    QCOMPARE(reopened.title(), QStringLiteral("安卓合成工程"));
    QCOMPARE(reopened.objects().size(), 1);
    QCOMPARE(reopened.objects().first().beat, 4.0);
    QCOMPARE(digest(reopened.audioPath()), digest(audio));
    QCOMPARE(digest(reopened.coverPath()), digest(cover));
    QCOMPARE(digest(reopened.importSource().path), digest(media));
    QCOMPARE(reopened.importSource().streamIndex, 1);
    QCOMPARE(reopened.importSource().startSeconds, 1.5);
    QCOMPARE(reopened.importSource().endSeconds, 4.0);
    auto changed = reopened.objects().first();
    changed.beat = 8;
    QVERIFY2(reopened.updateObject(changed, &error), qPrintable(error));
    QVERIFY2(reopened.autoSave(&error), qPrintable(error));
    QVERIFY(BeatmapDocument::hasRecovery(project));
    // Copy the whole project as a unit and reopen both the formal checkpoint and
    // its recovery file. This checks assets/source/autosave relocation together.
    const QString moved = root.filePath(QStringLiteral("independent-project-copy"));
    QVERIFY2(ProjectStore::copyTree(QFileInfo(project).absolutePath(), moved, &error), qPrintable(error));
    BeatmapDocument copied, recovery;
    QVERIFY2(copied.loadProject(QDir(moved).filePath(QStringLiteral("project.lmsc")), &error), qPrintable(error));
    QCOMPARE(copied.objects().first().beat, 4.0);
    QVERIFY2(recovery.loadProject(QDir(moved).filePath(QStringLiteral("autosave.lmsc")), &error), qPrintable(error));
    QCOMPARE(recovery.objects().first().beat, 8.0);
    QCOMPARE(digest(recovery.audioPath()), digest(audio));
    QCOMPARE(digest(recovery.importSource().path), digest(media));
    QVERIFY(recovery.isModified());
}

void AndroidStorageTest::packagedZipExtractionAndTraversalRejection() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto root = QDir(temporary.path());
    const QString normal = root.filePath(QStringLiteral("synthetic-normal.zip"));
    const QByteArray payload = "synthetic ZIP content, original bytes preserved";
    QVERIFY(writeBytes(normal, storedZip(QStringLiteral("中文目录/Info.dat"), payload)));
    const QByteArray original = readBytes(normal);
    QString error;
    const QString destination = root.filePath(QStringLiteral("normal-output"));
    QVERIFY2(ProjectStore::extractZip(normal, destination, &error), qPrintable(error));
    QCOMPARE(readBytes(QDir(destination).filePath(QStringLiteral("中文目录/Info.dat"))), payload);
    QCOMPARE(readBytes(normal), original);
    // An existing output must remain byte-for-byte intact.
    QVERIFY(!ProjectStore::extractZip(normal, destination, &error));
    QVERIFY(!error.isEmpty());
    QCOMPARE(readBytes(QDir(destination).filePath(QStringLiteral("中文目录/Info.dat"))), payload);
    const QString hostile = root.filePath(QStringLiteral("synthetic-traversal.zip"));
    QVERIFY(writeBytes(hostile, storedZip(QStringLiteral("../outside.dat"), payload)));
    const QString rejectedDestination = root.filePath(QStringLiteral("rejected-output"));
    QVERIFY(!ProjectStore::extractZip(hostile, rejectedDestination, &error));
    QVERIFY(!error.isEmpty());
    QVERIFY(!QFileInfo::exists(root.filePath(QStringLiteral("outside.dat"))));
    QVERIFY(!QFileInfo::exists(rejectedDestination)); // rejected before any write
}

#if defined(Q_OS_ANDROID) && defined(LMSC_ANDROID_PLATFORM_TESTS)
void AndroidStorageTest::realDocumentsProviderSyntheticRoundTrip() {
    const QString privateData = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    const QString marker = QDir(privateData).filePath(QStringLiteral("platform-tests/enable-saf-synthetic.txt"));
    if (!QFileInfo(marker).isFile() || QFileInfo(marker).isSymLink())
        QSKIP("Real SAF verification is opt-in: create the private enable-saf-synthetic.txt marker first.");
    const QString hardware = QAndroidJniObject::getStaticObjectField(
        "android/os/Build", "HARDWARE", "Ljava/lang/String;").toString();
    QVERIFY2(hardware == QStringLiteral("ranchu") || hardware == QStringLiteral("goldfish"),
             "Synthetic SAF writes are allowed only on an Android emulator.");
    QTemporaryDir temporary(QDir(privateData).filePath(QStringLiteral("platform-tests/saf-synthetic-XXXXXX")));
    QVERIFY(temporary.isValid());
    qInfo("SAF synthetic test: choose the newly created empty Documents/LMSC-SAF-synthetic-<id> directory.");
    QString error;
    const QString tree = AndroidStorage::pickDirectory(&error);
    QVERIFY2(!tree.isEmpty(), qPrintable(error.isEmpty() ? QStringLiteral("Synthetic SAF selection was cancelled.") : error));
    const auto javaTree = QAndroidJniObject::fromString(tree);
    const auto uri = QAndroidJniObject::callStaticObjectMethod("android/net/Uri", "parse",
        "(Ljava/lang/String;)Landroid/net/Uri;", javaTree.object<jstring>());
    QCOMPARE(uri.callObjectMethod("getAuthority", "()Ljava/lang/String;").toString(),
             QStringLiteral("com.android.externalstorage.documents"));
    const QString documentId = QAndroidJniObject::callStaticObjectMethod("android/provider/DocumentsContract", "getTreeDocumentId",
        "(Landroid/net/Uri;)Ljava/lang/String;", uri.object()).toString();
    const QRegularExpression designated(QStringLiteral("^primary:Documents/LMSC-SAF-synthetic-[A-Za-z0-9_-]+$"));
    QVERIFY2(designated.match(documentId).hasMatch(), "Refusing writes outside the designated synthetic emulator directory.");
    const QString emptyCopy = importSyntheticSafTree(tree, temporary.path(), &error);
    QVERIFY2(!emptyCopy.isEmpty(), qPrintable(error));
    QVERIFY2(QDir(emptyCopy).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System).isEmpty(),
             "The selected synthetic tree must be empty before any remote creation.");
    qInfo().noquote() << "SAF synthetic tree:" << tree;

    const QDir root(temporary.path());
    const QString original = root.filePath(QStringLiteral("original-synthetic-song"));
    QVERIFY(QDir().mkpath(original));
    const QJsonObject unknown{{QStringLiteral("keep"), true},
        {QStringLiteral("sentinel"), QJsonArray{QStringLiteral("synthetic-unknown"), 17}}};
    const QJsonObject expert{{QStringLiteral("_version"), QStringLiteral("2.2.0")},
        {QStringLiteral("_notes"), QJsonArray{QJsonObject{{QStringLiteral("_time"), 4}, {QStringLiteral("_lineIndex"), 1},
            {QStringLiteral("_lineLayer"), 1}, {QStringLiteral("_type"), 0}, {QStringLiteral("_cutDirection"), 1},
            {QStringLiteral("unknownNoteSentinel"), 29}}}},
        {QStringLiteral("_obstacles"), QJsonArray{}}, {QStringLiteral("_events"), QJsonArray{}},
        {QStringLiteral("unknownMapSentinel"), unknown}};
    QJsonObject hard = expert;
    hard.insert(QStringLiteral("_notes"), QJsonArray{});
    const QJsonObject metadata{{QStringLiteral("_version"), QStringLiteral("2.0.0")},
        {QStringLiteral("_songName"), QStringLiteral("SAF 合成歌曲")}, {QStringLiteral("_beatsPerMinute"), 120},
        {QStringLiteral("_songTimeOffset"), 0}, {QStringLiteral("_songFilename"), QStringLiteral("song.ogg")},
        {QStringLiteral("_coverImageFilename"), QString()}, {QStringLiteral("unknownInfoSentinel"), unknown},
        {QStringLiteral("_difficultyBeatmapSets"), QJsonArray{QJsonObject{
            {QStringLiteral("_beatmapCharacteristicName"), QStringLiteral("Standard")},
            {QStringLiteral("_difficultyBeatmaps"), QJsonArray{
                QJsonObject{{QStringLiteral("_difficulty"), QStringLiteral("Expert")}, {QStringLiteral("_difficultyRank"), 7},
                    {QStringLiteral("_beatmapFilename"), QStringLiteral("Expert.dat")}},
                QJsonObject{{QStringLiteral("_difficulty"), QStringLiteral("Hard")}, {QStringLiteral("_difficultyRank"), 5},
                    {QStringLiteral("_beatmapFilename"), QStringLiteral("Hard.dat")}}}}}}}};
    QVERIFY2(ProjectStore::writeJson(QDir(original).filePath(QStringLiteral("Info.dat")), metadata, &error), qPrintable(error));
    QVERIFY2(ProjectStore::writeJson(QDir(original).filePath(QStringLiteral("Expert.dat")), expert, &error), qPrintable(error));
    QVERIFY2(ProjectStore::writeJson(QDir(original).filePath(QStringLiteral("Hard.dat")), hard, &error), qPrintable(error));
    QVERIFY(writeBytes(QDir(original).filePath(QStringLiteral("song.ogg")), "OggS synthetic preservation fixture only"));
    const auto originalFiles = ProjectStore::files(original, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QMap<QString, QByteArray> originalHashes;
    for (const auto &relative : originalFiles) originalHashes.insert(relative, digest(QDir(original).filePath(relative)));
    BeatmapDocument importedSong;
    QVERIFY2(importedSong.loadSong(original, &error), qPrintable(error));
    QString expertId;
    for (const auto &difficulty : importedSong.difficulties())
        if (difficulty.name == QStringLiteral("Expert")) expertId = difficulty.id;
    QVERIFY(!expertId.isEmpty());
    QVERIFY2(importedSong.setDifficulty(expertId, &error), qPrintable(error));
    QVERIFY(!importedSong.objects().isEmpty());
    auto edited = importedSong.objects().first();
    edited.direction = 6;
    QVERIFY2(importedSong.updateObject(edited, &error), qPrintable(error));
    const QString songName = QStringLiteral("SAF合成歌曲-by光剑曲谱");
    const QString stagedSong = root.filePath(songName);
    QVERIFY2(importedSong.exportSong(stagedSong, &error), qPrintable(error));
    const QString firstUri = AndroidStorage::copyDirectoryToTree(stagedSong, tree, &error);
    QVERIFY2(!firstUri.isEmpty(), qPrintable(error));
    const QString secondUri = AndroidStorage::copyDirectoryToTree(stagedSong, tree, &error);
    QVERIFY2(!secondUri.isEmpty(), qPrintable(error));
    QVERIFY(firstUri != secondUri);

    // This separate new-song project has original source media and a later
    // autosave checkpoint; all are exported as one directory through real SAF.
    const QString source = root.filePath(QStringLiteral("synthetic-source.mp4"));
    QVERIFY(writeBytes(source, "synthetic original source-media bytes"));
    BeatmapDocument projectDocument;
    QVERIFY2(projectDocument.createNew(QDir(original).filePath(QStringLiteral("song.ogg")),
        QStringLiteral("SAF合成工程"), 120, 0, {}, &error), qPrintable(error));
    QVERIFY2(projectDocument.setImportSource(source, 1, 1.5, 4.0, &error), qPrintable(error));
    BeatObject projectNote;
    projectNote.beat = 4; projectNote.x = 1; projectNote.y = 1; projectNote.direction = 1;
    QVERIFY2(projectDocument.addObject(projectNote, &error), qPrintable(error));
    const QString projectPath = WorkspacePaths::suggestedProjectFile(QStringLiteral("SAF合成工程"), temporary.path());
    QVERIFY2(projectDocument.saveProject(projectPath, &error), qPrintable(error));
    auto recoveredNote = projectDocument.objects().first(); recoveredNote.beat = 8;
    QVERIFY2(projectDocument.updateObject(recoveredNote, &error), qPrintable(error));
    QVERIFY2(projectDocument.autoSave(&error), qPrintable(error));
    const QString projectFolder = QFileInfo(projectPath).absolutePath();
    const QString projectUri = AndroidStorage::copyProjectDirectoryToTree(projectFolder, tree, &error);
    QVERIFY2(!projectUri.isEmpty(), qPrintable(error));
    const QString snapshot = importSyntheticSafTree(tree, temporary.path(), &error);
    QVERIFY2(!snapshot.isEmpty(), qPrintable(error));
    const QString songCategory = QDir(snapshot).filePath(QStringLiteral("光剑曲谱制作"));
    const QString importedFirst = QDir(songCategory).filePath(songName);
    const QString importedSecond = QDir(songCategory).filePath(songName + QStringLiteral("-2"));
    const QString importedProject = QDir(QDir(snapshot).filePath(QStringLiteral("光剑曲谱制作工程")))
        .filePath(QFileInfo(projectFolder).fileName());
    for (const auto &pair : QVector<QPair<QString, QString>>{{stagedSong, importedFirst},
            {stagedSong, importedSecond}, {projectFolder, importedProject}}) {
        const auto expectedFiles = ProjectStore::files(pair.first, &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(ProjectStore::files(pair.second, &error), expectedFiles);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        for (const auto &relative : expectedFiles)
            QCOMPARE(digest(QDir(pair.second).filePath(relative)), digest(QDir(pair.first).filePath(relative)));
    }
    QJsonObject copiedInfo, copiedExpert;
    QVERIFY2(ProjectStore::readJson(QDir(importedFirst).filePath(QStringLiteral("Info.dat")), &copiedInfo, &error), qPrintable(error));
    QVERIFY2(ProjectStore::readJson(QDir(importedFirst).filePath(QStringLiteral("Expert.dat")), &copiedExpert, &error), qPrintable(error));
    QCOMPARE(copiedInfo.value(QStringLiteral("unknownInfoSentinel")), QJsonValue(unknown));
    QCOMPARE(copiedExpert.value(QStringLiteral("unknownMapSentinel")), QJsonValue(unknown));
    QCOMPARE(copiedExpert.value(QStringLiteral("_notes")).toArray().first().toObject().value(QStringLiteral("unknownNoteSentinel")).toInt(), 29);
    QCOMPARE(digest(QDir(importedFirst).filePath(QStringLiteral("Hard.dat"))), originalHashes.value(QStringLiteral("Hard.dat")));
    QCOMPARE(digest(QDir(importedFirst).filePath(QStringLiteral("song.ogg"))), originalHashes.value(QStringLiteral("song.ogg")));
    BeatmapDocument formal, recovery;
    QVERIFY2(formal.loadProject(QDir(importedProject).filePath(QStringLiteral("project.lmsc")), &error), qPrintable(error));
    QCOMPARE(formal.objects().first().beat, 4.0);
    QVERIFY(BeatmapDocument::hasRecovery(formal.projectPath()));
    QVERIFY2(recovery.loadProject(QDir(importedProject).filePath(QStringLiteral("autosave.lmsc")), &error), qPrintable(error));
    QCOMPARE(recovery.objects().first().beat, 8.0);
    QCOMPARE(digest(recovery.importSource().path), digest(source));
    QCOMPARE(recovery.importSource().streamIndex, 1);
    for (const auto &relative : originalFiles)
        QCOMPARE(digest(QDir(original).filePath(relative)), originalHashes.value(relative));
    qInfo().noquote() << "SAF real DocumentsProvider passed; remote synthetic directories retained:" << firstUri << secondUri << projectUri;
}
#endif

int runAndroidStorageTests(const QString &reportPath) {
    AndroidStorageTest tests;
    return QTest::qExec(&tests, QStringList{QStringLiteral("AndroidStorageTest"),
        QStringLiteral("-o"), reportPath + QStringLiteral(",txt")});
}

#include "AndroidStorageTest.moc"
