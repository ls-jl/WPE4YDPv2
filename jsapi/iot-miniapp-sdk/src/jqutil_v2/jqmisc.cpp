#include "jqutil_v2/jqmisc.h"
#include "jqutil_v2/jqbson.h"
#include "jqutil_v2/JQRefCpp.h"
#include "utils/log.h"
#include "utils/REF.h"
#include <string.h>
#include <string>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

namespace JQUTIL_NS {

static void jq_dump_obj(JSContext* ctx, JSValueConst val)
{
    const char* str;

    str = JS_ToCString(ctx, val);
    if (str) {
        if (str[0] != '\0') {
            LOGE("%s", str);
        }
        JS_FreeCString(ctx, str);
    } else {
        LOGE("[exception]");
    }
}

void jq_dump_error1(JSContext* ctx, JSValueConst exception_val)
{
    JSValue val;
    int is_error;

    is_error = JS_IsError(ctx, exception_val);
    jq_dump_obj(ctx, exception_val);
    if (is_error) {
        val = JS_GetPropertyStr(ctx, exception_val, "stack");
        if (!JS_IsUndefined(val)) {
            jq_dump_obj(ctx, val);
        }
        JS_FreeValue(ctx, val);
    }
}

void jq_dump_error(JSContext* ctx)
{
    JSValue exception_val;

    exception_val = JS_GetException(ctx);
    jq_dump_error1(ctx, exception_val);
    JS_FreeValue(ctx, exception_val);
}

int jq_eval_binary_with_exc(JSContext* ctx, const uint8_t* buf, size_t buf_len)
{
    JSValue obj, val;
    obj = JS_ReadObject(ctx, buf, buf_len, JS_READ_OBJ_BYTECODE);
    if (JS_IsException(obj))
        goto exception;
    if (JS_VALUE_GET_TAG(obj) == JS_TAG_MODULE) {
        if (JS_ResolveModule(ctx, obj) < 0) {
            JS_FreeValue(ctx, obj);
            goto exception;
        }
    }
    val = JS_EvalFunction(ctx, obj);
    if (JS_IsException(val)) {
        exception:
        jq_dump_error(ctx);
        return 1;
    }
    JS_FreeValue(ctx, val);
    return 0;
}

JSValue jq_call_return(JSContext* ctx, JSValueConst func,const std::vector<JSValueConst> &args/*={}*/,
                       JSValueConst this_val/*=JS_UNDEFINED*/)
{
    JS_DupValue(ctx, func);
    JS_DupValue(ctx, this_val);
    JSValue ret;
    if (args.size() == 0) {
        ret = JS_Call(ctx, func, this_val, 0, NULL);
    } else {
        ret = JS_Call(ctx, func, this_val, args.size(), (JSValueConst*)(&args.at(0)));
    }
    if (JS_IsException(ret)) {
        jq_dump_error(ctx);
    }
    JS_FreeValue(ctx, this_val);
    JS_FreeValue(ctx, func);
    return ret;
}

bool jq_call_void(JSContext *ctx, JSValueConst func, const std::vector<JSValueConst> &args/*={}*/,
                  JSValueConst this_val/*=JS_UNDEFINED*/)
{
    bool result;
    JS_DupContext(ctx);
    JSValue ret = jq_call_return(ctx, func, args, this_val);
    result = !JS_IsException(ret);
    JS_FreeValue(ctx, ret);
    JS_FreeContext(ctx);
    return result;
}

bool jq_call_method_void(JSContext *ctx, JSValueConst obj, const std::string &methodName,
                         const std::vector<JSValueConst> &args/*={}*/)
{
    JSValue propFunc = JS_GetPropertyStr(ctx, obj, methodName.c_str());
    if (JS_IsException(propFunc)) {
        LOGE("get method failed %s", methodName.c_str());
        jq_dump_error(ctx);
        return false;
    }
    if (!JS_IsFunction(ctx, propFunc)) {
        LOGE("can not call method, not function, %s", methodName.c_str());
        return false;
    }
    return jq_call_void(ctx, propFunc, args, obj);
}

JSValue jq_call_method_return(JSContext *ctx, JSValueConst obj, const std::string &methodName,
                              const std::vector<JSValueConst> &args/*={}*/)
{
    JSValue propFunc = JS_GetPropertyStr(ctx, obj, methodName.c_str());
    if (JS_IsException(propFunc)) {
        LOGE("get method failed %s", methodName.c_str());
        jq_dump_error(ctx);
        return propFunc;
    }
    if (!JS_IsFunction(ctx, propFunc)) {
        LOGE("can not call method, not function, %s", methodName.c_str());
        return JS_ThrowInternalError(ctx, "can not call method, not function, %s", methodName.c_str());
    }
    return jq_call_return(ctx, propFunc, args, obj);
}

JSValue jq_bind_obj_func(JSContext *ctx, JSValueConst obj, const std::string &funcName, JSValueConst this_obj/*=JS_UNDEFINED*/)
{
    JSValue objFunc = JS_GetPropertyStr(ctx, obj, funcName.c_str());
    if (!JS_IsFunction(ctx, objFunc)) {
        JS_FreeValue(ctx, objFunc);
        return JS_ThrowInternalError(ctx, "bind func \"%s\" is not js function", funcName.c_str());
    }
    JSValue bindFunc = JS_GetPropertyStr(ctx, objFunc, "bind");
    JSValue result = JS_Call(ctx, bindFunc, objFunc, 1, JS_IsUndefined(this_obj) ? &obj: &this_obj);
    JS_FreeValue(ctx, bindFunc);
    JS_FreeValue(ctx, objFunc);
    return result;
}

JSValue _deserializeJson(JSContext *ctx, const Bson& obj)
{
    if (obj.is_null()) {
        return JS_NULL;
    } else if (obj.is_double()) {
        return JS_NewFloat64(ctx, obj.double_value());
    } else if (obj.is_int()) {
        return JS_NewInt32(ctx, obj.int_value());
    } else if (obj.is_bool()) {
        return JS_NewBool(ctx, obj.bool_value());
    } else if (obj.is_string()) {
        return JS_NewString(ctx, obj.string_value().c_str());
    } else if (obj.is_array()) {
        JSValue jarr = JS_NewArray(ctx);
        int idx = 0;
        for (auto &item: obj.array_items()) {
            JS_SetPropertyUint32(ctx, jarr, idx++, _deserializeJson(ctx, item));
        }
        return jarr;
    } else if (obj.is_object()) {
        JSValue jobj = JS_NewObject(ctx);
        for (auto &iter: obj.object_items()) {
            JS_SetPropertyStr(ctx, jobj, iter.first.c_str(), _deserializeJson(ctx, iter.second));
        }
        return jobj;
    } else if (obj.is_binary()) {
        return JS_NewArrayBufferCopy(ctx, obj.binary_value().data(), obj.binary_value().size());
    } else {
        // unexpect
        abort();
    }
}

JSValue bsonToJSValue(JSContext *ctx, const Bson& json)
{
    return _deserializeJson(ctx, json);
}

Bson _serializeJson(JSContext *ctx, JSValueConst value)
{
    if (JS_VALUE_GET_TAG(value) == JS_TAG_INT) {
        int32_t val = 0;
        JS_ToInt32(ctx, &val, value);
        return Bson(val);
    } else if (JS_TAG_IS_FLOAT64(JS_VALUE_GET_TAG(value))) {
        double val = 0;
        JS_ToFloat64(ctx, &val, value);
        return Bson(val);
    } else if (JS_IsUndefined(value) || JS_IsNull(value)) {
        return Bson();
    } else if (JS_IsBool(value)) {
        return Bson(JS_ToBool(ctx, value) == 1);
    } else if (JS_IsString(value)) {
        size_t plen;
        const char* cstr = JS_ToCStringLen(ctx, &plen, value);
        Bson result(std::string(cstr, plen));
        JS_FreeCString(ctx, cstr);
        return result;
    } else if (JQ_IsABOrABView(ctx, value)) {
        size_t byteLength;
        JSValue ab = JQ_ToArrayBuffer(ctx, value);
        const uint8_t* buf = JS_GetArrayBuffer(ctx, &byteLength, ab);
        Bson::binary bin;
        if (buf) {
            bin.resize(byteLength);
            memcpy(bin.data(), buf, byteLength);
        }
        JS_FreeValue(ctx, ab);
        return bin;
    } else if (JS_IsArray(ctx, value)) {
        // array
        Bson::array result;

        uint32_t len = 0;
        JSValue len0 = JS_GetPropertyStr(ctx, value, "length");
        JS_ToUint32(ctx, &len, len0);
        JS_FreeValue(ctx, len0);

        for (unsigned idx=0; idx < len; idx++) {
            JSValue item = JS_GetPropertyUint32(ctx, value, idx);
            result.push_back(_serializeJson(ctx, item));
            JS_FreeValue(ctx, item);
        }

        return result;
    } else if (JS_IsObject(value)) {
        Bson::object result;
        uint32_t len, i;
        JSPropertyEnum *tab;
        const char *key;
        JSValue val;

        if (JS_GetOwnPropertyNames(ctx, &tab, &len, value,
                JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0) {
            return false;
        }

        for(i = 0; i < len; i++) {
            key = JS_AtomToCString(ctx, tab[i].atom);
            if (!key) {
                continue;
            }

            val = JS_GetProperty(ctx, value, tab[i].atom);
            if (JS_IsException(val)) {
                JS_FreeCString(ctx, key);
                continue;
            }

            result[key] = _serializeJson(ctx, val);

            JS_FreeValue(ctx, val);
            JS_FreeCString(ctx, key);
        }

        for(i = 0; i < len; i++)
            JS_FreeAtom(ctx, tab[i].atom);
        js_free(ctx, tab);

        return result;
    } else {
        return Bson();
    }
}

Bson JSValueToBson(JSContext *ctx, JSValueConst val)
{
    return _serializeJson(ctx, val);
}

JSValue mapToJSValue(JSContext* ctx, const std::map<std::string, std::string> &map)
{
    JSValue result = JS_NewObject(ctx);
    for (auto &iter: map) {
        JS_SetPropertyStr(ctx, result, iter.first.c_str(),
                          JS_NewStringLen(ctx, iter.second.c_str(), iter.second.size()));
    }
    return result;
}

JSValue getOrCreateFunc(JSContext *ctx, const char* funcName, const char* funcCode)
{
    JSValue globalObject = JS_GetGlobalObject(ctx);
    std::string funcNameInGlobal = jq_printf("__jq_%s_%p", funcName, &getOrCreateFunc);
    JSValue funcObj = JS_GetPropertyStr(ctx, globalObject, funcNameInGlobal.c_str());
    if (JS_IsUndefined(funcObj)) {
        std::string funcCodeWrap = jq_printf("globalThis.%s=(%s)", funcNameInGlobal.c_str(), funcCode);
        JSValue r = JS_Eval(ctx, funcCodeWrap.data(), funcCodeWrap.size(), funcNameInGlobal.c_str(), JS_EVAL_TYPE_GLOBAL);
        if (JS_IsException(r)) {
            jq_dump_error(ctx);
            funcObj = r;
        } else {
            JS_FreeValue(ctx, r);
            funcObj = JS_GetPropertyStr(ctx, globalObject, funcNameInGlobal.c_str());
        }
    }
    JS_FreeValue(ctx, globalObject);
    return funcObj;
}

bool JQ_IsABOrABView(JSContext *ctx, JSValueConst val)
{
    // isView is true, when object is typedarray and dataview, then checking the ArrayBuffer type
    static const char* funcCode = \
"function(obj){return ArrayBuffer.isView(obj) || obj instanceof ArrayBuffer;}";
    JSValue func = getOrCreateFunc(ctx, "IsABOrABView", funcCode);
    if (JS_IsException(func)) {
        return false;
    }
    JSValue ret = JS_Call(ctx, func, JS_UNDEFINED, 1, &val);
    JS_FreeValue(ctx, func);
    if (JS_IsException(ret)) {
        jq_dump_error(ctx);
        return false;
    }
    bool result = 1 == JS_ToBool(ctx, ret);
    JS_FreeValue(ctx, ret);
    return result;
}

JSValue JQ_NewUint8Array(JSContext *ctx, JSValueConst arg0, JSValueConst arg1/*=JS_UNDEFINED*/, JSValueConst arg2/*=JS_UNDEFINED*/)
{
    static const char* funcCode = \
"function(arg0, arg1, arg2){return new Uint8Array(arg0, arg1, arg2);}";
    JSValue func = getOrCreateFunc(ctx, "NewUint8Array", funcCode);
    if (JS_IsException(func)) {
        return func;
    }
    JSValueConst argv[3] = {arg0, arg1, arg2};
    JSValue ret = JS_Call(ctx, func, JS_UNDEFINED, 3, argv);
    JS_FreeValue(ctx, func);
    if (JS_IsException(ret)) {
        jq_dump_error(ctx);
        return ret;
    }
    return ret;
}

JSValue JQ_ToArrayBuffer(JSContext *ctx, JSValueConst value)
{
    static const char* funcCode = \
"function(obj){"
"if (ArrayBuffer.isView(obj)) return obj.buffer;"
"else if (obj instanceof ArrayBuffer) return obj;"
"else return undefined;"
"}";
    JSValue func = getOrCreateFunc(ctx, "ToArrayBuffer", funcCode);
    if (JS_IsException(func)) {
        return func;
    }
    JSValue ret = JS_Call(ctx, func, JS_UNDEFINED, 1, &value);
    JS_FreeValue(ctx, func);
    if (JS_IsException(ret)) {
        jq_dump_error(ctx);
        return ret;
    }
    return ret;
}

JQStdFuncTask::JQStdFuncTask(std::function<void()> func)
        :_func(func)
{}

JQStdFuncTask::~JQStdFuncTask()
{}

void JQStdFuncTask::run()
{
    if (_func) {
        _func();
    }
}

int jq_vsnprintf(char *outBuf, size_t size, const char* format, va_list ap)
{
#ifdef _WIN32
    int count = -1;
    if (size != 0)
        count = _vsnprintf_s(outBuf, size, _TRUNCATE, format, ap);
    if (count == -1)
        count = _vscprintf(format, ap);
    return count;
#else
    return vsnprintf(outBuf, size, format, ap);
#endif
}

void jq_printf_va(std::string &buf, const char* fmt, va_list ap)
{
    unsigned len;
    if (buf.size() == 0) buf.resize(128);

    va_list ap1;
    va_copy(ap1, ap);
    len = jq_vsnprintf(&buf.at(0), buf.size(), fmt, ap1);
    va_end(ap1);
    if (len >= buf.size()) {
        // not enough to save
        buf.resize(len+1);  // snprintf ensure a \0 terminating

        va_copy(ap1, ap);
        jq_vsnprintf(&buf.at(0), buf.size(), fmt, ap1);
        va_end(ap1);
        buf.resize(len);  // resize to string length after snprintf fill the \0 ternimating
    } else {
        buf.resize(len);
    }
}

std::string jq_printf(const char* fmt, ...)
{
    std::string buf(128, 0);

    va_list ap;
    va_start(ap, fmt);
    jq_printf_va(buf, fmt, ap);
    va_end(ap);

    return buf;
}

CtxHolder::CtxHolder(JSContext* ctx)
    :JQuick::REF_BASE(), ctx(ctx)
{}

CtxHolder::~CtxHolder()
{}

JQuick::wp<CtxHolder> getOrCreateCtxHolder(JSContext *ctx)
{
    JSValue globalObject = JS_GetGlobalObject(ctx);
    static std::string nameStr = jq_printf("__jq_ctxWp_%p", &getOrCreateCtxHolder).c_str();
    JSAtom nameAtom = JS_NewAtomLen(ctx, nameStr.c_str(), nameStr.size());
    JQuick::sp<CtxHolder> ctxHolder;

    JSValue holderObj = JS_GetProperty(ctx, globalObject, nameAtom);
    ctxHolder = JQUnwrapRefSimple<CtxHolder>(holderObj);
    if (ctxHolder == NULL) {
        // else create/set the pointer and return
        ctxHolder = new CtxHolder(ctx);
        JS_DefinePropertyValue(ctx, globalObject, nameAtom,
                               JQRefCppSimple::NewJSValue(ctx, ctxHolder.get()), 0);
    }

    JS_FreeValue(ctx, holderObj);
    JS_FreeAtom(ctx, nameAtom);
    JS_FreeValue(ctx, globalObject);

    return ctxHolder;
}

class GetSetHolder: public JQuick::REF_BASE {
public:
    GetSetHolder(const std::string &sourceKey, const std::string &key)
            :sourceKey(sourceKey), key(key) {}
    std::string sourceKey;
    std::string key;
};

static JSValue GetterProxy(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv, int magic, JSValueConst *func_data)
{
    GetSetHolder *holder = JQUnwrapRefSimple<GetSetHolder>(func_data[0]);
    JSValue source = JS_GetPropertyStr(ctx, this_val, holder->sourceKey.c_str());
    JSValue val = JS_GetPropertyStr(ctx, source, holder->key.c_str());
    JS_FreeValue(ctx, source);
    return val;
}

static JSValue SetterProxy(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv, int magic, JSValueConst *func_data)
{
    if (argc > 0) {
        GetSetHolder *holder = JQUnwrapRefSimple<GetSetHolder>(func_data[0]);
        JSValue source = JS_GetPropertyStr(ctx, this_val, holder->sourceKey.c_str());
        JS_SetPropertyStr(ctx, source, holder->key.c_str(), JS_DupValue(ctx, argv[0]));
        JS_FreeValue(ctx, source);
    }
    return JS_UNDEFINED;
}

void jq_define_property(JSContext *ctx, JSValueConst target, const std::string &sourceKey, const std::string &key)
{
    JSValue holder = JQRefCppSimple::NewJSValue(ctx, new GetSetHolder(sourceKey, key));
    JSValue getter = JS_NewCFunctionData(ctx, GetterProxy, 0, 0, 1, &holder);
    JSValue setter = JS_NewCFunctionData(ctx, SetterProxy, 0, 0, 1, &holder);
    JS_FreeValue(ctx, holder);
    JSAtom prop = JS_NewAtom(ctx, key.c_str());
    JS_DefinePropertyGetSet(ctx, target, prop, getter, setter, 0);
    JS_FreeAtom(ctx, prop);
}

}  // namespace JQUTIL_NS