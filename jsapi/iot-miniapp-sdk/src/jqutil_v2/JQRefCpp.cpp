#include "jqutil_v2/JQRefCpp.h"

namespace JQUTIL_NS {

// == JQRefCppSimple START ==
static JSClassID g_JQRefCppSimple_classid = 0;

static void __js_ref_finalizer_simple(JSRuntime *rt, JSValue v)
{
    JQuick::REF_BASE* cppObj = (JQuick::REF_BASE*)JS_GetOpaque(v, g_JQRefCppSimple_classid);
    if (cppObj) {
        JS_SetOpaque(v, NULL);
        cppObj->UNREF();
    }
}

static void __js_ref_gc_mark_simple(JSRuntime *rt, JSValueConst v, JS_MarkFunc *mark_func)
{
}

static void __js_init_ref_class_simple(JSContext *ctx)
{
    JSClassDef ClassDef = {
            "JQRefCppSimple",
            .finalizer = __js_ref_finalizer_simple,
            .gc_mark = __js_ref_gc_mark_simple,
    };
    JS_NewClassID(&g_JQRefCppSimple_classid);
    JS_NewClass(JS_GetRuntime(ctx), g_JQRefCppSimple_classid, &ClassDef);
    // NOTE: new class default proto is JS_NULL(for all contexts), no need to set it repeatly
    // JS_SetClassProto(ctx, g_JQRefCppSimple_classid, JS_NULL);
}

// static
JSValue JQRefCppSimple::NewJSValue(JSContext *ctx, JQuick::REF_BASE* cppObj)
{
    if (g_JQRefCppSimple_classid == 0 || !JS_IsRegisteredClass(JS_GetRuntime(ctx), g_JQRefCppSimple_classid)) {
        // NOTE: CppObjClass have no proto, so inited once in Runtime level
        __js_init_ref_class_simple(ctx);
    }
    JSValue result = JS_NewObjectClass(ctx, g_JQRefCppSimple_classid);
    cppObj->REF();
    JS_SetOpaque(result, cppObj);
    return result;
}

// static
void* JQRefCppSimple::FromJSObject(JSValueConst value) {
    if (!JS_IsObject(value)) return NULL;
    void* cppObj = (void*)JS_GetOpaque(value, g_JQRefCppSimple_classid);
    return cppObj;
}
// == JQRefCppSimple END ==

#if 0
// == JQRefCpp START ==
JQRefCpp::JQRefCpp()
    :REF_BASE()
{}

JQRefCpp::~JQRefCpp()
{}

static JSClassID g_JQRefCpp_classid = 0;

// static
void JQRefCpp::__js_ref_finalizer(JSRuntime *rt, JSValue v)
{
    JQRefCpp* cppObj = (JQRefCpp*)JS_GetOpaque(v, g_JQRefCpp_classid);
    if (cppObj) {
        JS_SetOpaque(v, NULL);
        cppObj->OnGCCollect(rt, v);
        cppObj->UNREF();
    }
}

void JQRefCpp::__js_ref_gc_mark(JSRuntime *rt, JSValueConst v, JS_MarkFunc *mark_func)
{
    JQRefCpp* cppObj = (JQRefCpp*)JS_GetOpaque(v, g_JQRefCpp_classid);
    if (cppObj) {
        cppObj->OnGCMark(rt, v, mark_func);
    }
}

// static
void JQRefCpp::__js_init_ref_class(JSContext *ctx)
{
    JSClassDef ClassDef = {
            "JQRefCppClass",
            .finalizer = JQRefCpp::__js_ref_finalizer,
            .gc_mark = JQRefCpp::__js_ref_gc_mark,
    };
    JS_NewClassID(&g_JQRefCpp_classid);
    JS_NewClass(JS_GetRuntime(ctx), g_JQRefCpp_classid, &ClassDef);
    // NOTE: new class default proto is JS_NULL(for all contexts), no need to set it repeatly
    // JS_SetClassProto(ctx, g_JQRefCpp_classid, JS_NULL);
}

// static
JSValue JQRefCpp::NewJSValue(JSContext *ctx, JQRefCpp* cppObj)
{
    if (g_JQRefCpp_classid == 0 || !JS_IsRegisteredClass(JS_GetRuntime(ctx), g_JQRefCpp_classid)) {
        // NOTE: CppObjClass have no proto, so inited once in Runtime level
        __js_init_ref_class(ctx);
    }
    JSValue result = JS_NewObjectClass(ctx, g_JQRefCpp_classid);
    cppObj->REF();
    JS_SetOpaque(result, cppObj);
    cppObj->OnInit(ctx, result);
    return result;
}

// static
void* JQRefCpp::FromJSObject(JSValueConst value) {
    if (!JS_IsObject(value)) return NULL;
    void* cppObj = (void*)JS_GetOpaque(value, g_JQRefCpp_classid);
    return cppObj;
}
// == JQRefCpp END ==
#endif

}  // namespace JQUTIL_NS