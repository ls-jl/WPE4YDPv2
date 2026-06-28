#include "jqutil_v2/JQBaseObject.h"
#include "jqutil_v2/JQRefCpp.h"
#include "jqutil_v2/JQObjectTemplate.h"
#include "jqutil_v2/JQFunctionTemplate.h"
#include "jqutil_v2/JQAsyncExecutor.h"
#include "jqutil_v2/JQTemplateEnv.h"

// #define ENABLE_DUP_CONTEXT

#ifdef ENABLE_DUP_CONTEXT
// quickjs-ext.h
extern "C" void QJS_MarkContext(JSRuntime *rt, JSContext *ctx, JS_MarkFunc* mark_func);
#endif

namespace JQUTIL_NS {

// === JQBaseObject START ===
JQBaseObject::JQBaseObject()
        : REF_BASE()
{
}

JQBaseObject::~JQBaseObject()
{
#ifdef ENABLE_JQBASEOBJECT_FLAG
    if (_asyncExecutorFlag()) {
        _setAsyncExecutorFlag(false);
        _objTpl->destroyAsyncExecutor((uintptr_t)this);
    }
    if (_signalRegisterStage1Flag()) {
        _setSignalRegisterStage1Flag(false);
        _objTpl->destroySignalRegister((uintptr_t)this);
    }
    if (_internalRefHolderFlag()) {  // delete again
        _setInternalRefHolderFlag(false);
        _objTpl->destroyInternalRefHolder((uintptr_t)this);
    }
    assert(_flags == 0);
#else
    _objTpl->destroyAsyncExecutor((uintptr_t)this);
    _objTpl->destroySignalRegister((uintptr_t)this);
    // delete again
    _objTpl->destroyInternalRefHolder((uintptr_t)this);
#endif
}

void JQBaseObject::_on_js_detach()
{
    OnGCCollect();
    JSContext* ctx = getContext();

#ifdef ENABLE_JQBASEOBJECT_FLAG
    if (_gcRegisterFlag()) {
        _setGcRegisterFlag(false);
        _objTpl->collectAndDestroyGCRegister((uintptr_t)this, ctx);
    }

    if (_internalValueHolderFlag()) {
        _setInternalValueHolderFlag(false);
        _objTpl->collectAndDestroyInternalValueHolder((uintptr_t)this, ctx);
    }

    if (_internalRefHolderFlag()) {
        _setInternalRefHolderFlag(false);
        _objTpl->destroyInternalRefHolder((uintptr_t)this);
    }

    if (_signalRegisterStage0Flag()) {
        _setSignalRegisterStage0Flag(false);
        _objTpl->collectSignalRegister((uintptr_t)this, ctx);
    }
    _setAttachedFlag(false);
#else
    _objTpl->collectAndDestroyGCRegister((uintptr_t)this, ctx);
    _objTpl->collectAndDestroyInternalValueHolder((uintptr_t)this, ctx);
    _objTpl->destroyInternalRefHolder((uintptr_t)this);
    _objTpl->collectSignalRegister((uintptr_t)this, ctx);
#endif

    if (_valuePtr) {
        _valuePtr = NULL;
    }

#ifdef ENABLE_DUP_CONTEXT
    JS_FreeContext(ctx);
#endif
    _ctx = NULL;
}

void JQBaseObject::_on_js_attach(JSContext *ctx, JSValueConst value)
{
    JSContext* oldCtx = getContext();
    bool valid = true;
    if (oldCtx) {
        valid = (intptr_t)oldCtx & 0x01;
        JS_FreeContext(oldCtx);
    }

    if (_valuePtr) {
        _valuePtr = NULL;
    }

#ifdef ENABLE_DUP_CONTEXT
    ctx = JS_DupContext(ctx);
#endif
    if (valid) {
        ctx = (JSContext*)((intptr_t)ctx | 0x01);
    }
    _ctx = ctx;
    _valuePtr = JS_VALUE_GET_PTR(value);

#ifdef ENABLE_JQBASEOBJECT_FLAG
    _setAttachedFlag(true);
#endif

    OnInit();
}

void JQBaseObject::_on_js_gc_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func)
{
    OnGCMark(rt, val, mark_func);

#ifdef ENABLE_JQBASEOBJECT_FLAG
    if (_gcRegisterFlag()) {
        JQObjectGCRegister *gcRegister = _objTpl->getGCRegister((uintptr_t)this);
        if (gcRegister) {
            gcRegister->onGCMark(this, rt, val, mark_func);
        }
    }

    if (_internalValueHolderFlag()) {
        JQInternalValueHolder *internalValueHolder = _objTpl->getInternalValueHolder((uintptr_t)this);
        if (internalValueHolder) {
            internalValueHolder->onGCMark(rt, val, mark_func);
        }
    }

    if (_signalRegisterFlag()) {
        JQObjectSignalRegister *signalRegister = _objTpl->getSignalRegister((uintptr_t)this);
        if (signalRegister) {
            signalRegister->onGCMark(rt, val, mark_func);
        }
    }
#else
    JQObjectGCRegister *gcRegister = _objTpl->getGCRegister((uintptr_t)this);
    if (gcRegister) {
        gcRegister->onGCMark(this, rt, val, mark_func);
    }
    JQInternalValueHolder *internalValueHolder = _objTpl->getInternalValueHolder((uintptr_t)this);
    if (internalValueHolder) {
        internalValueHolder->onGCMark(rt, val, mark_func);
    }
    JQObjectSignalRegister *signalRegister = _objTpl->getSignalRegister((uintptr_t)this);
    if (signalRegister) {
        signalRegister->onGCMark(rt, val, mark_func);
    }
#endif

#ifdef ENABLE_DUP_CONTEXT
    JSContext *ctx = getContext();
    if (ctx) {
        QJS_MarkContext(rt, ctx, mark_func);
    }
#endif
}

void JQBaseObject::hookGCCollect(GCCollectHook hook)
{
    JQObjectGCRegister *gcRegister = _objTpl->getOrCreateGCRegister((uintptr_t)this);
#ifdef ENABLE_JQBASEOBJECT_FLAG
    _setGcRegisterFlag(true);
#endif
    gcRegister->pushGCCollectHook(hook);
}

void JQBaseObject::hookGCMark(GCMarkHook hook)
{
    JQObjectGCRegister *gcRegister = _objTpl->getOrCreateGCRegister((uintptr_t)this);
#ifdef ENABLE_JQBASEOBJECT_FLAG
    _setGcRegisterFlag(true);
#endif
    gcRegister->pushGCMarkHook(hook);
}

JQuick::sp<JQObjectTemplate> JQBaseObject::objTpl() const
{
    return _objTpl;
}

JQTemplateEnvRef JQBaseObject::tplEnv() const
{
    return _objTpl->tplEnv();
}

JQuick::sp<JQuick::Handler> JQBaseObject::jsHandler() const
{
    return _objTpl->jsHandler();
}

bool JQBaseObject::inSameThread() const
{
    return _objTpl->jsHandler()->getLooper()->getThreadId() == jquick_thread_get_current();
}

JQInternalValueHolder* JQBaseObject::getInternalValueHolder() const
{
#ifdef ENABLE_JQBASEOBJECT_FLAG
    if (!_internalValueHolderFlag()) {
        return NULL;
    }
#else
    if (!isAttached()) {
        return NULL;
    }
#endif
    return _objTpl->getInternalValueHolder((uintptr_t)this);
}

JQInternalValueHolder* JQBaseObject::getOrCreateInternalValueHolder()
{
    if (!isAttached()) {
        return NULL;
    }
    JQInternalValueHolder* holder = _objTpl->getOrCreateInternalValueHolder((uintptr_t)this);
#ifdef ENABLE_JQBASEOBJECT_FLAG
    _setInternalValueHolderFlag(true);
#endif
    return holder;
}

JQInternalRefHolder* JQBaseObject::getInternalRefHolder() const
{
#ifdef ENABLE_JQBASEOBJECT_FLAG
    if (!_internalRefHolderFlag()) {
        return NULL;
    }
#else
    if (!isAttached()) {
        return NULL;
    }
#endif
    return _objTpl->getInternalRefHolder((uintptr_t)this);
}

JQInternalRefHolder* JQBaseObject::getOrCreateInternalRefHolder()
{
    if (!isAttached()) {
        return NULL;
    }
    JQInternalRefHolder* holder = _objTpl->getOrCreateInternalRefHolder((uintptr_t)this);
#ifdef ENABLE_JQBASEOBJECT_FLAG
    _setInternalRefHolderFlag(true);
#endif
    return holder;
}

JQAsyncExecutor* JQBaseObject::getOrCreateAsyncExecutor()
{
#ifdef ENABLE_JQBASEOBJECT_FLAG
    _setAsyncExecutorFlag(true);
#endif
    return _objTpl->getOrCreateAsyncExecutor((uintptr_t)this);
}

JQuick::sp<JQAsyncExecutor> JQBaseObject::getAsyncExecutor() const
{
#ifdef ENABLE_JQBASEOBJECT_FLAG
    if (!_asyncExecutorFlag()) {
        return NULL;
    }
#endif
    return _objTpl->getAsyncExecutor((uintptr_t)this);
}

JQObjectSignalRegister* JQBaseObject::getOrCreateSignalRegister()
{
#ifdef ENABLE_JQBASEOBJECT_FLAG
    _setSignalRegisterStage0Flag(true);
    _setSignalRegisterStage1Flag(true);
#endif
    return _objTpl->getOrCreateSignalRegister((uintptr_t)this);
}

JQObjectSignalRegister* JQBaseObject::getSignalRegister() const
{
#ifdef ENABLE_JQBASEOBJECT_FLAG
    if (!_signalRegisterFlag()) {
        return NULL;
    }
#endif
    return _objTpl->getSignalRegister((uintptr_t)this);
}

// === JQBaseObject END ===

// === CppObjClass START ===
static JSClassID g_JQBaseObject_classid = 0;

// static
void JQBaseObject::__js_object_finalizer(JSRuntime *rt, JSValue v)
{
    JQBaseObject* cppObj = (JQBaseObject*)JS_GetOpaque(v, g_JQBaseObject_classid);
    if (cppObj) {
        cppObj->_on_js_detach();
        JS_SetOpaque(v, NULL);
        cppObj->UNREF();
    }
}

void JQBaseObject::__js_object_gc_mark(JSRuntime *rt, JSValueConst v, JS_MarkFunc *mark_func)
{
    JQBaseObject* cppObj = (JQBaseObject*)JS_GetOpaque(v, g_JQBaseObject_classid);
    if (cppObj) {
        cppObj->_on_js_gc_mark(rt, v, mark_func);
    }
}

// static
void JQBaseObject::__js_init_object_class(JSContext *ctx)
{
    JSClassDef ClassDef = {
            "JQBaseObjectClass",
            .finalizer = JQBaseObject::__js_object_finalizer,
            .gc_mark = JQBaseObject::__js_object_gc_mark,
    };
    JS_NewClassID(&g_JQBaseObject_classid);
    JS_NewClass(JS_GetRuntime(ctx), g_JQBaseObject_classid, &ClassDef);
    // NOTE: new class default proto is JS_NULL(for all contexts), no need to set it repeatly
    // JS_SetClassProto(ctx, g_JQBaseObject_classid, JS_NULL);
}

// static
JSValue JQBaseObject::NewObject(JSContext *ctx, JQBaseObject* cppObj)
{
    if (g_JQBaseObject_classid == 0 || !JS_IsRegisteredClass(JS_GetRuntime(ctx), g_JQBaseObject_classid)) {
        // NOTE: CppObjClass have no proto, so inited once in Runtime level
        __js_init_object_class(ctx);
    }
    JSValue result = JS_NewObjectClass(ctx, g_JQBaseObject_classid);
    cppObj->REF();
    JS_SetOpaque(result, cppObj);
    cppObj->_on_js_attach(ctx, result);
    return result;
}
// === CppObjClass END ===

JQBaseObject* JQBaseObject::FromJSObject(JSValueConst value) {
    if (!JS_IsObject(value)) return NULL;
    JQBaseObject* cppObj = (JQBaseObject*)JS_GetOpaque(value, g_JQBaseObject_classid);
    return cppObj;
}

}  // namespace JQUTIL_NS