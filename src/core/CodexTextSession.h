#pragma once

#include "AiTextTransport.h"
#include <QObject>
#include <functional>
#include <memory>

namespace lmsc {

// A generation owns an isolated app-server process. Account settings continue
// to use CodexAccountClient without creating a model session.
class CodexTextSession final : public QObject {
public:
    using Success = std::function<void(const AiTextResult &)>;
    using Failure = std::function<void(const AiFailure &)>;
    using Progress = std::function<void(const QString &, qint64)>;
    CodexTextSession(const AppPreferences &preferences, const AiTextRequest &request,
                     Success success, Failure failure, QObject *parent, Progress progress = {});
    ~CodexTextSession() override;
    void start();
    void cancel();
private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace lmsc
