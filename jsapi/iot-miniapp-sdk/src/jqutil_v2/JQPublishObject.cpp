#include "jqutil_v2/JQPublishObject.h"
#include "jqutil_v2/JQTypes.h"
#include "jqutil_v2/jqbson.h"
#include "jqutil_v2/jqmisc.h"
#include "utils/Functional.h"
#include "utils/log.h"

namespace JQUTIL_NS {

JQPublishObject::JQPublishObject()
    : _pubCbTokenId(0)
{}

void JQPublishObject::OnInit()
{
    hookGCMark([this](JQBaseObject*, JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func) {
      for (auto &iter0: _topicCallbacksMap) {
          for (auto &iter1: iter0.second) {
              JS_MarkValue(rt, iter1.second, mark_func);
          }
      }
    });
    hookGCCollect([this](JQBaseObject*, JSContext *ctx) {
      for (auto &iter0: _topicCallbacksMap) {
          for (auto &iter1: iter0.second) {
              JS_FreeValue(ctx, iter1.second);
          }
      }
      _topicCallbacksMap.clear();
    });
}

// static
void JQPublishObject::InitTpl(JQFunctionTemplateRef &tpl)
{
    tpl->SetProtoMethod("on", &JQPublishObject::_SubscribeTopic);
    tpl->SetProtoMethod("off", &JQPublishObject::_UnsubscribeTopic);
}

// static
void JQPublishObject::InitTpl(JQObjectTemplateRef &tpl)
{
    tpl->Set("on", &JQPublishObject::_SubscribeTopic);
    tpl->Set("off", &JQPublishObject::_UnsubscribeTopic);
}

void JQPublishObject::_SubscribeTopic(JQFunctionInfo &info)
{
    JSContext *ctx = info.GetContext();
    JQString topic(ctx, info[0]);
    if (!topic.isString()) {
        info.GetReturnValue().ThrowTypeError("arg0 should be topic as string type");
        return;
    }
    JQFunction callback(ctx, info[1]);
    if (!callback.isFunction()) {
        info.GetReturnValue().ThrowTypeError("arg1 should be callback as function type");
    }

    std::vector<std::pair<uint32_t, JSValue> > &callbacks = _topicCallbacksMap[topic.get()];
    uint32_t token = ++_pubCbTokenId;
    callbacks.push_back(std::make_pair(token, JS_DupValue(ctx, callback.value())));
    info.GetReturnValue().Set(token);
    onSubscribe(topic.get());
}

// virtual
void JQPublishObject::onSubscribe(const char* topic)
{}

// virtual
void JQPublishObject::onUnsubscribe(const char* topic)
{}

void JQPublishObject::_UnsubscribeTopic(JQFunctionInfo &info)
{
    JSContext *ctx = info.GetContext();

    // if none args, turning off all callbacks
    if (info.Length() == 0) {
        for (auto &iter0: _topicCallbacksMap) {
            for (auto &iter1: iter0.second) {
                JS_FreeValue(ctx, iter1.second);
            }
            onUnsubscribe(iter0.first.c_str());
        }
        _topicCallbacksMap.clear();
        return;
    }

    // only token case
    if (JS_IsNumber(info[0])) {
        // only token to unsubscribe
        JQNumber tokenObj(ctx, info[0]);
        uint32_t token = tokenObj.getUint32();
        std::vector< std::pair< uint32_t, JSValue > >::iterator iter;
        for (auto &iter0: _topicCallbacksMap) {
            for (iter = iter0.second.begin(); iter != iter0.second.end(); iter++) {
                if (iter->first == token) {
                    break;
                }
            }
            if (iter != iter0.second.end()) {
                // found
                JS_FreeValue(ctx, iter->second);
                iter0.second.erase(iter);
                break;
            }
        }
        return;
    }

    JQString topic(ctx, info[0]);
    if (!topic.isString()) {
        // do not throw error, to avoid break other user logic outside
        LOGE("arg0 should be topic as string type");
        return;
    }

    std::vector< std::pair< uint32_t, JSValue > >& callbacks = _topicCallbacksMap[topic.get()];
    std::vector< std::pair< uint32_t, JSValue > >::iterator iter = callbacks.end();

    // only topic case
    if (info.Length() == 1) {
        for (auto &iter: callbacks) {
            JS_FreeValue(ctx, iter.second);
        }
        callbacks.clear();
        onUnsubscribe(topic.get());
        return;
    }

    // (topic, token/cb) case
    JSValueConst tokenOrCb = info[1];
    if (!JS_IsFunction(ctx, tokenOrCb) && !JS_IsNumber(tokenOrCb)) {
        // do not throw error, to avoid break other user logic outside
        LOGE("arg1 should be token or function");
        return;
    }

    if (JS_IsNumber(tokenOrCb)) {
        JQNumber tokenObj(ctx, tokenOrCb);
        uint32_t token = tokenObj.getUint32();
        for (iter=callbacks.begin(); iter!=callbacks.end(); iter++) {
            if (iter->first == token) {
                break;  // found
            }
        }
    } else if (JS_IsFunction(ctx, tokenOrCb)) {
        for (iter=callbacks.begin(); iter!=callbacks.end(); iter++) {
            if (JS_VALUE_GET_PTR(iter->second) == JS_VALUE_GET_PTR(tokenOrCb)) {
                break;  // found
            }
        }
    }

    if (iter != callbacks.end()) {
        JS_FreeValue(ctx, iter->second);
        callbacks.erase(iter);
        onUnsubscribe(topic.get());
    }
}

// run js thread
void JQPublishObject::_OnPublishJSON(const std::string &topic, const std::string &json)
{
    if (!isAttached()) {
        return;
    }

    JSValue ret, cb, jjson;
    std::vector<JSValue> cbs;
    // find callback
    auto iter = _topicCallbacksMap.find(topic);
    if (iter == _topicCallbacksMap.end()) {
        return;
    }

    JSContext* ctx = JS_DupContext(getContext());
    // json.parse(json), then call callback
    jjson = JS_ParseJSON(ctx, json.c_str(), json.size(), "_OnPublishJSON.json");
    if (JS_IsException(jjson)) {
        jq_dump_error(ctx);
        goto exit;
    }

    for (auto cbIter: iter->second) {
        cbs.push_back(JS_DupValue(ctx, cbIter.second));
    }
    for (auto cb: cbs) {
        // XXX: here json maybe modified by some callback, should be sealed?
        ret = JS_Call(ctx, cb, JS_UNDEFINED, 1, &jjson);
        if (JS_IsException(ret)) {
            jq_dump_error(ctx);
            goto exit;
        }
        JS_FreeValue(ctx, ret);
    }
exit:
    for (auto cb: cbs) {
        JS_FreeValue(ctx, cb);
    }
    JS_FreeValue(ctx, jjson);
    JS_FreeContext(ctx);
}

// run js thread
void JQPublishObject::_OnPublish(const std::string &topic, const Bson &bson)
{
    if (!isAttached()) {
        return;
    }

    JSValue ret, cb, jsval;
    std::vector<JSValue> cbs;
    // find callback
    auto iter = _topicCallbacksMap.find(topic);
    if (iter == _topicCallbacksMap.end()) {
        return;
    }

    JSContext* ctx = JS_DupContext(getContext());

    jsval = bsonToJSValue(ctx, bson);

    if (JS_IsException(jsval)) {
        jq_dump_error(ctx);
        goto exit;
    }
    for (auto cbIter: iter->second) {
        cbs.push_back(JS_DupValue(ctx, cbIter.second));
    }
    for (auto cb: cbs) {
        // XXX: here jsval maybe modified by some callback, should be sealed?
        ret = JS_Call(ctx, cb, JS_UNDEFINED, 1, &jsval);
        if (JS_IsException(ret)) {
            jq_dump_error(ctx);
            goto exit;
        }
        JS_FreeValue(ctx, ret);
    }
exit:
    for (auto cb: cbs) {
        JS_FreeValue(ctx, cb);
    }
    JS_FreeValue(ctx, jsval);
    JS_FreeContext(ctx);
}

void JQPublishObject::publishJSON(const std::string &topic, const std::string &json, JQPublishType pubType/*=JQ_PUBLISH_TYPE_AUTO*/)
{
    bool sync = false;
    if (pubType == JQ_PUBLISH_TYPE_AUTO) {
        sync = inSameThread();
    } else if (pubType == JQ_PUBLISH_TYPE_ASYNC) {
        sync = false;
    } else if (pubType == JQ_PUBLISH_TYPE_SYNC){
        sync = true;
    }

    if (sync) {
        _OnPublishJSON(topic, json);
    } else {
        jsHandler()->run(new JQuick::FunctionalTask(JQuick::bind(&JQPublishObject::_OnPublishJSON, JQuick::sp<JQPublishObject>(this), topic, json)));
    }
}

void JQPublishObject::publish(const std::string &topic, const Bson &bson, JQPublishType pubType/*=JQ_PUBLISH_TYPE_AUTO*/)
{
    bool sync = false;
    if (pubType == JQ_PUBLISH_TYPE_AUTO) {
        sync = inSameThread();
    } else if (pubType == JQ_PUBLISH_TYPE_ASYNC) {
        sync = false;
    } else if (pubType == JQ_PUBLISH_TYPE_SYNC){
        sync = true;
    }

    if (sync) {
        _OnPublish(topic, bson);
    } else {
        jsHandler()->run(new JQuick::FunctionalTask(JQuick::bind(&JQPublishObject::_OnPublish, JQuick::sp<JQPublishObject>(this), topic, bson)));
    }
}

}  // namespace JQUTIL_NS