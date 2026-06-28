#include "jqutil_v2/JQAsyncSchedule.h"
#include "jqutil_v2/JQObjectTemplate.h"
#include "jqutil_v2/JQNamedThread.h"
#include "jqutil_v2/JQTemplateEnv.h"
#include <map>

namespace JQUTIL_NS {

std::string JQAsyncScheduleInfo::moduleName() const
{
    return _objTpl->moduleName();
}

std::string JQAsyncScheduleInfo::functionName() const
{
    return _objTpl->functionName();
}

std::string JQAsyncScheduleInfo::appid() const
{
    return _objTpl->appid();
}

JQuick::sp<JQObjectTemplate> JQAsyncScheduleInfo::objTpl() const
{
    return _objTpl;
}

JQuick::sp<JQBaseObject> JQAsyncScheduleInfo::cppObj() const
{
    return _cppObj;
}

intptr_t JQAsyncScheduleInfo::cppObjIntPtr() const
{
    return (intptr_t)_cppObj.get();
}

std::string JQAsyncScheduleInfo::getPropName() const
{
    JSContext* ctx = _objTpl->context();
    return _asyncInfo->property().getName(ctx);
}

JQTemplateEnvRef JQAsyncScheduleInfo::tplEnv() const
{
    return _asyncInfo->tplEnv();
}

void JQAsyncScheduleInfo::setHandler(JQuick::sp<JQuick::Handler> handler)
{
    _outHandler = handler;
}

void JQAsyncScheduleInfo::setThreadPool(JQuick::ThreadPool *threadPool)
{
    _outThreadPool = threadPool;
}

void JQAsyncScheduleInfo::setThreadName(const std::string &threadName)
{
    _outThreadName = threadName;
}

void JQAsyncScheduleInfo::setThreadStackSize(int32_t stackSize)
{
    _outThreadStackSize = stackSize;
}

void JQAsyncScheduleInfo::setThreadPriority(int32_t priority)
{
    _outThreadPriority = priority;
}

void JQAsyncScheduleInfo::setThreadKeepAliveMs(int32_t keepAliveMs)
{
    _outThreadKeepAliveMs = keepAliveMs;
}

JQuick::sp<JQuick::Handler> JQAsyncScheduleInfo::handler() const
{
    return _outHandler;
}

JQuick::ThreadPool * JQAsyncScheduleInfo::threadPool() const
{
    return _outThreadPool;
}

std::string JQAsyncScheduleInfo::threadName() const
{
    return _outThreadName;
}

const JQObjectProperty& JQAsyncScheduleInfo::property() const
{
    return _asyncInfo->property();
}

JQAsyncInfo& JQAsyncScheduleInfo::asyncInfo()
{
    return *_asyncInfo;
}

void JQAsyncSchedule::setMode(int mode)
{
    _mode = mode;
}

void JQAsyncSchedule::setHook(JQAsyncScheduleHook hook)
{
    _hook = hook;
}

// NOTE: run in module thread
static void AsyncCaller(JQAsyncCallbackType async_cb, JQAsyncInfo info)
{
    async_cb(info);
}

void JQAsyncSchedule::dispatch(JQuick::sp<JQObjectTemplate> objTpl, JQuick::sp<JQBaseObject> cppObj,
                               JQObjectProperty &property, JQAsyncInfo &asyncInfo)
{
    std::string key;
    if (_mode & MODE_MODULE) {
        key += "<m>" + objTpl->moduleName();
    }
    if (_mode & MODE_FUNCTION) {
        key += "<f>" + objTpl->functionName();
    }
    if (_mode & MODE_APP) {
        key += "<a>" + objTpl->appid();
    }
    if (_mode & MODE_OBJECT) {
        key += "<o>" + std::to_string((intptr_t)cppObj.get());
    }

    if (_hook) {
        JQAsyncScheduleInfo info;
        info._cppObj = cppObj;
        info._objTpl = objTpl;
        info._asyncInfo = &asyncInfo;
        info._outThreadName = key;
        _hook(info);
        JQuick::Closure c = JQuick::bind(AsyncCaller, property.async_cb, asyncInfo);
        if (info._outThreadPool) {
            info._outThreadPool->execute("JQAsyncSchedule", "dispatch", c);
        } else if (info._outHandler.get()) {
            info._outHandler->post(new JQuick::FunctionalTask(c));
        } else if (!info._outThreadName.empty()) {
            PostOnNamedThread(info._outThreadName, c,
                              info._outThreadStackSize, info._outThreadPriority,
                              info._outThreadKeepAliveMs);
        }
    } else {
        JQuick::Closure c = JQuick::bind(AsyncCaller, property.async_cb, asyncInfo);
        PostOnNamedThread(key, c);
    }
}

}  // namespace JQUTIL_NS