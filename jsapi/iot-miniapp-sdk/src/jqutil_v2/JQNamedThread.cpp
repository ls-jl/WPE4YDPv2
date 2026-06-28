#include "jqutil_v2/JQNamedThread.h"
#include "looper/Thread.h"
#include "utils/log.h"
#include "port/jquick_mutex.h"
#include "port/jquick_condition.h"
#include <string>
#include <map>
#include <queue>

#define DEBUG_JQ_NAMED_THREAD

namespace JQUTIL_NS {

static JQuick::Mutex NamedThread_mutex;

class NamedThread;
class NamedThreadManager {
public:
    static NamedThreadManager* Instance();
    NamedThread* postOnThreadLocked(const std::string &name, JQuick::Closure c,
                                    int32_t stackSize, int32_t priority, uint32_t keepAliveMs);
    NamedThread* removeThreadLocked(const std::string &name);

protected:
    NamedThreadManager();

protected:
    friend class NamedThread;
    std::map<std::string, NamedThread*> _nameThreadMap;
};

class NamedThread: public JQuick::Thread {
public:
    NamedThread(const std::string &name, int32_t priority,
                int32_t stackSize, uint32_t keepAliveMs,
                JQuick::ReleaseThreadFunction releaseFunc):
            JQuick::Thread(name, priority, releaseFunc)
            , _stackSize(stackSize)
            , _keepAliveMs(keepAliveMs > 30000 ? 30000: keepAliveMs)
    {
    }

    virtual size_t getStackSize() const
    {
        return _stackSize;
    }

    virtual void run()
    {
        while (true) {
            JQuick::Closure c;
            {
                JQuick::Mutex::Autolock l(NamedThread_mutex);
                if (_funcQueue.size() == 0 && _keepAliveMs > 0) {
#ifdef DEBUG_JQ_NAMED_THREAD
                    LOGD("NamedThread::run %s, wait keepAliveMs %d START", _threadName.c_str(), _keepAliveMs);
#endif
                    _condition.waitRelative(NamedThread_mutex, _keepAliveMs);
#ifdef DEBUG_JQ_NAMED_THREAD
                    LOGD("NamedThread::run %s, wait keepAliveMs %d END", _threadName.c_str(), _keepAliveMs);
#endif
                }

                if (_funcQueue.size() == 0) {
                    NamedThreadManager::Instance()->removeThreadLocked(_threadName);
                    break;
                }
                c = _funcQueue.front();
                _funcQueue.pop();
            }
            c();
        }
        Thread::run();
    }

    void postClosureLocked(JQuick::Closure &c)
    {
        _funcQueue.push(c);
        _condition.signal();
    }

protected:
    int32_t _stackSize;
    uint32_t _keepAliveMs;
    std::queue<JQuick::Closure> _funcQueue;
    JQuick::Condition _condition;
};

// static
void NamedThreadRelease(JQuick::Thread* t)
{
    delete t;
}

// static
NamedThreadManager* NamedThreadManager::Instance()
{
    static NamedThreadManager instance;
    return &instance;
}

NamedThreadManager::NamedThreadManager()
{}

NamedThread* NamedThreadManager::postOnThreadLocked(
        const std::string &name, JQuick::Closure c,
        int32_t stackSize, int32_t priority, uint32_t keepAliveMs)
{
    NamedThread *thread;
    auto iter = _nameThreadMap.find(name);
    if (iter == _nameThreadMap.end()) {
        // create
        thread = new NamedThread(name, stackSize, priority, keepAliveMs, &NamedThreadRelease);
        // add thread
        _nameThreadMap[name] = thread;
        thread->start();
#ifdef DEBUG_JQ_NAMED_THREAD
        LOGD("NamedThread added %s", name.c_str());
#endif
    } else {
        thread = iter->second;
    }

    thread->postClosureLocked(c);

    return thread;
}

NamedThread* NamedThreadManager::removeThreadLocked(const std::string &name)
{
    NamedThread *thread = NULL;
    auto iter = _nameThreadMap.find(name);
    if (iter != _nameThreadMap.end()) {
        // remove
        thread = iter->second;
        _nameThreadMap.erase(iter);
#ifdef DEBUG_JQ_NAMED_THREAD
        LOGD("NamedThread removed %s", name.c_str());
#endif
    }
    return thread;

}

void PostOnNamedThread(const std::string &name, JQuick::Closure c,
                       int32_t stackSize/*=0*/, int32_t priority/*=0*/,
                       uint32_t keepAliveMs/*=10000*/)
{
    JQuick::Mutex::Autolock _lock(NamedThread_mutex);
    // first get or create Thread
    NamedThreadManager::Instance()->postOnThreadLocked(name, c, stackSize, priority, keepAliveMs);
}

static void StdFuncCaller(std::function<void()> c)
{
    c();
}

void PostOnNamedThread(const std::string &name, std::function<void()> c,
                         int32_t stackSize/*=0*/, int32_t priority/*=0*/,
                         uint32_t keepAliveMs/*=10000*/)
{
    PostOnNamedThread(name, JQuick::bind(StdFuncCaller, c), stackSize, priority, keepAliveMs);
}

}  // namespace JQUTIL_NS