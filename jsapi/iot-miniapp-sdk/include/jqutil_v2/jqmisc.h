#pragma once
#include "jqutil_v2/JQDefs.h"
#include "jqutil_v2/jqbson.h"
#include "looper/Task.h"
#include "quickjs/quickjs.h"
#include <functional>
#include <vector>

#ifndef offsetof
#define offsetof(type, field) ((size_t) &((type *)0)->field)
#endif
#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

namespace JQUTIL_NS {

void jq_dump_error1(JSContext* ctx, JSValueConst exception_val);
void jq_dump_error(JSContext* ctx);
int jq_eval_binary_with_exc(JSContext* ctx, const uint8_t* buf, size_t buf_len);

/* set the new value and free the old value after (freeing the value
   can reallocate the object data) */
inline void jq_set_value(JSContext *ctx, JSValue *pval, JSValue new_val)
{
    JSValue old_val;
    old_val = *pval;
    *pval = new_val;
    JS_FreeValue(ctx, old_val);
}

JSValue jq_call_return(JSContext* ctx, JSValueConst func, const std::vector<JSValueConst> &args={}, JSValueConst this_val=JS_UNDEFINED);
bool jq_call_void(JSContext *ctx, JSValueConst func, const std::vector<JSValueConst> &args={}, JSValueConst this_val=JS_UNDEFINED);
JSValue jq_bind_obj_func(JSContext *ctx, JSValueConst obj, const std::string &funcName, JSValueConst this_obj=JS_UNDEFINED);
bool jq_call_method_void(JSContext *ctx, JSValueConst obj, const std::string &methodName, const std::vector<JSValueConst> &args={});
JSValue jq_call_method_return(JSContext *ctx, JSValueConst obj, const std::string &methodName, const std::vector<JSValueConst> &args={});
void jq_define_property(JSContext *ctx, JSValueConst target, const std::string &sourceKey, const std::string &key);

JSValue bsonToJSValue(JSContext *ctx, const Bson& bson);
Bson JSValueToBson(JSContext *ctx, JSValueConst val);

JSValue mapToJSValue(JSContext* ctx, const std::map<std::string, std::string> &map);

JSValue getOrCreateFunc(JSContext *ctx, const char* funcName, const char* funcCode);
bool JQ_IsABOrABView(JSContext *ctx, JSValueConst val);
JSValue JQ_NewUint8Array(JSContext *ctx, JSValueConst arg0, JSValueConst arg1=JS_UNDEFINED, JSValueConst arg2=JS_UNDEFINED);
JSValue JQ_ToArrayBuffer(JSContext *ctx, JSValueConst value);

int jq_vsnprintf(char *outBuf, size_t size, const char* format, va_list ap);
void jq_printf_va(std::string &buf, const char* fmt, va_list ap);
std::string jq_printf(const char* fmt, ...);

class CtxHolder: public JQuick::REF_BASE {
public:
    CtxHolder(JSContext *ctx);
    ~CtxHolder();
    JSContext *ctx;
};
JQuick::wp<CtxHolder> getOrCreateCtxHolder(JSContext *ctx);

class JQStdFuncTask : public JQuick::Task
{
public:
    JQStdFuncTask(std::function<void()> func);
    virtual ~JQStdFuncTask();
    virtual void run();

private:
    std::function<void()> _func;
};

#define DEF_MODULE_LOAD_FUNC(NAME, INIT_FUNC) \
static JSModuleDef* NAME##_module_load(JSContext *ctx, const char *moduleName) { \
    if (strcmp(moduleName, #NAME) == 0) { \
        JSModuleDef *m = JS_NewCModule(ctx, moduleName, INIT_FUNC); \
        if (!m) return NULL; \
        JS_AddModuleExport(ctx, m, "default"); \
        return m; \
    } \
    return NULL; \
}

#define DEF_MODULE_LOAD_FUNC_EXPORT(NAME, INIT_FUNC, EXPORT_LIST) \
static JSModuleDef* NAME##_module_load(JSContext *ctx, const char *moduleName) { \
    if (strcmp(moduleName, #NAME) == 0) { \
        JSModuleDef *m = JS_NewCModule(ctx, moduleName, INIT_FUNC); \
        if (!m) return NULL; \
        JS_AddModuleExport(ctx, m, "default"); \
        for (auto &item: EXPORT_LIST) JS_AddModuleExport(ctx, m, item.c_str()); \
        return m; \
    } \
    return NULL; \
}

}  // namespace JQUTIL_NS

#define DEF_MODULE_EXPORT_FUNC(NAME) \
extern "C" JQUICK_EXPORT void custom_init_jsapis() { \
    registerCModuleLoader(#NAME, &NAME##_module_load); \
}

#define DEF_MODULE(NAME, INIT_FUNC) \
    DEF_MODULE_LOAD_FUNC(NAME, INIT_FUNC) \
    DEF_MODULE_EXPORT_FUNC(NAME)
