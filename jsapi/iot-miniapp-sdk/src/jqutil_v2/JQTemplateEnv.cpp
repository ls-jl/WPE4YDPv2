#include "jqutil_v2/JQTemplateEnv.h"
#include "jqutil_v2/JQObjectTemplate.h"
#include "jqutil_v2/JQFunctionTemplate.h"
#include "jqutil_v2/JQNamedThread.h"
#include "utils/log.h"

namespace JQUTIL_NS {

JQTemplateEnv::JQTemplateEnv(JSContext *ctx)
    :_envCtx(ctx)
{
    _handler = new JQuick::Handler(JQuick::getJSLooper(JS_GetRuntime(ctx)));
}

JQTemplateEnv::~JQTemplateEnv()
{}

void JQTemplateEnv::OnGCCollect()
{
    for (auto &iter: _nameFuncMap) {
        JS_FreeValue(_envCtx, iter.second);
    }
    _nameFuncMap.clear();
    _objTplMap.clear();
    _funcTplMap.clear();
}
void JQTemplateEnv::OnGCMark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func)
{
    for (auto &iter: _nameFuncMap) {
        JS_MarkValue(rt, iter.second, mark_func);
    }
}

JSContext* JQTemplateEnv::context() const
{
    return _envCtx;
}

void JQTemplateEnv::postJSThread(std::function<void()> func)
{
    _handler->post(new JQUTIL_NS::JQStdFuncTask(func));
}

void JQTemplateEnv::postNamedThread(const std::string &name, std::function<void()> func)
{
    PostOnNamedThread(name, func);
}

JSValueConst JQTemplateEnv::getFunction(const std::string &funcName)
{
    auto iter = _nameFuncMap.find(funcName);
    if (iter == _nameFuncMap.end()) {
        return JS_UNDEFINED;
    } else {
        return iter->second;
    }
}
void JQTemplateEnv::registerFunction(const std::string &funcName, JSValue func)
{
    auto iter = _nameFuncMap.find(funcName);
    if (iter == _nameFuncMap.end()) {
        _nameFuncMap[funcName] = func;
    } else {
        jq_set_value(_envCtx, &iter->second, func);
    }
}
void JQTemplateEnv::unregisterFunction(const std::string &funcName)
{
    auto iter = _nameFuncMap.find(funcName);
    if (iter != _nameFuncMap.end()) {
        JS_FreeValue(_envCtx, iter->second);
        _nameFuncMap.erase(iter);
    }
}

JQObjectTemplateRef JQTemplateEnv::getObjectTemplate(const std::string &tplName)
{
    auto iter = _objTplMap.find(tplName);
    if (iter == _objTplMap.end()) {
        return NULL;
    } else {
        return iter->second;
    }
}
void JQTemplateEnv::registerObjectTemplate(const std::string &tplName, JQObjectTemplateRef tpl)
{
    _objTplMap[tplName] = tpl;
}
void JQTemplateEnv::unregisterObjectTemplate(const std::string &tplName)
{
    auto iter = _objTplMap.find(tplName);
    if (iter != _objTplMap.end()) {
        _objTplMap.erase(iter);
    }
}

JQFunctionTemplateRef JQTemplateEnv::getFunctionTemplate(const std::string &tplName)
{
    auto iter = _funcTplMap.find(tplName);
    if (iter == _funcTplMap.end()) {
        return NULL;
    } else {
        return iter->second;
    }
}
void JQTemplateEnv::registerFunctionTemplate(const std::string &tplName, JQFunctionTemplateRef tpl)
{
    _funcTplMap[tplName] = tpl;
}
void JQTemplateEnv::unregisterFunctionTemplate(const std::string &tplName)
{
    auto iter = _funcTplMap.find(tplName);
    if (iter != _funcTplMap.end()) {
        _funcTplMap.erase(iter);
    }
}

JQGlobalObjectEnv::JQGlobalObjectEnv(JSContext *ctx)
    :JQTemplateEnv(ctx)
{}

std::string JQGlobalObjectEnv::dataDir() const
{
    return _dataDir;
}

std::string JQGlobalObjectEnv::appid() const
{
    return _appid;
}

std::string JQGlobalObjectEnv::name() const
{
    return _name;
}

std::string JQGlobalObjectEnv::appDir() const
{
    return _appDir;
}

// static
JQuick::sp<JQGlobalObjectEnv> JQGlobalObjectEnv::Create(JSContext *ctx, const std::string &name)
{
    JQuick::sp<JQGlobalObjectEnv> tplEnv = new JQGlobalObjectEnv(ctx);
    tplEnv->_name = name;
    JQFunctionTemplateRef tpl = JQFunctionTemplate::New(tplEnv, name.c_str());

    tpl->InstanceTemplate()->setObjectCreator([tplEnv]() {
        return tplEnv.get();
    });

    JSValue envJSObject = tpl->CallConstructor(0, NULL);

    JSValue globalObject = JS_GetGlobalObject(ctx);
    JS_DefinePropertyValueStr(ctx, globalObject, name.c_str(), envJSObject, 0);
    JS_FreeValue(ctx, globalObject);

    return tplEnv;
}

JQModuleEnv::JQModuleEnv(JSContext *ctx, JSModuleDef* def)
    :JQTemplateEnv(ctx), _def(def)
{
    _appid = JQuick::getAppid(ctx);
    _dataDir = JQuick::getDataDir(ctx);
    _appDir = JQuick::getWorkspace(ctx);
}

JSModuleDef* JQModuleEnv::moduleDef() const
{
    return _def;
}

void JQModuleEnv::setModuleField(const std::string &name, JSValue obj)
{
    JS_SetPropertyStr(_envCtx, getJSValue(), name.c_str(), JS_DupValue(_envCtx, obj));
}

void JQModuleEnv::setModuleExport(const std::string &name, JSValue obj, bool withFieldSet/*=true*/)
{
    if (withFieldSet) {
        JS_SetPropertyStr(_envCtx, getJSValue(), name.c_str(), JS_DupValue(_envCtx, obj));
    }

    auto iter = _collectModuleExportEntries.find(name);
    if (iter != _collectModuleExportEntries.end()) {
        LOGE("JQModuleEnv duplicate setModuleExport of %s", name.c_str());
        JS_FreeValue(_envCtx, iter->second);
    }

    _collectModuleExportEntries[name] = obj;
}

// module is same as getJSValue()
void JQModuleEnv::setModuleExportDone(JSValue module/*=JS_UNDEFINED*/, const std::vector<std::string> &exportList/*={}*/)
{
    if (JS_IsUndefined(module)) {
        // default module
        module = getJSValue();
    }

    for (auto &name: exportList) {
        auto iter = _collectModuleExportEntries.find(name);
        if (iter != _collectModuleExportEntries.end()) {
            JS_SetModuleExport(_envCtx, _def, name.c_str(), iter->second);
            _collectModuleExportEntries.erase(iter);
        } else {
            std::string moduleName = this->name();
            LOGE("JQModuleEnv module %s, failed to export (can not find) %s ", moduleName.c_str(), name.c_str());
        }
    }

    for (auto &iter: _collectModuleExportEntries) {
        std::string moduleName = name();
        LOGE("JQModuleEnv module %s, invalid to export (not add before) %s", moduleName.c_str(), iter.first.c_str());
        JS_FreeValue(_envCtx, iter.second);
    }

    _collectModuleExportEntries.clear();

    assert(JS_IsObject(module));
    JSValueConst selfValue = getJSValue();
    if (JS_VALUE_GET_PTR(module) != JS_VALUE_GET_PTR(selfValue)) {
        JS_DefinePropertyValueStr(_envCtx, module, "__module_env__", getJSValue(), 0);
    }

    // finally to export self
    JS_SetModuleExport(_envCtx, _def, "default", module);
}

void JQModuleEnv::setModuleExportFailed()
{
    LOGD("JQModuleEnv::setModuleExportFailed on module %s", _name.c_str());
    JS_FreeValue(_envCtx, getJSValue());
}

// static
JQuick::sp<JQModuleEnv> JQModuleEnv::CreateModule(JSContext *ctx, JSModuleDef* def, const std::string &moduleName)
{
    JQuick::sp<JQModuleEnv> moduleEnv = new JQModuleEnv(ctx, def);
    JQFunctionTemplateRef tpl = JQFunctionTemplate::New(moduleEnv, moduleName.c_str());

    tpl->InstanceTemplate()->setObjectCreator([moduleEnv]() {
      return moduleEnv.get();
    });

    // NOTE: return value is attached to moduleEnv cppObj,
    // will be freed when calling method moduleEnv->setModuleExportDone()
    // JSValue module =
    tpl->CallConstructor(0, NULL);
    moduleEnv->_name = moduleName;
    return moduleEnv;
}

std::string JQModuleEnv::dataDir() const
{
    return _dataDir;
}

std::string JQModuleEnv::appid() const
{
    return _appid;
}

std::string JQModuleEnv::name() const
{
    return _name;
}

std::string JQModuleEnv::appDir() const
{
    return _appDir;
}

}  // namespace JQUTIL_NS