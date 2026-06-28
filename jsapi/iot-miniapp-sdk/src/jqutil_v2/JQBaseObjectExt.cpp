#include "jqutil_v2/JQBaseObjectExt.h"
#include "jqutil_v2/JQObjectTemplate.h"

namespace JQUTIL_NS {

void JQInternalValueHolder::onGCMark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func)
{
    for (auto &iter: _fields) {
        JS_MarkValue(rt, iter.second, mark_func);
    }
}

void JQInternalValueHolder::onGCCollect(JSContext* ctx)
{
    clear(ctx);
}

void JQInternalValueHolder::setField(JSContext* ctx, const std::string &key, JSValue field)
{
    auto iter = _fields.find(key);
    if (iter == _fields.end()) {
        _fields[key] = field;
    } else {
        jq_set_value(ctx, &_fields[key], field);
    }
}

JSValueConst JQInternalValueHolder::getField(const std::string &key) const
{
    auto iter = _fields.find(key);
    if (iter == _fields.end()) {
        return JS_UNDEFINED;
    }
    return iter->second;
}

bool JQInternalValueHolder::hasField(const std::string &key) const
{
    auto iter = _fields.find(key);
    return iter != _fields.end();
}

bool JQInternalValueHolder::removeField(JSContext* ctx, const std::string &key)
{
    auto iter = _fields.find(key);
    if (iter != _fields.end()) {
        JS_FreeValue(ctx, iter->second);
        _fields.erase(iter);
        return true;
    }
    return false;
}

void JQInternalValueHolder::clear(JSContext *ctx)
{
    assert(ctx != NULL);
    for (auto &iter: _fields) {
        JS_FreeValue(ctx, iter.second);
    }
    _fields.clear();
}

void JQInternalRefHolder::setField(const std::string &key, JQuick::REF_BASE* field)
{
    _fields[key] = field;
}

JQuick::REF_BASE* JQInternalRefHolder::getField(const std::string &key)
{
    auto iter = _fields.find(key);
    if (iter == _fields.end()) {
        return NULL;
    } else {
        return iter->second.get();
    }
}

bool JQInternalRefHolder::hasField(const std::string &key) const
{
    auto iter = _fields.find(key);
    return iter != _fields.end();
}

bool JQInternalRefHolder::removeField(const std::string &key)
{
    auto iter = _fields.find(key);
    if (iter != _fields.end()) {
        _fields.erase(iter);
        return true;
    }
    return false;
}

void JQInternalRefHolder::clear()
{
    _fields.clear();
}

JQObjectSignalRegister::JQObjectSignalRegister(JQuick::wp<JQBaseObject> object)
        :_object(object)
{}

// static
JSValue JQObjectSignalRegister::_SignalSubscribe(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic, JSValueConst *refs)
{
    uint32_t prop = (uint32_t)magic;
    JQBaseObject* cppObj = JQUnwrapRefSimple<JQBaseObject>(refs[JQ_REFS_CPP_INDEX]);
    if (!cppObj) {
        return JS_ThrowInternalError(ctx, "no cppObj to be subscribed");
    }
    JQObjectSignalRegister *signalRegister = cppObj->getOrCreateSignalRegister();

    if (argc > 0 && JS_IsFunction(ctx, argv[0])) {
        std::vector<std::pair<uint32_t, JSValue> > &listeners = signalRegister->_signalListenersMap[prop];
        uint32_t token = ++signalRegister->_tokenId;
        listeners.push_back(std::make_pair(token, JS_DupValue(ctx, argv[0])));
        return JS_NewInt32(ctx, token);
    }
    return JS_ThrowTypeError(ctx, "arg 1 must be function");
}

// static
JSValue JQObjectSignalRegister::_SignalUnsubscribe(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic, JSValueConst *refs)
{
    uint32_t prop = (uint32_t)magic;
    JQBaseObject* cppObj = JQUnwrapRefSimple<JQBaseObject>(refs[JQ_REFS_CPP_INDEX]);
    if (!cppObj) {
        return JS_ThrowInternalError(ctx, "no cppObj to be unsubscribed");
    }
    JQObjectSignalRegister *signalRegister = cppObj->getSignalRegister();
    if (!signalRegister) {
        return JS_UNDEFINED;
    }

    if (signalRegister->_signalListenersMap.find(prop) == signalRegister->_signalListenersMap.end()) {
        return JS_UNDEFINED;
    }

    std::vector< std::pair< uint32_t, JSValue > >& listeners = signalRegister->_signalListenersMap[prop];
    std::vector< std::pair< uint32_t, JSValue > >::iterator iter = listeners.end();

    if (argc > 0) {
        if (JS_IsNumber(argv[0])) {
            uint32_t token;
            if (-1 == JS_ToUint32(ctx, &token, argv[0])) {
                return JS_EXCEPTION;
            }
            for (iter=listeners.begin(); iter!=listeners.end(); iter++) {
                if (iter->first == token) {
                    break;  // found
                }
            }
        } else if (JS_IsFunction(ctx, argv[0])) {
            JSValueConst func = argv[0];
            for (iter=listeners.begin(); iter!=listeners.end(); iter++) {
                if (JS_VALUE_GET_PTR(iter->second) == JS_VALUE_GET_PTR(func)) {
                    break;  // found
                }
            }
        }

        if (iter != listeners.end()) {
            JS_FreeValue(ctx, iter->second);
            listeners.erase(iter);
        }
        return JS_UNDEFINED;
    } else {
        // off all signals
        for (auto &iter: listeners) {
            JS_FreeValue(ctx, iter.second);
        }
        listeners.clear();
    }
    return JS_UNDEFINED;
}

// static
void JQObjectSignalRegister::_signalDoPublish(uint32_t prop, JSContext *ctx, std::vector<JSValueConst> &arr)
{
    // copy and do publish
    std::vector< std::pair< uint32_t, JSValue > > listeners = _signalListenersMap[prop];

    JS_DupContext(ctx);
    for (auto &iter: listeners) {
        JSValue func = JS_DupValue(ctx, iter.second);
        JSValue ret;
        if (arr.size() > 0) {
            // XXX: here arr args maybe modified by some callback, should be sealed?
            ret = JS_Call(ctx, func, JS_UNDEFINED, arr.size(), &arr.at(0));
        } else {
            ret = JS_Call(ctx, func, JS_UNDEFINED, 0, NULL);
        }
        if (JS_IsException(ret)) {
            jq_dump_error(ctx);
        }
        JS_FreeValue(ctx, func);
        JS_FreeValue(ctx, ret);
    }
    JS_FreeContext(ctx);
}

void JQObjectSignalRegister::onGCMark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func)
{
    // call JQObject self gcmark logics
    for (auto &iter: _signalListenersMap) {
        for (auto &iter1: iter.second) {
            // JSValue of listener
            JS_MarkValue(rt, iter1.second, mark_func);
        }
    }
}

void JQObjectSignalRegister::onGCCollect(JSContext* ctx)
{
    // call JQObject self gccollect logics
    for (auto &iter: _signalListenersMap) {
        for (auto &iter1: iter.second) {
            // JSValue of listener
            JS_FreeValue(ctx, iter1.second);
        }
    }
    _signalListenersMap.clear();
}

void JQObjectGCRegister::onGCMark(JQBaseObject *cppObj, JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func)
{
    // call pedding gcmark functions
    for (auto gcMarkFunc: _gcMarkHooks) {
        gcMarkFunc(cppObj, rt, val, mark_func);
    }
}

void JQObjectGCRegister::onGCCollect(JQBaseObject *cppObj, JSContext* ctx)
{
    for (auto gcCollectFunc: _gcCollectHooks) {
        gcCollectFunc(cppObj, ctx);
    }

    // gc hook already called, no need to hold these c++ function ptr (may hold other pointer reference)
    _gcMarkHooks.clear();
    _gcCollectHooks.clear();
}

void JQObjectGCRegister::pushGCCollectHook(JQBaseObject::GCCollectHook hook)
{
    _gcCollectHooks.push_back(hook);
}

void JQObjectGCRegister::pushGCMarkHook(JQBaseObject::GCMarkHook hook)
{
    _gcMarkHooks.push_back(hook);
}

}  // namespace JQUTIL_NS