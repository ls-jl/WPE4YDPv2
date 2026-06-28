#include "jqutil_v2/JQIterObject.h"
#include "jqutil_v2/JQObjectTemplate.h"
#include "jqutil_v2/JQTemplateEnv.h"
#include "jqutil_v2/JQTypes.h"

namespace JQUTIL_NS {

static int16_t g_iter_magic = 0;

class IterNextInternalField: public JQuick::REF_BASE {
public:
    int kind;
    JSCFunctionListEntry iterator_funcs[1];
    KeepPtr<JQIterNextInterf> iterNext;
};

class TmpIterInternalField: public JQuick::REF_BASE {
public:
    int kind;
    KeepPtr<JQIterNextInterf> iterNext;
};

//virtual
JQObjectTemplateRef JQIterNextInterf::onGetIteratorTpl()
{
    return NULL;
}

JQIterObject::JQIterObject()
    :JQBaseObject()
{}

JQIterObject::~JQIterObject()
{}

// static
JQObjectTemplateRef JQIterObject::CreateTpl(JQTemplateEnvRef env)
{
    JQObjectTemplateRef objTpl = JQObjectTemplate::New(env);
    objTpl->setObjectCreator([]() {
        return new JQIterObject();
    });
    return objTpl;
}

JSValue _create_tmp_iter(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic, JSValue *func_data)
{
    JSValue iterator = JS_NewObject(ctx);
    JSValueConst target = func_data[0];

    JQBaseObject* cppObj = JQUnwrap<JQBaseObject>(target);
    assert(cppObj);
    JQInternalRefHolder *holder = cppObj->getOrCreateInternalRefHolder();
    assert(holder);
    std::string fieldKey = jq_printf("_jq_iter_magic_%d", magic);
    JQuick::sp<TmpIterInternalField> field = holder->getFieldAs<TmpIterInternalField>(fieldKey);
    holder->removeField(fieldKey);  // not use anymore

    JS_SetPropertyStr(ctx, iterator, "target", JS_DupValue(ctx, target));
    JQIterObject::SetIterator(ctx, target, field->iterNext, field->kind, iterator);
    return iterator;
}

// static
void JQIterObject::SetIterMethod(JSContext* ctx, JSValueConst target, const std::string &funcName,
                                 KeepPtr<JQIterNextInterf> iterNext, int kind/*=0*/)
{
    int16_t magic = ++g_iter_magic;
    TmpIterInternalField *field = new TmpIterInternalField();
    field->iterNext = iterNext;
    field->kind = kind;

    JQBaseObject* cppObj = JQUnwrap<JQBaseObject>(target);
    assert(cppObj);
    JQInternalRefHolder *holder = cppObj->getOrCreateInternalRefHolder();
    assert(holder);

    holder->setField(jq_printf("_jq_iter_magic_%d", magic), field);

    JS_SetPropertyStr(ctx, target, funcName.c_str(),
                      JS_NewCFunctionData(ctx, _create_tmp_iter, 0, magic, 1, &target));
}

// static
JSValue _create_iter_next(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int magic)
{
    JQBaseObject* cppObj = JQUnwrap<JQBaseObject>(this_val);
    if (cppObj == NULL) {
        // tmp iterator case
        JSValue target = JS_GetPropertyStr(ctx, this_val, "target");
        cppObj = JQUnwrap<JQBaseObject>(target);
        assert(cppObj);
        JS_FreeValue(ctx, target);
    }
    assert(cppObj);
    JQInternalRefHolder *holder = cppObj->getOrCreateInternalRefHolder();
    assert(holder);

    std::string fieldKey = jq_printf("_jq_iter_magic_%d", magic);
    JQuick::sp<IterNextInternalField> field = holder->getFieldAs<IterNextInternalField>(fieldKey);
    assert(field != NULL);

    JQObjectTemplateRef iterTpl = field->iterNext->onGetIteratorTpl();
    if (iterTpl == NULL) {
        iterTpl = JQIterObject::CreateTpl(cppObj->tplEnv());
    }

    JSValue result = iterTpl->NewInstance();
    JQIterObject *cppObjIter = JQUnwrap<JQIterObject>(result);
    cppObjIter->setTargetObject(ctx, this_val);
    cppObjIter->iterNext = field->iterNext;
    cppObjIter->kind = field->kind;
    field->iterNext.clear();  // not use anymore
    return result;
}

//static
void JQIterObject::SetIterator(JSContext *ctx, JSValueConst target, KeepPtr<JQIterNextInterf> iterNext,
                               int kind/*=0*/, JSValueConst iterator/*=JS_UNDEFINED*/)
{
    int16_t magic = ++g_iter_magic;
    IterNextInternalField *field = new IterNextInternalField();
    field->iterNext = iterNext;
    field->kind = kind;

    // some C++ compiler can not initialize struct in assignment, so use this tmp memcpy method.
    JSCFunctionListEntry iterator_funcs_tmp[1] = {
            JS_CFUNC_MAGIC_DEF("[Symbol.iterator]", 0, _create_iter_next, magic),
    };
    memcpy(field->iterator_funcs, iterator_funcs_tmp, sizeof(JSCFunctionListEntry));

    JQBaseObject* cppObj = JQUnwrap<JQBaseObject>(target);
    assert(cppObj);
    JQInternalRefHolder *holder = cppObj->getOrCreateInternalRefHolder();
    assert(holder);

    std::string fieldKey = jq_printf("_jq_iter_magic_%d", magic);
    holder->setField(fieldKey, field);

    // NOTE: cannot set twice, we do not check here
    if (JS_IsUndefined(iterator)) {
        // default case, target is the iterator
        JS_SetPropertyFunctionList(ctx, target, field->iterator_funcs, 1);
    } else {
        JS_SetPropertyFunctionList(ctx, iterator, field->iterator_funcs, 1);
    }
}

static JSValue _iter_next_func(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv, int* pdone, int magic)
{
    JQIterObject* cppObjIter = JQUnwrap<JQIterObject>(this_val);
    assert(cppObjIter);

    cppObjIter->done = true;
    JSValue value = cppObjIter->iterNext->onIteratorNext(*cppObjIter);

    *pdone = cppObjIter->done ?
            1: // TRUE
            0; // false
    return value;
}

static const JSCFunctionListEntry next_funcs[] = {
        JS_ITERATOR_NEXT_DEF("next", 0, _iter_next_func, 0),
};

void JQIterObject::OnInit()
{
    JSContext *ctx = getContext();
    JSValue selfJSValue = getJSValue();
    JQAtom next_prop(ctx, "next");
    if (0 == JS_GetOwnProperty(ctx, NULL, selfJSValue, next_prop.getAtom())) {
        JS_SetPropertyFunctionList(getContext(), getJSValue(), next_funcs, countof(next_funcs));
    }
}

// virtual
void JQIterObject::setTargetObject(JSContext *ctx, JSValueConst target)
{
    jq_set_value(ctx, &_target, JS_DupValue(ctx, target));
}

//virtual
void JQIterObject::OnGCCollect()
{
    JS_FreeValue(getContext(), _target);
}

//virtual
void JQIterObject::OnGCMark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func)
{
    JS_MarkValue(rt, _target, mark_func);
}

}  // namespace JQUTIL_NS