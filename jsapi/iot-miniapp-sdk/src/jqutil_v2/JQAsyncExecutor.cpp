#include "jqutil_v2/JQAsyncExecutor.h"
#include "jqutil_v2/jqmisc.h"
#include "JQuickContext.h"
#include "utils/log.h"
#include <assert.h>

#define JQ_ASYNC_MARK_AND_RELEASE_DIRECTLY

namespace JQUTIL_NS {

//static
uint32_t JQAsyncExecutor::_tokenId = 0;

static void _addCallback(JSContext* ctx, uint32_t token, int type, JSValue val);
static void _removeCallback(JSContext* ctx, uint32_t token, int type);

static void _CallDescVec(JSContext *ctx, std::vector<JQCallbackDesc> descVec, const JQErrorDesc * errDesc, int argc, JSValueConst *argv, bool callCustomParams=true);
#ifndef JQ_ASYNC_MARK_AND_RELEASE_DIRECTLY
static void _OnGCCollectNextStep(JSContext *ctx, std::map<uint32_t/*callbackid*/, std::vector<JQCallbackDesc>/*callback list*/> callbackMap);
#endif

JQAsyncExecutor::JQAsyncExecutor(JSContext *ctx, JQuick::sp<JQuick::Handler> jsHandler/*=NULL*/)
{
    _ctxHolder = getOrCreateCtxHolder(ctx);
    // NOTE: create JQAsyncExecutor should in JSThread
    assert(jquick_thread_get_current() == jsHandler->getLooper()->getThreadId());
    if (jsHandler.get()) {
        _jsHandler = jsHandler;
    } else {
        _jsHandler = new JQuick::Handler(JQuick::getJSLooper(JS_GetRuntime(ctx)));
    }

#ifndef JQ_ASYNC_MARK_AND_RELEASE_DIRECTLY
    // NOTE: not used yet
    // should hold context to call _OnGCCollectNextStep on JSThread later
    _ctx = JS_DupContext(object->getContext());
    assert(_ctx != NULL);
#endif
}

#ifndef JQ_ASYNC_MARK_AND_RELEASE_DIRECTLY
// static
void _OnGCCollectNextStep(JSContext *ctx, std::map<uint32_t/*callbackid*/, std::vector<JQCallbackDesc>/*callback list*/> callbackMap)
{
    if (!ctx) {
        return;  // already released
    }
    for (auto &iter0: callbackMap) {
        JQErrorDesc errDesc;
        errDesc.name = "GCDestroyedError";
        errDesc.message = "destroyed by GC";
        // last arg is false, bacause we do not known how to call error type of JQCallbackType_CustomParams
        _CallDescVec(ctx, iter0.second, &errDesc, 0, NULL, false);
        for (auto &iter1: iter0.second) {
            JS_FreeValue(ctx, iter1.func);
        }
    }
    JS_FreeContext(ctx);
}
#endif

JQAsyncExecutor::~JQAsyncExecutor()
{}

// function callback, return callbackid
uint32_t JQAsyncExecutor::addCallback(JSValueConst callback, JQCallbackType type/*=JQCallbackType_Std*/)
{
    JQuick::sp<CtxHolder> ctxHolder = _ctxHolder.promote();
    if (ctxHolder == NULL) {
        LOGD("JQAsyncExecutor::addCallback not found ctxHolder, this is unexpected");
        return 0;
    }
    JSContext *ctx = ctxHolder->ctx;

    uint32_t callbackid = _genId();
    _callbackMap[callbackid] = std::vector<JQCallbackDesc>();
    _callbackMap[callbackid].push_back({.type=type, .func=callback});
    _addCallback(ctx, callbackid, type, JS_DupValue(ctx, callback));
    return callbackid;
}

void JQAsyncExecutor::removeCallback(uint32_t callbackid)
{
    JQuick::sp<CtxHolder> ctxHolder = _ctxHolder.promote();
    if (ctxHolder == NULL) {
        LOGD("JQAsyncExecutor::removeCallback not found ctxHolder, this is unexpected, may cause memleak on async callback");
        return;
    }
    JSContext *ctx = ctxHolder->ctx;

    // find callback
    auto iter = _callbackMap.find(callbackid);
    if (iter == _callbackMap.end()) {
        LOGD("JQAsyncExecutor::removeCallback not found callbackid %d, this is unexpected, may cause memleak on async callback", callbackid);
        return;
    }
    _freeDescVec(ctx, iter->first, iter->second);
    _callbackMap.erase(iter);
}

uint32_t JQAsyncExecutor::createPromiseId(JSContext *ctx, JSValue *outPromiseOrException, const std::string &tip)
{
    LOGD("JQAsyncExecutor::createPromiseId tip %s", tip.c_str());

    JSValue resolving_funcs[2];
    *outPromiseOrException = JS_NewPromiseCapability(ctx, resolving_funcs);
    if (JS_IsException(*outPromiseOrException)) {
        LOGE("AsyncExecutor create promise failed");
        return 0;
    }

    JSValue tipVal = JS_NewStringLen(ctx, tip.c_str(), tip.size());
    JS_SetPropertyStr(ctx, resolving_funcs[0], "tip", JS_DupValue(ctx, tipVal));
    JS_SetPropertyStr(ctx, resolving_funcs[1], "tip", JS_DupValue(ctx, tipVal));
    JS_SetPropertyStr(ctx, *outPromiseOrException, "tip", JS_DupValue(ctx, tipVal));
    JS_FreeValue(ctx, tipVal);

    uint32_t promiseCbId = _addResolvingFuncs(ctx, resolving_funcs[0], resolving_funcs[1]);
    if (promiseCbId == 0) {
        LOGE("AsyncExecutor set reject resolve callback error");
        return 0;
    }
    return promiseCbId;
}

// run js thread, with json res
void JQAsyncExecutor::onCallbackJSON(uint32_t callbackid, const std::string &json, const JQErrorDesc * errDesc/*=nullptr*/, bool autoDel/*=true*/)
{
    JQuick::sp<CtxHolder> ctxHolder = _ctxHolder.promote();
    if (ctxHolder == NULL) {
        LOGD("JQAsyncExecutor::onCallbackJSON not found ctxHolder, this is unexpected, may cause memleak on async callback");
        return;
    }
    JSContext *ctx = ctxHolder->ctx;

    JSValue res = JS_UNDEFINED;
    // find callback
    auto iter = _callbackMap.find(callbackid);
    if (iter == _callbackMap.end()) {
        LOGD("JQAsyncExecutor::onCallbackJSON not found callbackid %d, this is unexpected, may cause memleak on async callback", callbackid);
        return;
    }
    // json.parse(res), then call callback
    if (!json.empty()) {
        res = JS_ParseJSON(ctx, json.c_str(), json.size(), "JQAsyncExecutor::onCallbackJSON");
        if (JS_IsException(res)) {
            jq_dump_error(ctx);
        }
    }

    _CallDescVec(ctx, iter->second, errDesc, 1, &res);

    // exit:
    JS_FreeValue(ctx, res);
    // clean callback
    if (autoDel) {
        auto iter = _callbackMap.find(callbackid);
        if (iter != _callbackMap.end()) {
            _freeDescVec(ctx, iter->first, iter->second);
            _callbackMap.erase(iter);
        }
    }
}

void JQAsyncExecutor::onCallback(uint32_t callbackid, const Bson &bson, const JQErrorDesc * errDesc/*=nullptr*/, bool autoDel/*=true*/)
{
    JQuick::sp<CtxHolder> ctxHolder = _ctxHolder.promote();
    if (ctxHolder == NULL) {
        LOGD("JQAsyncExecutor::onCallback not found ctxHolder, this is unexpected, may cause memleak on async callback");
        return;
    }
    JSContext *ctx = ctxHolder->ctx;

    JSValue res = JS_UNDEFINED;
    // find callback
    auto iter = _callbackMap.find(callbackid);
    if (iter == _callbackMap.end()) {
        LOGD("JQAsyncExecutor::onCallback not found callbackid %d, this is unexpected, may cause memleak on async callback", callbackid);
        return;
    }
    // call callback
    res = bsonToJSValue(ctx, bson);

    _CallDescVec(ctx, iter->second, errDesc, 1, &res);

    // exit:
    JS_FreeValue(ctx, res);
    // clean callback
    if (autoDel) {
        auto iter = _callbackMap.find(callbackid);
        if (iter != _callbackMap.end()) {
            _freeDescVec(ctx, iter->first, iter->second);
            _callbackMap.erase(iter);
        }
    }
}

// static
void _CallDescVec(JSContext *ctx, std::vector<JQCallbackDesc> descVec, const JQErrorDesc * errDesc, int argc, JSValueConst *argv, bool callCustomParams/*=true*/)
{
    JS_DupContext(ctx);

    for (auto &desc: descVec) {
        JS_DupValue(ctx, desc.func);
    }

    for (auto &desc: descVec) {
        JSValue ret = JS_UNDEFINED;
        if (desc.type == JQCallbackType_Resolve || desc.type == JQCallbackType_Reject) {
            // resolve(res) OR reject(error)
            if (errDesc == nullptr) {
                // resolve
                if (desc.type == JQCallbackType_Resolve) {
                    ret = JS_Call(ctx, desc.func, JS_UNDEFINED, argc > 1 ? 1: argc, argv);
                }
            } else {
                // reject
                if (desc.type == JQCallbackType_Reject) {
                    JSValue error = errDesc->createJSValue(ctx);
                    if (argv && !JS_IsUndefined(argv[0])) {
                        JS_SetPropertyStr(ctx, error, "res", JS_DupValue(ctx, argv[0]));
                    }
                    ret = JS_Call(ctx, desc.func, JS_UNDEFINED, 1, &error);
                    JS_FreeValue(ctx, error);
                }
            }
        } else if (desc.type == JQCallbackType_Std) {
            // callback(error, res)
            JSValueConst argv_local[2];
            if (errDesc == nullptr) {
                argv_local[0] = JS_NULL;
                argv_local[1] = argv ? argv[0]: JS_UNDEFINED;
                ret = JS_Call(ctx, desc.func, JS_UNDEFINED, 2, argv_local);
            } else {
                JSValue error = errDesc->createJSValue(ctx);

                argv_local[0] = error;
                argv_local[1] = argv ? argv[0]: JS_UNDEFINED;
                ret = JS_Call(ctx, desc.func, JS_UNDEFINED, 2, argv_local);

                JS_FreeValue(ctx, error);
            }
        } else if (desc.type == JQCallbackType_Simple) {
            // callback(res)
            JSValueConst argv_local[1];
            if (errDesc == nullptr) {
                argv_local[0] = argv ? argv[0]: JS_UNDEFINED;
                ret = JS_Call(ctx, desc.func, JS_UNDEFINED, 1, argv_local);
            } else {
                JSValue error = errDesc->createJSValue(ctx);

                argv_local[0] = error;
                ret = JS_Call(ctx, desc.func, JS_UNDEFINED, 1, argv_local);

                JS_FreeValue(ctx, error);
            }
        } else if (desc.type == JQCallbackType_CustomParams) {
            if (callCustomParams) {
                ret = JS_Call(ctx, desc.func, JS_UNDEFINED, argc, argv);
            }
        }

        if (JS_IsException(ret)) {
            jq_dump_error(ctx);
        }
        JS_FreeValue(ctx, ret);
    }

    for (auto &desc: descVec) {
        JS_FreeValue(ctx, desc.func);
    }

    JS_FreeContext(ctx);
}

void JQAsyncExecutor::_freeDescVec(JSContext *ctx, uint32_t callbackid, std::vector<JQCallbackDesc> &descVec)
{
    for (auto &desc: descVec) {
        _removeCallback(ctx, callbackid, desc.type);
    }
}

void JQAsyncExecutor::onCallbackJSValue(uint32_t callbackid, int argc, JSValueConst *argv, const JQErrorDesc * errDesc/*=nullptr*/, bool autoDel/*=true*/)
{
    JQuick::sp<CtxHolder> ctxHolder = _ctxHolder.promote();
    if (ctxHolder == NULL) {
        LOGD("JQAsyncExecutor::onCallbackJSValue not found ctxHolder, this is unexpected, may cause memleak on async callback");
        return;
    }
    JSContext *ctx = ctxHolder->ctx;

    // find callback
    auto iter = _callbackMap.find(callbackid);
    if (iter == _callbackMap.end()) {
        LOGD("JQAsyncExecutor::onCallbackJSValue not found callbackid %d, this is unexpected, may cause memleak on async callback", callbackid);
        return;
    }

    _CallDescVec(ctx, iter->second, errDesc, argc, argv);

    // exit:
    // clean callback
    if (autoDel) {
        auto iter = _callbackMap.find(callbackid);
        if (iter != _callbackMap.end()) {
            _freeDescVec(ctx, iter->first, iter->second);
            _callbackMap.erase(iter);
        }
    }
}

void JQAsyncExecutor::onError(int callbackid, const std::string &message, int code/*=0*/, const std::string &name/*=""*/, bool autoDel/*=true*/)
{
    JQuick::sp<CtxHolder> ctxHolder = _ctxHolder.promote();
    if (ctxHolder == NULL) {
        LOGD("JQAsyncExecutor::onError not found ctxHolder, this is unexpected, may cause memleak on async callback");
        return;
    }
    JSContext *ctx = ctxHolder->ctx;

    // find callback
    auto iter = _callbackMap.find(callbackid);
    if (iter == _callbackMap.end()) {
        LOGD("JQAsyncExecutor::onError not found callbackid %d, this is unexpected, may cause memleak on async callback", callbackid);
        return;
    }

    JQErrorDesc errDesc;
    errDesc.name = name;
    errDesc.message = message;
    errDesc.code = code;
    _CallDescVec(ctx, iter->second, &errDesc, 0, NULL);

    // exit:
    // clean callback
    if (autoDel) {
        auto iter = _callbackMap.find(callbackid);
        if (iter != _callbackMap.end()) {
            _freeDescVec(ctx, iter->first, iter->second);
            _callbackMap.erase(iter);
        }
    }
}

uint32_t JQAsyncExecutor::_addResolvingFuncs(JSContext *ctx, JSValue resolve, JSValue reject)
{
    uint32_t callbackid = _genId();
    _callbackMap[callbackid].push_back({.type=JQCallbackType_Resolve, .func=resolve});
    _callbackMap[callbackid].push_back({.type=JQCallbackType_Reject, .func=reject});

    _addCallback(ctx, callbackid, JQCallbackType_Resolve, resolve);
    _addCallback(ctx, callbackid, JQCallbackType_Reject, reject);

    return callbackid;
}

bool JQAsyncExecutor::hasCallbackid(uint32_t callbackid) const
{
    return _callbackMap.find(callbackid) != _callbackMap.end();
}

JQuick::sp<JQuick::Handler> JQAsyncExecutor::jsHandler()
{
    return _jsHandler;
}

JSValue JQErrorDesc::createJSValue(JSContext *ctx) const
{
    if (!customValue.is_null()) {
        return bsonToJSValue(ctx, customValue);
    } else {
        JSValue error = JS_NewError(ctx);
        if (!name.empty()) {
            JS_SetPropertyStr(ctx, error, "name", JS_NewStringLen(ctx, name.c_str(), name.size()));
        }
        if (!message.empty()) {
            JS_SetPropertyStr(ctx, error, "message", JS_NewStringLen(ctx, message.c_str(), message.size()));
        }
        JS_SetPropertyStr(ctx, error, "code", JS_NewInt32(ctx, code));
        return error;
    }
}

// can be called not on JSThread
void JQAsyncExecutor::removeCallbackAsync(uint32_t callbackid)
{
    if (callbackid > 0) {
        _jsHandler->run(new JQuick::FunctionalTask(JQuick::bind(&JQAsyncExecutor::removeCallback, JQuick::sp<JQAsyncExecutor>(this), callbackid)));
    }
}

void JQAsyncExecutor::onCallbackJSONAsync(uint32_t callbackid, const std::string &json, const JQErrorDesc * errDesc/*=nullptr*/, bool autoDel/*=true*/)
{
    if (callbackid > 0) {
        JQErrorDesc errDesc0;
        if (errDesc) errDesc0 = *errDesc;
        _jsHandler->run(new JQuick::FunctionalTask(JQuick::bind(&JQAsyncExecutor::_onCallbackJSONJSThread, JQuick::sp<JQAsyncExecutor>(this), callbackid, json, errDesc0, autoDel, errDesc != NULL)));
    }
}

void JQAsyncExecutor::onCallbackAsync(uint32_t callbackid, const Bson &bson, const JQErrorDesc * errDesc/*=nullptr*/, bool autoDel/*=true*/)
{
    if (callbackid > 0) {
        JQErrorDesc errDesc0;
        if (errDesc) errDesc0 = *errDesc;
        _jsHandler->run(new JQuick::FunctionalTask(JQuick::bind(&JQAsyncExecutor::_onCallbackJSThread, JQuick::sp<JQAsyncExecutor>(this), callbackid, bson, errDesc0, autoDel, errDesc != NULL)));
    }
}

void JQAsyncExecutor::onErrorAsync(uint32_t callbackid, const std::string &message, int code/*=0*/, const std::string &name/*=""*/, bool autoDel/*=true*/)
{
    if (callbackid > 0) {
        _jsHandler->run(new JQuick::FunctionalTask(JQuick::bind(&JQAsyncExecutor::onError, JQuick::sp<JQAsyncExecutor>(this), callbackid, message, code, name, autoDel)));
    }
}

void JQAsyncExecutor::_onCallbackJSONJSThread(uint32_t callbackid, const std::string &json, const JQErrorDesc &errDesc, bool autoDel, bool hasErrDesc)
{
    onCallbackJSON(callbackid, json, hasErrDesc ? &errDesc: NULL, autoDel);
}

void JQAsyncExecutor::_onCallbackJSThread(uint32_t callbackid, const Bson &bson, const JQErrorDesc &errDesc, bool autoDel, bool hasErrDesc)
{
    onCallback(callbackid, bson, hasErrDesc ? &errDesc: NULL, autoDel);
}

static std::string getFixedMapString()
{
    static std::string fixedMap;
    if (fixedMap.empty()) {
        // must get key with some library identifier, because of multi shared libraries may have multi code
        fixedMap = jq_printf("__jq_AE_cbMap_%p", &getFixedMapString);
    }
    return fixedMap;
}

//static
void _addCallback(JSContext* ctx, uint32_t token, int type, JSValue val)
{
    JSValue globalObject = JS_GetGlobalObject(ctx);
    std::string fixedMap = getFixedMapString();
    JSAtom cbMapAtom = JS_NewAtom(ctx, fixedMap.c_str());
    JSValue cbMapObj = JS_GetProperty(ctx, globalObject, cbMapAtom);
    if (JS_IsUndefined(cbMapObj)) {
        cbMapObj = JS_NewObject(ctx);
        JS_SetProperty(ctx, globalObject, cbMapAtom, JS_DupValue(ctx, cbMapObj));
    }

    std::string cbKey = jq_printf("%d-%d", token, type);
    JS_SetPropertyStr(ctx, cbMapObj, cbKey.c_str(), val);
    LOGD("JQAsyncExecutor::_addCallback key: %s on map: %s", cbKey.c_str(), fixedMap.c_str());
    JS_FreeValue(ctx, cbMapObj);
    JS_FreeAtom(ctx, cbMapAtom);
    JS_FreeValue(ctx, globalObject);
}

//static
void _removeCallback(JSContext* ctx, uint32_t token, int type)
{
    JSValue globalObject = JS_GetGlobalObject(ctx);
    std::string fixedMap = getFixedMapString();
    JSAtom cbMapAtom = JS_NewAtom(ctx, fixedMap.c_str());
    JSValue cbMapObj = JS_GetProperty(ctx, globalObject, cbMapAtom);
    if (!JS_IsUndefined(cbMapObj)) {
        std::string cbKey = jq_printf("%d-%d", token, type);
        JSAtom cbKeyAtom = JS_NewAtom(ctx, cbKey.c_str());
        JS_DeleteProperty(ctx, cbMapObj, cbKeyAtom, 0);
        LOGD("JQAsyncExecutor::_removeCallback key: %s from map: %s", cbKey.c_str(), fixedMap.c_str());
        JS_FreeAtom(ctx, cbKeyAtom);
    }

    JS_FreeValue(ctx, cbMapObj);
    JS_FreeAtom(ctx, cbMapAtom);
    JS_FreeValue(ctx, globalObject);
}

}  // namespace JQUTIL_NS
