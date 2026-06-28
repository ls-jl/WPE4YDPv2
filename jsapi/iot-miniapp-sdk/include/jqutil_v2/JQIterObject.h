#pragma once
#include "jqutil_v2/JQDefs.h"
#include "jqutil_v2/JQBaseObject.h"
#include "jqutil_v2/JQKeepPtr.h"

namespace JQUTIL_NS {

// interface
class JQIterNextInterf {
public:
    JQIterNextInterf() {}
    virtual ~JQIterNextInterf() {}
    virtual JQObjectTemplateRef onGetIteratorTpl();
    virtual JSValue onIteratorNext(JQIterObject& iter) { return JS_UNDEFINED; }
};

class JQIterObject: public JQBaseObject {
public:
    JQIterObject();
    virtual ~JQIterObject();
    virtual void OnInit() override;
    virtual void OnGCCollect() override;
    virtual void OnGCMark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func) override;

    virtual void setTargetObject(JSContext *ctx, JSValueConst target);
    virtual JSValueConst getTargetObject() { return _target; }

    static JQObjectTemplateRef CreateTpl(JQTemplateEnvRef env);

    // set funcName function on target, which will return a iterator temp object, has a [Symbol.iterator] property setted
    static void SetIterMethod(JSContext* ctx, JSValueConst target, const std::string &funcName,
                              KeepPtr<JQIterNextInterf> iterNext, int kind=0);

    // set [Symbol.iterator] on target, creating a .next object to call back to iterNext interface
    static void SetIterator(JSContext *ctx, JSValueConst target, KeepPtr<JQIterNextInterf> iterNext,
                            int kind=0, JSValueConst iterator=JS_UNDEFINED);

public:
    int kind = 0;
    KeepPtr<JQIterNextInterf> iterNext;
    uint32_t index = 0;
    bool done = false;

protected:
    JSValue _target = JS_UNDEFINED;
};


}  // namespace JQUTIL_NS