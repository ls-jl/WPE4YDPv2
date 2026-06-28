#pragma once
#include "jqutil_v2/JQDefs.h"
#include "jqutil_v2/JQSerializer.h"
#include "jqutil_v2/JQBaseObject.h"
#include "quickjs/quickjs.h"
#include "utils/REF.h"
#include <string>
#include <vector>
#include <map>

namespace JQUTIL_NS {

class JQInternalValueHolder {
public:
    void onGCMark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func);
    void onGCCollect(JSContext* ctx);

    void setField(JSContext* ctx, const std::string &key, JSValue field);
    JSValueConst getField(const std::string &key) const;
    bool hasField(const std::string &key) const;
    bool removeField(JSContext *ctx, const std::string &key);
    void clear(JSContext *ctx);

    inline std::map<std::string, JSValue>& fields() { return _fields; }
    inline const std::map<std::string, JSValue>& fields() const { return _fields; }

protected:
    std::map<std::string, JSValue> _fields;
};

class JQInternalRefHolder {
public:
    void setField(const std::string &key, JQuick::REF_BASE* field);
    JQuick::REF_BASE* getField(const std::string &key);
    template<typename T>
    T* getFieldAs(const std::string &key) { return (T*)getField(key); }
    bool hasField(const std::string &key) const;
    bool removeField(const std::string &key);
    void clear();

    inline std::map<std::string, JQuick::sp<JQuick::REF_BASE>>& fields() { return _fields; }
    inline const std::map<std::string, JQuick::sp<JQuick::REF_BASE>>& fields() const { return _fields; }

protected:
    std::map<std::string, JQuick::sp<JQuick::REF_BASE>> _fields;
};

class JQObjectSignalRegister {
public:
    JQObjectSignalRegister(JQuick::wp<JQBaseObject> object);
    void onGCMark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func);
    void onGCCollect(JSContext* ctx);

    static JSValue _SignalSubscribe(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic, JSValueConst *refs);
    static JSValue _SignalUnsubscribe(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv, int magic, JSValueConst *refs);
    void _signalDoPublish(uint32_t prop, JSContext *ctx, std::vector<JSValueConst> &arr);

private:
    friend class JQBaseObject;
    friend class JQObjectTemplate;
    // signal property atom(as int) to signal listeners <token, listener>
    std::map<uint32_t/*property atom*/, std::vector<std::pair<uint32_t/*token*/, JSValue/*listener*/> > > _signalListenersMap;
    uint32_t _tokenId = 0;
    JQuick::wp<JQBaseObject> _object;
};

class JQObjectGCRegister {
public:
    void onGCMark(JQBaseObject *cppObj, JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func);
    void onGCCollect(JQBaseObject *cppObj, JSContext* ctx);
    void pushGCCollectHook(JQBaseObject::GCCollectHook hook);
    void pushGCMarkHook(JQBaseObject::GCMarkHook hook);

private:
    // for gc hooks
    std::vector< JQBaseObject::GCMarkHook > _gcMarkHooks;
    std::vector< JQBaseObject::GCCollectHook > _gcCollectHooks;
};

}  // namespace JQUTIL_NS
