#pragma once
#include "jqutil_v2/JQDefs.h"
#include "utils/REF.h"
#include <functional>

#define JQUTIL_NS jqutil_dist

namespace JQUTIL_NS {

template <typename T>
using JQRef = JQuick::sp<T>;
template <typename T>
using JQWRef = JQuick::wp<T>;

class JQFunctionTemplate;
class JQObjectProperty;
class JQObjectTemplate;
class JQBaseObject;
class JQIterObject;
class JQAsyncExecutor;
class JQAsyncSchedule;

class JQTemplateEnv;
using JQTemplateEnvRef = JQuick::sp<JQTemplateEnv>;
class JQGlobalObjectEnv;
class JQModuleEnv;

class JQObjectTemplate;
using JQObjectTemplateRef = JQRef< JQObjectTemplate >;

class JQFunctionTemplate;
using JQFunctionTemplateRef = JQRef< JQFunctionTemplate >;

class JQInternalValueHolder;
class JQInternalRefHolder;
class JQObjectSignalRegister;

class JQFunctionInfo;
class JQAsyncInfo;
class JQJSThreadInfo;
using JQJSThreadCallback = std::function<void(JQJSThreadInfo&)>;

typedef void JQFunctionCallback (JQFunctionInfo &);
typedef void JQAsyncCallback (JQAsyncInfo &);

using JQFunctionCallbackType = std::function<JQFunctionCallback >;
using JQAsyncCallbackType = std::function<JQAsyncCallback>;

}  // namespace JQUTIL_NS
