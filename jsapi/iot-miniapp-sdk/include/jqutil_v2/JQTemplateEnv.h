#pragma once
#include "jqutil_v2/JQDefs.h"
#include "jqutil_v2/JQBaseObject.h"
#include "quickjs/quickjs.h"
#include <map>
#include <string>

namespace JQUTIL_NS {

// base class
class JQTemplateEnv: public JQBaseObject {
public:
    JQTemplateEnv(JSContext *ctx);
    virtual ~JQTemplateEnv();

    virtual void OnGCCollect();
    virtual void OnGCMark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func);

    template<typename T>
    JQuick::sp<T> as() { return static_cast<T*>(this); }

    JSContext* context() const;
    inline JQuick::sp<JQuick::Handler> jsHandler() const { return _handler; }
    void postJSThread(std::function<void()> func);
    void postNamedThread(const std::string &name, std::function<void()> func);

    JSValueConst getFunction(const std::string &funcName);
    void registerFunction(const std::string &funcName, JSValue func);
    void unregisterFunction(const std::string &funcName);

    JQObjectTemplateRef getObjectTemplate(const std::string &tplName);
    void registerObjectTemplate(const std::string &tplName, JQObjectTemplateRef tpl);
    void unregisterObjectTemplate(const std::string &tplName);

    JQFunctionTemplateRef getFunctionTemplate(const std::string &tplName);
    void registerFunctionTemplate(const std::string &tplName, JQFunctionTemplateRef tpl);
    void unregisterFunctionTemplate(const std::string &tplName);

    virtual std::string appid() const = 0;
    virtual std::string dataDir() const = 0;
    virtual std::string name() const = 0;
    virtual std::string appDir() const = 0;

protected:
    std::map<std::string, JSValue> _nameFuncMap;
    std::map<std::string, JQObjectTemplateRef> _objTplMap;
    std::map<std::string, JQFunctionTemplateRef> _funcTplMap;
    JSContext *_envCtx;
    JQuick::sp<JQuick::Handler> _handler;
};

class JQGlobalObjectEnv: public JQTemplateEnv {
public:
    JQGlobalObjectEnv(JSContext *ctx);
    virtual std::string appid() const;
    virtual std::string dataDir() const;
    virtual std::string name() const;
    virtual std::string appDir() const;

    inline void setAppid(const std::string &appid) { _appid = appid; }
    inline void setDataDir(const std::string &dataDir) { _dataDir = dataDir; }
    inline void setAppDir(const std::string &appDir) { _appDir = appDir; }

    static JQuick::sp<JQGlobalObjectEnv> Create(JSContext *ctx, const std::string &name);

protected:
    std::string _appid;
    std::string _dataDir;
    std::string _name;
    std::string _appDir;
};

class JQModuleEnv: public JQTemplateEnv {
public:
    JQModuleEnv(JSContext *ctx, JSModuleDef* def);

    virtual std::string appid() const;
    virtual std::string dataDir() const;
    virtual std::string name() const;
    virtual std::string appDir() const;

    JSModuleDef* moduleDef() const;

    void setModuleField(const std::string &name, JSValue obj);
    void setModuleExport(const std::string &name, JSValue obj, bool withFieldSet=true);
    void setModuleExportDone(JSValue module=JS_UNDEFINED, const std::vector<std::string> &exportList={});
    void setModuleExportFailed();

    static JQuick::sp<JQModuleEnv> CreateModule(JSContext *ctx, JSModuleDef* def, const std::string &moduleName);

protected:
    std::string _appid;
    std::string _dataDir;
    std::string _name;
    std::string _appDir;
    JSModuleDef *_def;

    // will be released when call setModuleExportDone, so do not need to mark gc
    std::map<std::string, JSValue> _collectModuleExportEntries;
};

}  // namespace JQUTIL_NS