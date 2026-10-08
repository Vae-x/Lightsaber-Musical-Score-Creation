#pragma once

#include <QThread>
#include <functional>
#include <utility>

namespace lmsc {

// Qt5 Android builds can disable cxx11_future and omit QThread::create. The
// worker lifetime, interruption and finished signal still use ordinary QThread.
class WorkerThread final : public QThread {
public:
    explicit WorkerThread(std::function<void()> task) : m_task(std::move(task)) {}
protected:
    void run() override { m_task(); }
private:
    std::function<void()> m_task;
};

inline QThread *createWorkerThread(std::function<void()> task) {
    return new WorkerThread(std::move(task));
}

}
