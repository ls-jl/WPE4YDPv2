#include "jqutil_v2/jqutil.h"
#include "jsmodules/JSCModuleExtension.h"
#include "jquick_config.h"

using namespace JQUTIL_NS;

namespace browser {

extern void browser_init(JQModuleEnv* env);

static std::vector<std::string> exportList = {
    "browserPlayer"
};

static int module_init(JSContext *ctx, JSModuleDef *m)
{
    JQuick::sp<JQModuleEnv> env = JQModuleEnv::CreateModule(ctx, m, "browser");
    browser_init(env.get());
    env->setModuleExportDone(JS_UNDEFINED, exportList);
    return 0;
}

DEF_MODULE_LOAD_FUNC_EXPORT(browser, module_init, exportList)

}  // namespace browser

extern "C" JQUICK_EXPORT void custom_init_jsapis()
{
    registerCModuleLoader("browser", &browser::browser_module_load);
}
