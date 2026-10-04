#include "RefinementTypes.h"
#include <QCryptographicHash>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <algorithm>
#include <cmath>

namespace lmsc {
namespace {
QJsonObject objectJson(const BeatObject &object) {
    return {{"id", object.id}, {"kind", int(object.kind)}, {"beat", object.beat}, {"x", object.x},
        {"y", object.y}, {"color", object.color}, {"direction", object.direction},
        {"duration", object.duration}, {"width", object.width}, {"height", object.height},
        {"custom", object.preservedCustomData}, {"protected", object.protectedReason}};
}
}
QString refinementBaselineHash(const QVector<BeatObject> &objects) {
    auto ordered = objects;
    std::stable_sort(ordered.begin(), ordered.end(), [](const BeatObject &a, const BeatObject &b) { return a.id < b.id; });
    QJsonArray rows;
    for (const auto &object : ordered) rows.append(objectJson(object));
    return QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(rows).toJson(QJsonDocument::Compact),
        QCryptographicHash::Sha256).toHex());
}
RefinementPatch refinementDifference(const QVector<BeatObject> &before, const QVector<BeatObject> &after) {
    RefinementPatch patch;
    QHash<QString, BeatObject> prior, current;
    for (const auto &object : before) prior.insert(object.id, object);
    for (const auto &object : after) {
        current.insert(object.id, object);
        if (!prior.contains(object.id)) patch.additions.append(object);
        else if (objectJson(prior.value(object.id)) != objectJson(object)) patch.updates.append(object);
    }
    for (const auto &object : before) if (!current.contains(object.id)) patch.removals.append(object.id);
    return patch;
}
RefinementStats refinementStatistics(const RefinementPatch &patch, const QVector<BeatObject> &before) {
    RefinementStats result;
    result.changed = patch.updates.size(); result.added = patch.additions.size(); result.removed = patch.removals.size();
    QHash<QString, double> times;
    for (const auto &object : before) times.insert(object.id, object.beat);
    for (const auto &object : patch.updates)
        if (times.contains(object.id) && std::abs(times.value(object.id) - object.beat) > 1e-7) ++result.moved;
    return result;
}
}
