#include "jqutil_v2/JQFunctionTemplate.h"
#include "jqutil_v2/JQTemplateEnv.h"
#include <assert.h>

namespace JQUTIL_NS {

JQFunctionTemplate::JQFunctionTemplate(JQTemplateEnvRef env, const std::string &name)
    : JQTemplate(env), _func(nullptr), _name(name)
{}

// static
JQFunctionTemplateRef JQFunctionTemplate::New(JQTemplateEnvRef env, const std::string &name)
{
    return JQFunctionTemplateRef(new JQFunctionTemplate(env, name));
}

// static
JSValue JQFunctionTemplate::InstanceCtor(JSContext *ctx, JSValueConst new_target,
    int argc, JSValueConst *argv, int magic, JSValueConst *func_data)
{
    JSValue result;

    JQFunctionTemplate* tpl = JQUnwrapRefSimple<JQFunctionTemplate>(func_data[JQ_REFS_TPL_INDEX]);
    if (tpl == NULL) {
        return JS_ThrowInternalError(ctx, "cannot get __tplObj ptr");
    }

    if (tpl->_prototypeTpl.get() && JS_IsUndefined(new_target)) {
        return JS_ThrowInternalError(ctx, "%s prototype function should create instance by new operator", tpl->_name.c_str());
    }

    JSValue prototype = JS_UNDEFINED;
    // find prototype firstly
    if (!JS_IsUndefined(new_target)) {
        prototype = JS_GetPropertyStr(ctx, new_target, "prototype");
        if (JS_IsException(prototype)) {
            return prototype;
        }
        if (JS_IsUndefined(prototype)) {
            // that's ok
        } else if (!JS_IsObject(prototype)) {
            // deformed prototype
            JS_FreeValue(ctx, prototype);
            return JS_ThrowTypeError(ctx, "prototype is deformed");
        }
    }
    if (JS_IsUndefined(prototype)) {
        prototype = JS_NewObject(ctx);
    }

    bool forceCreateCppObj = false;
    if (tpl->_prototypeTpl.get()) {
        // if has async property, we should forcely create cppObj to hold C++ callbackid -> callback reference
        forceCreateCppObj = tpl->_prototypeTpl->_hasAsyncProperty();
    }

    if (forceCreateCppObj) {
        // force create
        tpl->InstanceTemplate();
        JQObjectTemplateRef prototypeTpl = tpl->PrototypeTemplate();
    }

    if (tpl->_instanceTpl.get()) {
        result = tpl->_instanceTpl->NewInstance(forceCreateCppObj);
        if (JS_IsException(result)) {
            JS_FreeValue(ctx, prototype);
            return result;
        }
        // set prototype
        JS_SetPrototype(ctx, result, prototype);
        JS_FreeValue(ctx, prototype);
    } else {
        // create with prototype
        result = JS_NewObjectProto(ctx, prototype);
        JS_FreeValue(ctx, prototype);
    }

    JQBaseObject *cppObj = JQUnwrap<JQBaseObject>(result);
    // call tpl->_func of FuncType
    if (tpl->_func || cppObj) {
        JQFunctionInfo info(ctx);
        info._length = argc;
        info._argv = argv;
        info._this_value = result;
        info._holder = JS_UNDEFINED;
        info._new_target = new_target;
        info._isConstructorCall = !JS_IsUndefined(new_target);
        info._funcTpl = tpl;

        if (tpl->_func) {
            tpl->_func(info);
            JSValueConst ret = info.GetReturnValue().Get();
            if (!JS_IsUndefined(ret)) {
                JS_FreeValue(ctx, result);
                result = JS_DupValue(ctx, ret);
                info.GetReturnValue().SetUndefined();
            }
        }
        if (cppObj) {
            cppObj->OnCtor(info);
            JSValueConst ret = info.GetReturnValue().Get();
            if (!JS_IsUndefined(ret)) {
                JS_FreeValue(ctx, result);
                result = ret;
            }
        }
    }

    return result;
}

JSValue JQFunctionTemplate::GetFunction()
{
    JSValue jsTplObj = JQRefCppSimple::NewJSValue(_tplCtx, this);
    JSValue result = JS_NewCFunctionData(_tplCtx, InstanceCtor, 0/*length*/, 0/*magic*/, 1, &jsTplObj);
    JS_FreeValue(_tplCtx, jsTplObj);

    assert(1/*TRUE*/ == JS_DefinePropertyValueStr(_tplCtx, result, "name", JS_NewString(_tplCtx,
        _name.size() > 0 ? _name.c_str(): "FunctionTemplate"), 0));

    JSValue proto = JS_UNDEFINED;
    if (_prototypeTpl.get()) {
        proto = _prototypeTpl->NewInstance();
    }
    if (JS_IsUndefined(proto)) {
        proto = JS_NewObject(_tplCtx);
    }
    JS_SetConstructor(_tplCtx, result, proto);
    JS_FreeValue(_tplCtx, proto);

    // always support ctor init (new Class)
    JS_SetConstructorBit(_tplCtx, result, 1);

    return result;
}

JQObjectTemplateRef JQFunctionTemplate::InstanceTemplate()
{
    if (!_instanceTpl.get()) {
        _instanceTpl = JQObjectTemplate::New(_tplEnv);
        _instanceTpl->_funcTpl = this;
    }
    return _instanceTpl;
}

JQObjectTemplateRef JQFunctionTemplate::PrototypeTemplate()
{
    if (!_prototypeTpl.get()) {
        _prototypeTpl = JQObjectTemplate::New(_tplEnv);
        _prototypeTpl->_funcTpl = this;
    }
    return _prototypeTpl;
}

JSValue JQFunctionTemplate::CallConstructor(int argc/*=0*/, JSValueConst *argv/*=NULL*/)
{
    JSValue Cls = GetFunction();
    JSValue result = JS_CallConstructor(_tplCtx, Cls, argc, argv);
    JS_FreeValue(_tplCtx, Cls);
    return result;
}

void JQFunctionTemplate::setAsyncScheduleMode(int mode)
{
    InstanceTemplate()->setAsyncScheduleMode(mode);
    PrototypeTemplate()->setAsyncScheduleMode(mode);
}

void JQFunctionTemplate::setAsyncScheduleHook(JQAsyncScheduleHook hook)
{
    InstanceTemplate()->setAsyncScheduleHook(hook);
    PrototypeTemplate()->setAsyncScheduleHook(hook);
}

JQFunctionTemplateRef JQFunctionTemplate::clone() const
{
    JQFunctionTemplateRef result = new JQFunctionTemplate(_tplEnv, _name);
    result->_func = _func;
    if (_instanceTpl != NULL) {
        result->_instanceTpl = _instanceTpl->clone();
        result->_instanceTpl->_funcTpl = result;
    }
    if (_prototypeTpl != NULL) {
        result->_prototypeTpl = _prototypeTpl->clone();
        result->_prototypeTpl->_funcTpl = result;
    }
    return result;
}

}  // namespace JQUTIL_NS