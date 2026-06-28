#pragma once
#include "jqutil_v2/JQDefs.h"
#include "threadpool/ThreadPool.h"
#include "looper/Handler.h"
#include "utils/REF.h"
#include "quickjs/quickjs.h"
#include <functional>

namespace JQUTIL_NS {

class JQAsyncSchedule;
class JQAsyncScheduleInfo {
public:
    std::string moduleName() const;
    std::string functionName() const;
    std::string appid() const;
    JQuick::sp<JQBaseObject> cppObj() const;
    intptr_t cppObjIntPtr() const;
    JQuick::sp<JQObjectTemplate> objTpl() const;
    std::string getPropName() const;
    JQTemplateEnvRef tplEnv() const;

    void setHandler(JQuick::sp<JQuick::Handler> handler);
    void setThreadPool(JQuick::ThreadPool *threadPool);
    void setThreadName(const std::string &threadName);
    void setThreadStackSize(int32_t stackSize);
    void setThreadPriority(int32_t priority);
    void setThreadKeepAliveMs(int32_t keepAliveMs);

    JQuick::sp<JQuick::Handler> handler() const;
    JQuick::ThreadPool * threadPool() const;
    std::string threadName() const;
    const JQObjectProperty& property() const;
    JQAsyncInfo& asyncInfo();

private:
    friend class JQAsyncSchedule;
    JQuick::sp<JQuick::Handler> _outHandler;
    JQuick::ThreadPool* _outThreadPool = NULL;
    std::string _outThreadName;
    int32_t _outThreadStackSize = 0;
    int32_t _outThreadPriority = 0;
    uint32_t _outThreadKeepAliveMs = 10000;

    JQuick::sp<JQObjectTemplate> _objTpl;
    JQuick::sp<JQBaseObject> _cppObj;
    JQAsyncInfo *_asyncInfo = NULL;
};

using JQAsyncScheduleHook = std::function<void(JQAsyncScheduleInfo&)>;

class JQAsyncSchedule {
public:
    enum {
        MODE_MODULE     = 1<<0,
        MODE_FUNCTION   = 1<<1,
        MODE_APP        = 1<<2,
        MODE_OBJECT     = 1<<3,
    };

    void setMode(int mode);
    void dispatch(JQuick::sp<JQObjectTemplate> objTpl, JQuick::sp<JQBaseObject> cppObj,
                  JQObjectProperty &property, JQAsyncInfo &asyncInfo);

    void setHook(JQAsyncScheduleHook hook);

private:
    int _mode = MODE_MODULE;
    JQAsyncScheduleHook _hook;
};

}  // namespace JQUTIL_NS
