#include "jqutil_v2/JQObjectTemplate.h"
#include "jqutil_v2/JQFunctionTemplate.h"
#include "jqutil_v2/jqmisc.h"
#include "jqutil_v2/JQAsyncExecutor.h"
#include "jqutil_v2/JQAsyncSchedule.h"
#include "jqutil_v2/JQBaseObject.h"
#include "jqutil_v2/JQBaseObjectExt.h"
#include "jqutil_v2/JQTemplateEnv.h"
#include "threadpool/ThreadPool.h"
#include "quickjs/quickjs.h"
#include "JQuickContext.h"
#include <algorithm>

namespace JQUTIL_NS {

JQObjectProperty::JQObjectProperty()
{
    type = JQ_PROPERTY_UNKNOWN;

    get_cfunc.jsc_func = NULL;
    get_tpl = NULL;

    set_cfunc.jsc_func = NULL;
    set_tpl = NULL;

    get_cb = NULL;
    set_cb = NULL;

    flags = JQ_PROP_DEF;

    // primitive types

    // union {
    //     bool boolVal;
    //     int32_t int32Val;
    //     uint32_t uint32Val;
    //     double doubleVal;
    // } u;
    u.doubleVal = 0;

    objTpl = NULL;
    strVal = "";

    // signal binder function
    signalBinder = NULL;

    // JQProperty functions
    prop_set_cfunc = NULL;
    prop_get_cfunc = NULL;

    // async function
    async_cb = NULL;
}

JQObjectProperty::~JQObjectProperty() {

}

std::string JQObjectProperty::getName(JSContext *ctx) const
{
    std::string result;
    const char* cstr = JS_AtomToCString(ctx, prop);
    result = cstr;
    JS_FreeCString(ctx, cstr);
    return result;
}

// NOTE: _tplCtx can not be duplicated, because no gcmark gccollect cycle on JQTemplate instance.
//  and we do not consider multiple context case now.
JQTemplate::JQTemplate(JQTemplateEnvRef env)
    :JQuick::REF_BASE(), _tplCtx(env->context()), _tplEnv(env)
{
    if (_tplCtx) {
        // save it, used in dtor
        _tplRT = JS_GetRuntime(_tplCtx);
    }
}

JQTemplateEnvRef JQTemplate::tplEnv() const
{
    return _tplEnv;
}

std::string JQTemplate::appid() const
{
    return _tplEnv->appid();
}

std::string JQTemplate::moduleName() const
{
    return _tplEnv->name();
}

JQuick::sp<JQuick::Handler> JQTemplate::jsHandler() const
{
    return _tplEnv->jsHandler();
}


JQTemplate::~JQTemplate() {}

// === JQTemplate END ===

// === JQObjectTemplate START ===

JQObjectTemplate::JQObjectTemplate(JQTemplateEnvRef env)
    :JQTemplate(env), _objCreator(nullptr)
{}

JQObjectTemplate::~JQObjectTemplate()
{
    // _propertyMap.clear();
}

// static
JQObjectTemplateRef JQObjectTemplate::New(JQTemplateEnvRef env)
{
    return JQObjectTemplateRef(new JQObjectTemplate(env));
}

JQObjectProperty& JQObjectTemplate::SetAccessor(JSAtom prop, JSCFunction getter, JSCFunction setter)
{
    JQObjectProperty p;
    p.type=JQ_ACCESSOR_CFUNCTION, p.get_cfunc.jsc_func=getter, p.set_cfunc.jsc_func=setter;
    return _setProperty(prop, p);
}

JQObjectProperty& JQObjectTemplate::SetAccessor(JSAtom prop, JQFunctionCallback getter, JQFunctionCallback setter)
{
    JQObjectProperty p;
    p.type=JQ_ACCESSOR_CFUNCTION_CALLBACK, p.get_cfunc.jq_func_cb=getter, p.set_cfunc.jq_func_cb=setter;
    return _setProperty(prop, p);
}

JQObjectProperty& JQObjectTemplate::SetAccessor(JSAtom prop, JQFunctionTemplateRef getter, JQFunctionTemplateRef setter)
{
    JQObjectProperty p;
    p.type=JQ_ACCESSOR_FUNCTION_TEMPLATE, p.get_tpl=getter, p.set_tpl=setter;
    return _setProperty(prop, p);
}

JQObjectProperty& JQObjectTemplate::SetAccessor(const char* prop, JQFunctionTemplateRef getter, JQFunctionTemplateRef setter)
{
    JSAtom prop1 = JS_NewAtom(_tplCtx, prop);
    JQObjectProperty& result = SetAccessor(prop1, getter, setter);
    JS_FreeAtom(_tplCtx, prop1);
    return result;
}

JQObjectProperty& JQObjectTemplate::SetAccessor(JSAtom prop, JQFunctionCallbackType getter, JQFunctionCallbackType setter)
{
    JQObjectProperty p;
    p.type=JQ_ACCESSOR_FUNCTION_CALLBACK, p.get_cb=getter, p.set_cb=setter;
    return _setProperty(prop, p);
}

JQObjectProperty& JQObjectTemplate::Set(JSAtom prop, JSCFunction func)
{
    JQObjectProperty p;
    p.type=JQ_PROPERTY_CFUNCTION, p.get_cfunc.jsc_func=func;
    return _setProperty(prop, p);
}

JQObjectProperty& JQObjectTemplate::Set(JSAtom prop, JQFunctionCallback func)
{
    JQObjectProperty p;
    p.type=JQ_PROPERTY_CFUNCTION_CALLBACK, p.get_cfunc.jq_func_cb=func;
    return _setProperty(prop, p);
}

JQObjectProperty& JQObjectTemplate::Set(JSAtom prop, JQFunctionCallbackType func)
{
    JQObjectProperty p;
    p.type=JQ_PROPERTY_FUNCTION_CALLBACK, p.get_cb=func;
    return _setProperty(prop, p);
}

JQObjectProperty& JQObjectTemplate::_SetAsync(JSAtom prop, JQAsyncCallbackType func)
{
    JQObjectProperty p;
    p.type=JQ_PROPERTY_ASYNC_CALLBACK, p.async_cb=func;
    JQObjectProperty &result = _setProperty(prop, p);
    return result;
}

JQObjectProperty& JQObjectTemplate::_SetAsyncStd(JSAtom prop, JQAsyncCallbackType func)
{
    JQObjectProperty p;
    p.type=JQ_PROPERTY_ASYNC_STD_CALLBACK, p.async_cb=func;
    JQObjectProperty &result = _setProperty(prop, p);
    return result;
}

JQObjectProperty& JQObjectTemplate::_SetPromise(JSAtom prop, JQAsyncCallbackType func)
{
    JQObjectProperty p;
    p.type=JQ_PROPERTY_PROMISE_CALLBACK, p.async_cb=func;
    JQObjectProperty &result = _setProperty(prop, p);
    return result;
}

JQObjectProperty& JQObjectTemplate::Set(JSAtom prop, JQFunctionTemplateRef tpl)
{
    JQObjectProperty p; p.type=JQ_PROPERTY_FUNCTION_TEMPLATE, p.get_tpl=tpl;
    return _setProperty(prop, p);
}

JQObjectProperty& JQObjectTemplate::Set(const char* prop, JQFunctionTemplateRef tpl)
{
    JSAtom prop1 = JS_NewAtom(_tplCtx, prop);
    JQObjectProperty& result = Set(prop1, tpl);
    JS_FreeAtom(_tplCtx, prop1);
    return result;
}

// property of JQObjectTemplate
JQObjectProperty& JQObjectTemplate::Set(JSAtom prop, JQObjectTemplateRef tpl)
{
    JQObjectProperty p; p.type=JQ_PROPERTY_OBJECT_TEMPLATE, p.objTpl=tpl;
    return _setProperty(prop, p);
}

// bool
JQObjectProperty& JQObjectTemplate::SetBool(JSAtom prop, bool val)
{
    JQObjectProperty p; p.type=JQ_PROPERTY_TYPE_BOOL, p.u.boolVal=val;
    return _setProperty(prop, p);
}
// int32
JQObjectProperty& JQObjectTemplate::Set(JSAtom prop, int32_t val)
{
    JQObjectProperty p; p.type=JQ_PROPERTY_TYPE_INT32, p.u.int32Val=val;
    return _setProperty(prop, p);
}
// uint32
JQObjectProperty& JQObjectTemplate::Set(JSAtom prop, uint32_t val)
{
    JQObjectProperty p; p.type=JQ_PROPERTY_TYPE_UINT32, p.u.uint32Val=val;
    return _setProperty(prop, p);
}
// double
JQObjectProperty& JQObjectTemplate::Set(JSAtom prop, double val)
{
    JQObjectProperty p; p.type=JQ_PROPERTY_TYPE_DOUBLE, p.u.doubleVal=val;
    return _setProperty(prop, p);
}
// string
JQObjectProperty& JQObjectTemplate::Set(JSAtom prop, const std::string &val)
{
    JQObjectProperty p; p.type=JQ_PROPERTY_TYPE_STRING, p.strVal=val;
    return _setProperty(prop, p);
}

JQObjectProperty& JQObjectTemplate::_setProperty(JSAtom prop, JQObjectProperty &property)
{
    std::map<JSAtom, JQObjectProperty>::iterator iter = _propertyMap.find(prop);
    if (iter != _propertyMap.end()) {
        // release previous atom
        JS_FreeAtom(_tplCtx, iter->first);
    }
    // else
    property.prop = prop;
    return _propertyMap.emplace(JS_DupAtom(_tplCtx, prop), property).first->second;
}

// static
JSValue JQObjectTemplate::Caller(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv, int magic,
                                 int isGetterSetter/* 0 getter/ 1 setter*/,
                                 JSValueConst *refs)
{
    JSValue result = JS_UNDEFINED;
    JSAtom prop = (JSAtom)magic;

    JQObjectTemplate *objTpl = JQUnwrapRefSimple<JQObjectTemplate>(refs[JQ_REFS_TPL_INDEX]);

    auto iter = objTpl->_propertyMap.find(prop);
    if (iter == objTpl->_propertyMap.end()) {
        return JS_ThrowInternalError(ctx, "cannot get JQObjectTemplate property");
    }
    JQObjectProperty &property = iter->second;

#define CREATE_FUNCTION_INFO \
    JQFunctionInfo info(ctx); \
    info._length = argc; \
    info._argv = argv; \
    info._this_value = this_val; \
    info._holder = refs[JQ_REFS_CPP_INDEX]; \
    info._new_target = this_val; \
    info._isConstructorCall = false; \
    info._objTpl = objTpl; \
    info._property = &property;

// forceCreateCppObj flag from NewInstance, always exists in async case
#define CREATE_ASYNC_INFO \
    assert(thisCppObj != NULL); \
    JQAsyncInfo infoAsync; \
    infoAsync._thisCppObj = thisCppObj; \
    infoAsync._holderCppObj = JQUnwrapRefSimple<JQBaseObject>(refs[JQ_REFS_CPP_INDEX]); \
    infoAsync._objTpl = objTpl; \
    infoAsync._property = &property;

#define CREATE_PREPARE_INFO \
    JQPrepareInfo prepareInfo; \
    prepareInfo._cppObj = thisCppObj; \
    prepareInfo._info = &infoAsync; \
    prepareInfo._ctx = ctx; \
    prepareInfo._argc = argc; \
    prepareInfo._argv = argv;

    JQBaseObject* thisCppObj = JQUnwrap< JQBaseObject >(this_val);
    if (thisCppObj && thisCppObj->isJSCallDisabled()) {
        return JS_ThrowInternalError(ctx, "this object js call is disabled");
    }

    if (property.type == JQ_ACCESSOR_CFUNCTION) {
        if (isGetterSetter == 0) {
            if (property.get_cfunc.jsc_func) {
                result = property.get_cfunc.jsc_func(ctx, this_val, argc, argv);
            }
        } else {
            if (property.set_cfunc.jsc_func) {
                result = property.set_cfunc.jsc_func(ctx, this_val, argc, argv);
            }
        }
    } else if (property.type == JQ_ACCESSOR_CFUNCTION_CALLBACK) {
        CREATE_FUNCTION_INFO
        if (isGetterSetter == 0) {
            if (property.get_cfunc.jq_func_cb) {
                property.get_cfunc.jq_func_cb(info);
                result = info.GetReturnValue().Get();
            }
        } else {
            if (property.set_cfunc.jq_func_cb) {
                property.set_cfunc.jq_func_cb(info);
                result = info.GetReturnValue().Get();
            }
        }
    } else if (property.type == JQ_ACCESSOR_FUNCTION_CALLBACK) {
        CREATE_FUNCTION_INFO
        if (isGetterSetter == 0) {
            if (property.get_cb) {
                property.get_cb(info);
                result = info.GetReturnValue().Get();
            }
        } else {
            if (property.set_cb) {
                property.set_cb(info);
                result = info.GetReturnValue().Get();
            }
        }
    } else if (property.type == JQ_PROPERTY_CFUNCTION) {
        if (property.get_cfunc.jsc_func) {
            result = property.get_cfunc.jsc_func(ctx, this_val, argc, argv);
        }
    } else if (property.type == JQ_PROPERTY_CFUNCTION_CALLBACK) {
        CREATE_FUNCTION_INFO
        if (property.get_cfunc.jq_func_cb) {
            property.get_cfunc.jq_func_cb(info);
            result = info.GetReturnValue().Get();
        }
    } else if (property.type == JQ_PROPERTY_FUNCTION_CALLBACK) {
        CREATE_FUNCTION_INFO
        if (property.get_cb) {
            property.get_cb(info);
            result = info.GetReturnValue().Get();
        }
    } else if (property.type == JQ_PROPERTY_PROPERTY) {
        CREATE_FUNCTION_INFO
        if (isGetterSetter == 0) {
            if (property.prop_get_cfunc) {
                property.prop_get_cfunc(info);
                result = info.GetReturnValue().Get();
            }
        } else {
            if (property.prop_set_cfunc) {
                property.prop_set_cfunc(info);
                result = info.GetReturnValue().Get();
            }
        }
    } else if (property.type == JQ_PROPERTY_ASYNC_CALLBACK) {
        CREATE_ASYNC_INFO

        // first arg is params
        if (argc > 0) {
            infoAsync._params.push_back(JSValueToBson(ctx, argv[0]));
        }

        // second arg is callback, and always register callback on this_obj's cppObj
        if (argc > 1) {
            infoAsync._callbackId = thisCppObj->getOrCreateAsyncExecutor()->addCallback(argv[1], JQCallbackType_Simple);
        } else {
            infoAsync._callbackId = 0;
        }

        objTpl->_asyncSchedule.dispatch(objTpl, thisCppObj, property, infoAsync);  // to call property.async_cb
    } else if (property.type == JQ_PROPERTY_ASYNC_STD_CALLBACK) {
        CREATE_ASYNC_INFO

        // prepare callbackid hook
        if (property.prepareCallbackidHook) {
            CREATE_PREPARE_INFO
            infoAsync._callbackId = property.prepareCallbackidHook(prepareInfo);
        }

        // default the last param is the callback (according to nodejs)
        if (infoAsync._callbackId == 0 && argc > 0) {
            if (JS_IsFunction(ctx, argv[argc-1])) {
                infoAsync._callbackId = thisCppObj->getOrCreateAsyncExecutor()->addCallback(argv[argc-1], JQCallbackType_Std);
            }
        }

        // prepare params hook
        bool notSettled = true;
        bool paramsPrepared = false;
        if (property.prepareParamsHook) {
            CREATE_PREPARE_INFO
            paramsPrepared = property.prepareParamsHook(prepareInfo);
            // may settled (rejected) in prepareParamsHook
            notSettled = thisCppObj->getOrCreateAsyncExecutor()->hasCallbackid(infoAsync._callbackId);
        }

        if (notSettled) {
            if (!paramsPrepared) {
                // parepare default params
                for (int idx=0; idx < argc; idx++) {
                    infoAsync._params.push_back(JSValueToBson(ctx, argv[idx]));
                }
            }
            objTpl->_asyncSchedule.dispatch(objTpl, thisCppObj, property, infoAsync);  // to call property.async_cb
        }
    } else if (property.type == JQ_PROPERTY_PROMISE_CALLBACK) {
        CREATE_ASYNC_INFO

        // result is promise object, or exception
        const char* propName = JS_AtomToCString(ctx, prop);
        std::string tip = objTpl->functionName() + "." + propName;
        JS_FreeCString(ctx, propName);
        infoAsync._callbackId = thisCppObj->getOrCreateAsyncExecutor()->createPromiseId(ctx, &result, tip);
        if (infoAsync._callbackId != 0) {
            bool notSettled = true;
            bool paramsPrepared = false;
            if (property.prepareParamsHook) {
                CREATE_PREPARE_INFO
                paramsPrepared = property.prepareParamsHook(prepareInfo);
                // may settled (rejected) in prepareParamsHook
                notSettled = thisCppObj->getOrCreateAsyncExecutor()->hasCallbackid(infoAsync._callbackId);
            }
            if (notSettled) {
                if (!paramsPrepared) {
                    // parepare default params
                    for (int idx=0; idx < argc; idx++) {
                        infoAsync._params.push_back(JSValueToBson(ctx, argv[idx]));
                    }
                }
                objTpl->_asyncSchedule.dispatch(objTpl, thisCppObj, property, infoAsync);  // to call property.async_cb
            }
        }
    } else {
        abort();
    }

    return result;
}

// static
JSValue JQObjectTemplate::FuncCaller(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv, int magic, JSValueConst *func_data)
{
    return Caller(ctx, this_val, argc, argv, magic, 0, func_data);
}
// static
JSValue JQObjectTemplate::SetterCaller(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv, int magic, JSValueConst *func_data)
{
    return Caller(ctx, this_val, argc, argv, magic, 1, func_data);
}
// static
JSValue JQObjectTemplate::GetterCaller(JSContext *ctx, JSValueConst this_val,
                                    int argc, JSValueConst *argv, int magic, JSValueConst *func_data)
{
    return Caller(ctx, this_val, argc, argv, magic, 0, func_data);
}

JSValue JQObjectTemplate::NewInstance(bool forceCreateCppObj/* = false*/)
{
    JSValue result;
    JSValue refs[2] = {JS_UNDEFINED, JS_UNDEFINED};
    JQBaseObject* cppObj = NULL;

    if (forceCreateCppObj || _objCreator || _hasAsyncProperty()) {
        if (_objCreator) {
            cppObj = _objCreator();
            if (cppObj == NULL) {
                return JS_ThrowInternalError(_tplCtx, "got NULL, cpp creator should create a BaseObject instance.");
            }
        } else {
            // force create
            cppObj = new JQBaseObject();
        }
        cppObj->_objTpl = this;

        result = JQBaseObject::NewObject(_tplCtx, cppObj);
        refs[JQ_REFS_CPP_INDEX] = JQRefCppSimple::NewJSValue(_tplCtx, cppObj);
    } else {
        result = JS_NewObject(_tplCtx);
    }

    refs[JQ_REFS_TPL_INDEX] = JQRefCppSimple::NewJSValue(_tplCtx, this);

    injectProperties(result, countof(refs), refs, cppObj);
    for (unsigned idx = 0; idx < countof(refs); idx++) {
        if (!JS_IsUndefined(refs[idx])) {
            JS_FreeValue(_tplCtx, refs[idx]);
        }

    }

    return result;
}

void JQObjectTemplate::injectProperties(JSValueConst result, int refsCount, JSValueConst *refs, JQBaseObject* cppObj)
{
    // accessors
    for (auto &iter: _propertyMap) {
        JSAtom prop = iter.first;
        if (iter.second.type == JQ_ACCESSOR_CFUNCTION ||
            iter.second.type == JQ_ACCESSOR_CFUNCTION_CALLBACK ||
            iter.second.type == JQ_ACCESSOR_FUNCTION_CALLBACK) {
            // accessor
            JSValue getter = JS_NewCFunctionData(_tplCtx, GetterCaller,
                                                 0, /*magic*/prop, refsCount, refs);
            JSValue setter = JS_NewCFunctionData(_tplCtx, SetterCaller,
                                                 0, /*magic*/prop, refsCount, refs);

            JS_DefinePropertyGetSet(_tplCtx, result, prop, getter, setter, iter.second.flags);
        } else if (iter.second.type == JQ_ACCESSOR_FUNCTION_TEMPLATE) {
            // function template setter getter
            JSValue getter = iter.second.get_tpl->GetFunction();
            JSValue setter = iter.second.set_tpl->GetFunction();

            JS_DefinePropertyGetSet(_tplCtx, result, prop, getter, setter, iter.second.flags);
        } else if (iter.second.type == JQ_PROPERTY_CFUNCTION ||
                   iter.second.type == JQ_PROPERTY_CFUNCTION_CALLBACK ||
                   iter.second.type == JQ_PROPERTY_FUNCTION_CALLBACK ||
                   iter.second.type == JQ_PROPERTY_ASYNC_CALLBACK ||
                   iter.second.type == JQ_PROPERTY_ASYNC_STD_CALLBACK ||
                   iter.second.type == JQ_PROPERTY_PROMISE_CALLBACK) {
            // function
            JSValue func = JS_NewCFunctionData(_tplCtx, FuncCaller,
                                               0, /*magic*/prop, refsCount, refs);
            JS_DefinePropertyValue(_tplCtx, result, prop, func, iter.second.flags);
        } else if (iter.second.type == JQ_PROPERTY_FUNCTION_TEMPLATE) {
            JS_DefinePropertyValue(_tplCtx, result, prop, iter.second.get_tpl->GetFunction(), iter.second.flags);
        } else if (iter.second.type == JQ_PROPERTY_OBJECT_TEMPLATE) {
            JS_DefinePropertyValue(_tplCtx, result, prop, iter.second.objTpl->NewInstance(), iter.second.flags);
        }
        // primitive value
        else if (iter.second.type == JQ_PROPERTY_TYPE_BOOL) {
            JSValue val = JS_NewBool(_tplCtx, iter.second.u.boolVal);
            JS_DefinePropertyValue(_tplCtx, result, prop, val, iter.second.flags);
        } else if (iter.second.type == JQ_PROPERTY_TYPE_INT32) {
            JSValue val = JS_NewInt32(_tplCtx, iter.second.u.int32Val);
            JS_DefinePropertyValue(_tplCtx, result, prop, val, iter.second.flags);
        } else if (iter.second.type == JQ_PROPERTY_TYPE_UINT32) {
            JSValue val = JS_NewUint32(_tplCtx, iter.second.u.uint32Val);
            JS_DefinePropertyValue(_tplCtx, result, prop, val, iter.second.flags);
        } else if (iter.second.type == JQ_PROPERTY_TYPE_DOUBLE) {
            JSValue val = JS_NewFloat64(_tplCtx, iter.second.u.doubleVal);
            JS_DefinePropertyValue(_tplCtx, result, prop, val, iter.second.flags);
        } else if (iter.second.type == JQ_PROPERTY_TYPE_STRING) {
            JSValue val = JS_NewStringLen(_tplCtx, iter.second.strVal.c_str(), iter.second.strVal.size());
            JS_DefinePropertyValue(_tplCtx, result, prop, val, iter.second.flags);
        } else if (iter.second.type == JQ_PROPERTY_SIGNAL) {
            // call signalBinder to bind signal to cppObj methods itself
            if (cppObj) {
                iter.second.signalBinder(cppObj, prop);
            }
            // then construct signal property to set on to instance
            JSValue publisherObj = JS_NewObject(_tplCtx);
            JSValue onFunc = JS_NewCFunctionData(_tplCtx, JQObjectSignalRegister::_SignalSubscribe, 1, /*magic*/prop, refsCount, refs);
            JSValue offFunc = JS_NewCFunctionData(_tplCtx, JQObjectSignalRegister::_SignalUnsubscribe, 1, /*magic*/prop, refsCount, refs);
            JS_DefinePropertyValueStr(_tplCtx, publisherObj, "on", onFunc, 0);
            JS_DefinePropertyValueStr(_tplCtx, publisherObj, "off", offFunc, 0);
            JS_DefinePropertyValue(_tplCtx, result, prop, publisherObj, iter.second.flags);
        } else if (iter.second.type == JQ_PROPERTY_PROPERTY) {
            // call signalBinder to bind signal to cppObj methods itself
            if (cppObj) {
                iter.second.signalBinder(cppObj, prop);
            }
            // then construct signal property to set on to instance
            JSValue publisherObj = JS_NewObject(_tplCtx);
            JSValue onFunc = JS_NewCFunctionData(_tplCtx, JQObjectSignalRegister::_SignalSubscribe, 1, /*magic*/ prop, refsCount, refs);
            JSValue offFunc = JS_NewCFunctionData(_tplCtx, JQObjectSignalRegister::_SignalUnsubscribe, 1, /*magic*/ prop, refsCount, refs);
            JS_DefinePropertyValueStr(_tplCtx, publisherObj, "on", onFunc, 0);
            JS_DefinePropertyValueStr(_tplCtx, publisherObj, "off", offFunc, 0);

            JSValue getter = JS_NewCFunctionData(_tplCtx, GetterCaller, 1, /*magic*/ prop, refsCount, refs);
            JSValue setter = JS_NewCFunctionData(_tplCtx, SetterCaller, 1, /*magic*/ prop, refsCount, refs);
            JSAtom prop_value = JS_NewAtom(_tplCtx, "value");
            JS_DefinePropertyGetSet(_tplCtx, publisherObj, prop_value, getter, setter, 0);
            JS_FreeAtom(_tplCtx, prop_value);
            JS_DefinePropertyValue(_tplCtx, result, prop, publisherObj, iter.second.flags);
        } else {
            abort();
        }
    }
}

bool JQObjectTemplate::_hasAsyncProperty() const
{
    for (auto &iter: _propertyMap) {
        if (iter.second.type == JQ_PROPERTY_ASYNC_CALLBACK ||
            iter.second.type == JQ_PROPERTY_ASYNC_STD_CALLBACK ||
            iter.second.type == JQ_PROPERTY_PROMISE_CALLBACK) {
            return true;
        }
    }
    return false;
}

std::string JQObjectTemplate::functionName() const
{
    if (!_funcTpl.get()) {
        return "";
    } else {
        return _funcTpl->name();
    }
}

void JQObjectTemplate::setAsyncScheduleMode(int mode)
{
    _asyncSchedule.setMode(mode);
}

void JQObjectTemplate::setAsyncScheduleHook(JQAsyncScheduleHook hook)
{
    _asyncSchedule.setHook(hook);
}

// for JQBaseObjectExt
JQInternalValueHolder* JQObjectTemplate::getOrCreateInternalValueHolder(uintptr_t objPtr)
{
    auto iter = _objInternalValueHolderMap.find(objPtr);
    if (iter != _objInternalValueHolderMap.end()) {
        return iter->second;
    }
    // else create
    JQInternalValueHolder *holder = new JQInternalValueHolder();
    _objInternalValueHolderMap[objPtr] = holder;
    return holder;
}

JQInternalValueHolder* JQObjectTemplate::getInternalValueHolder(uintptr_t objPtr) const
{
    auto iter = _objInternalValueHolderMap.find(objPtr);
    if (iter == _objInternalValueHolderMap.end()) {
        return NULL;
    }
    return iter->second;
}

void JQObjectTemplate::collectAndDestroyInternalValueHolder(uintptr_t objPtr, JSContext *ctx)
{
    auto iter = _objInternalValueHolderMap.find(objPtr);
    if (iter == _objInternalValueHolderMap.end()) {
        return;
    }
    iter->second->onGCCollect(ctx);
    delete iter->second;
    _objInternalValueHolderMap.erase(iter);
}

JQInternalRefHolder* JQObjectTemplate::getOrCreateInternalRefHolder(uintptr_t objPtr)
{
    auto iter = _objInternalRefHolderMap.find(objPtr);
    if (iter != _objInternalRefHolderMap.end()) {
        return iter->second;
    }
    // else create
    JQInternalRefHolder *holder = new JQInternalRefHolder();
    _objInternalRefHolderMap[objPtr] = holder;
    return holder;
}

JQInternalRefHolder* JQObjectTemplate::getInternalRefHolder(uintptr_t objPtr) const
{
    auto iter = _objInternalRefHolderMap.find(objPtr);
    if (iter == _objInternalRefHolderMap.end()) {
        return NULL;
    }
    return iter->second;
}

void JQObjectTemplate::destroyInternalRefHolder(uintptr_t objPtr)
{
    auto iter = _objInternalRefHolderMap.find(objPtr);
    if (iter == _objInternalRefHolderMap.end()) {
        return;
    }
    delete iter->second;
    _objInternalRefHolderMap.erase(iter);
}

JQAsyncExecutor* JQObjectTemplate::getOrCreateAsyncExecutor(uintptr_t objPtr)
{
    JQuick::Mutex::Autolock lock(_mutex);
    auto iter = _objAsyncExecutorMap.find(objPtr);
    if (iter != _objAsyncExecutorMap.end()) {
        return iter->second;
    }
    // else create
    JQAsyncExecutor* holder = new JQAsyncExecutor(((JQBaseObject*)objPtr)->getContext(), jsHandler());
    holder->REF();
    _objAsyncExecutorMap[objPtr] = holder;
    return holder;
}

JQAsyncExecutor* JQObjectTemplate::getAsyncExecutor(uintptr_t objPtr) const
{
    JQuick::Mutex::Autolock lock(_mutex);
    auto iter = _objAsyncExecutorMap.find(objPtr);
    if (iter == _objAsyncExecutorMap.end()) {
        return NULL;
    }
    return iter->second;
}

void JQObjectTemplate::destroyAsyncExecutor(uintptr_t objPtr)
{
    JQuick::Mutex::Autolock lock(_mutex);
    auto iter = _objAsyncExecutorMap.find(objPtr);
    if (iter == _objAsyncExecutorMap.end()) {
        return;
    }
    iter->second->UNREF();
    _objAsyncExecutorMap.erase(iter);
}

JQObjectSignalRegister* JQObjectTemplate::getOrCreateSignalRegister(uintptr_t objPtr)
{
    auto iter = _objSignalRegisterMap.find(objPtr);
    if (iter != _objSignalRegisterMap.end()) {
        return iter->second;
    }
    // else create
    JQObjectSignalRegister *signalRegister = new JQObjectSignalRegister((JQBaseObject*)objPtr);
    _objSignalRegisterMap[objPtr] = signalRegister;
    return signalRegister;
}

JQObjectSignalRegister* JQObjectTemplate::getSignalRegister(uintptr_t objPtr) const
{
    auto iter = _objSignalRegisterMap.find(objPtr);
    if (iter == _objSignalRegisterMap.end()) {
        return NULL;
    }
    return iter->second;
}

void JQObjectTemplate::collectSignalRegister(uintptr_t objPtr, JSContext *ctx)
{
    auto iter = _objSignalRegisterMap.find(objPtr);
    if (iter == _objSignalRegisterMap.end()) {
        return;
    }
    iter->second->onGCCollect(ctx);
}

void JQObjectTemplate::destroySignalRegister(uintptr_t objPtr)
{
    auto iter = _objSignalRegisterMap.find(objPtr);
    if (iter == _objSignalRegisterMap.end()) {
        return;
    }
    delete iter->second;
    _objSignalRegisterMap.erase(iter);
}

JQObjectGCRegister* JQObjectTemplate::getOrCreateGCRegister(uintptr_t objPtr)
{
    auto iter = _objGCRegisterMap.find(objPtr);
    if (iter != _objGCRegisterMap.end()) {
        return iter->second;
    }
    // else create
    JQObjectGCRegister *gcRegister = new JQObjectGCRegister();
    _objGCRegisterMap[objPtr] = gcRegister;
    return gcRegister;
}

JQObjectGCRegister* JQObjectTemplate::getGCRegister(uintptr_t objPtr) const
{
    auto iter = _objGCRegisterMap.find(objPtr);
    if (iter == _objGCRegisterMap.end()) {
        return NULL;
    }
    return iter->second;
}

void JQObjectTemplate::collectAndDestroyGCRegister(uintptr_t objPtr, JSContext *ctx)
{
    auto iter = _objGCRegisterMap.find(objPtr);
    if (iter == _objGCRegisterMap.end()) {
        return;
    }
    iter->second->onGCCollect((JQBaseObject*)objPtr, ctx);
    delete iter->second;
    _objGCRegisterMap.erase(iter);
}

JQObjectTemplateRef JQObjectTemplate::clone() const
{
    JQObjectTemplateRef result = new JQObjectTemplate(_tplEnv);
    JSContext *ctx = _tplCtx;
    result->_propertyMap = _propertyMap;
    for (auto &iter: result->_propertyMap) {
        JS_DupAtom(ctx, iter.first);
    }
    result->_objCreator = _objCreator;
    result->_asyncSchedule = _asyncSchedule;
    return result;
}

}  // namespace JQUTIL_NS
