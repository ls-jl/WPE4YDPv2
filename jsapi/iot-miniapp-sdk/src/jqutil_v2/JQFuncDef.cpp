#include "jqutil_v2/JQDefs.h"
#include "jqutil_v2/JQObjectTemplate.h"
#include "jqutil_v2/JQFunctionTemplate.h"
#include "jqutil_v2/JQTemplateEnv.h"
#include "jqutil_v2/JQAsyncExecutor.h"
#include "jqutil_v2/jqmisc.h"

namespace JQUTIL_NS {

void JQReturnValue::ThrowSyntaxError(const char* fmt, ...)
{
    std::string buf;
    va_list ap; va_start(ap, fmt);
    jq_printf_va(buf, fmt, ap);
    va_end(ap);

    jq_set_value(_ctx, &_value, JS_ThrowSyntaxError(_ctx, "%s", buf.c_str()));
}
void JQReturnValue::ThrowTypeError(const char* fmt, ...)
{
    std::string buf;
    va_list ap; va_start(ap, fmt);
    jq_printf_va(buf, fmt, ap);
    va_end(ap);

    jq_set_value(_ctx, &_value, JS_ThrowTypeError(_ctx, "%s", buf.c_str()));
}
void JQReturnValue::ThrowReferenceError(const char* fmt, ...)
{
    std::string buf;
    va_list ap; va_start(ap, fmt);
    jq_printf_va(buf, fmt, ap);
    va_end(ap);

    jq_set_value(_ctx, &_value, JS_ThrowReferenceError(_ctx, "%s", buf.c_str()));
}
void JQReturnValue::ThrowRangeError(const char* fmt, ...)
{
    std::string buf;
    va_list ap; va_start(ap, fmt);
    jq_printf_va(buf, fmt, ap);
    va_end(ap);

    jq_set_value(_ctx, &_value, JS_ThrowRangeError(_ctx, "%s", buf.c_str()));
}
void JQReturnValue::ThrowInternalError(const char* fmt, ...)
{
    std::string buf;
    va_list ap; va_start(ap, fmt);
    jq_printf_va(buf, fmt, ap);
    va_end(ap);

    jq_set_value(_ctx, &_value, JS_ThrowInternalError(_ctx, "%s", buf.c_str()));
}

void JQReturnValue::ThrowOutOfMemory()
{
    jq_set_value(_ctx, &_value, JS_ThrowOutOfMemory(_ctx));
}

JQTemplateEnvRef JQFunctionInfo::tplEnv() const
{
    if (_objTpl.get()) {
        return _objTpl->tplEnv();
    } else if (_funcTpl.get()) {
        return _funcTpl->tplEnv();
    }
    return NULL;
}

JQParamsHolder JQFunctionInfo::toParamsHolder() const
{
    JQParams params;
    JQParamsHolder holder;
    for (unsigned idx=0; idx < _length; idx++) {
        params.push_back(JSValueToBson(_ctx, _argv[idx]));
    }
    holder.SetParams(params);
    return holder;
}

const JQObjectProperty* JQFunctionInfo::getProperty() const
{
    return _property;
}

void JQAsyncInfo::post(const Bson& bson, const JQErrorDesc *errDesc/*=NULL*/) const
{
    JQuick::sp<JQBaseObject> thisCppObj = _thisCppObj.promote();
    if (thisCppObj == NULL) {
        return;
    }

    JQuick::sp<JQAsyncExecutor> executor = thisCppObj->getAsyncExecutor();
    if (executor == NULL) {
        return;
    }
    executor->onCallbackAsync(_callbackId, bson, errDesc, true);
}

void JQAsyncInfo::postJSON(const std::string& json, const JQErrorDesc *errDesc/*=NULL*/) const
{
    JQuick::sp<JQBaseObject> thisCppObj = _thisCppObj.promote();
    if (thisCppObj == NULL) {
        return;
    }

    JQuick::sp<JQAsyncExecutor> executor = thisCppObj->getAsyncExecutor();
    if (executor == NULL) {
        return;
    }

    executor->onCallbackJSONAsync(_callbackId, json, errDesc, true);
}

void JQAsyncInfo::postError(const JQErrorDesc &errDesc) const
{
    JQuick::sp<JQBaseObject> thisCppObj = _thisCppObj.promote();
    if (thisCppObj == NULL) {
        return;
    }

    JQuick::sp<JQAsyncExecutor> executor = thisCppObj->getAsyncExecutor();
    if (executor == NULL) {
        return;
    }
    executor->onCallbackAsync(_callbackId, Bson(), &errDesc, true);
}

void JQAsyncInfo::postError(const std::string &message) const
{
    JQuick::sp<JQBaseObject> thisCppObj = _thisCppObj.promote();
    if (thisCppObj == NULL) {
        return;
    }

    JQuick::sp<JQAsyncExecutor> executor = thisCppObj->getAsyncExecutor();
    if (executor == NULL) {
        return;
    }

    JQErrorDesc errDesc;
    errDesc.message = message;
    executor->onCallbackAsync(_callbackId, Bson(), &errDesc, true);
}

void JQAsyncInfo::postError(const char* fmt, ...) const
{
    std::string buf(128, 0);

    va_list ap;
    va_start(ap, fmt);
    jq_printf_va(buf, fmt, ap);
    va_end(ap);

    postError(buf);
}

void JQAsyncInfo::postError(const Bson &bson) const
{
    JQuick::sp<JQBaseObject> thisCppObj = _thisCppObj.promote();
    if (thisCppObj == NULL) {
        return;
    }

    JQuick::sp<JQAsyncExecutor> executor = thisCppObj->getAsyncExecutor();
    if (executor == NULL) {
        return;
    }

    JQErrorDesc errDesc;
    errDesc.customValue = bson;
    executor->onCallbackAsync(_callbackId, Bson(), &errDesc, true);
}

// static
void JQAsyncInfo::Caller_postJSThread(JQJSThreadCallback func, JQAsyncInfo self)
{
    JQuick::sp<JQBaseObject> thisCppObj = self._thisCppObj.promote();
    if (!thisCppObj.get()) {
        return;
    }

    JSContext* ctx = thisCppObj->getContext();
    if (!ctx) {
        return;
    }

    JQuick::sp<JQAsyncExecutor> executor = thisCppObj->getAsyncExecutor();
    if (executor == NULL) {
        return;
    }

    JQJSThreadInfo info(ctx);
    info._thisCppObj = self._thisCppObj;
    info._holderCppObj = self._holderCppObj;
    info._objTpl = self._objTpl;
    info._data = self._data;

    func(info);  // call function
    JSValueConst result = info.GetReturnValue().Get();
    executor->onCallbackJSValue(self._callbackId, 1, &result, NULL, true);
    info.GetReturnValue().SetUndefined();
}

void JQAsyncInfo::postJSThread(JQJSThreadCallback func) const
{
    JQuick::sp<JQBaseObject> thisCppObj = _thisCppObj.promote();
    if (!thisCppObj.get()) {
        return;
    }

    JQuick::sp<JQAsyncExecutor> executor = thisCppObj->getAsyncExecutor();
    if (executor == NULL) {
        return;
    }

    executor->jsHandler()->run(new JQuick::FunctionalTask(
            JQuick::bind(&JQAsyncInfo::Caller_postJSThread, func, *this)));
}

std::string JQParamsHolder::ParamsToString() const
{
    Bson::array array;
    for (auto &item: _params) {
        array.push_back(item);
    }
    return Bson(array).dump();
}

bool JQAsyncInfo::isSettled() const
{
    JQuick::sp<JQBaseObject> thisCppObj = _thisCppObj.promote();

    if (!thisCppObj.get()) {
        // thisCppObj released, so this promise is rejected
        return true;
    }

    JQuick::sp<JQAsyncExecutor> executor = thisCppObj->getAsyncExecutor();
    if (executor == NULL) {
        return true;
    }

    return executor->hasCallbackid(_callbackId);
}

JQTemplateEnvRef JQAsyncInfo::tplEnv() const
{
    return _objTpl->tplEnv();
}

void JQAsyncInfo::setData(JQuick::REF_BASE* data)
{
    _data = data;
}

JQTemplateEnvRef JQJSThreadInfo::tplEnv() const
{
    return _objTpl->tplEnv();
}

}  // namespace JQUTIL_NS