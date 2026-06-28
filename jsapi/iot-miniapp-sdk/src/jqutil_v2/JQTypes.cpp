#include "jqutil_v2/JQTypes.h"

namespace JQUTIL_NS {

const char* JQValue::getTypeof()
{
    if (JS_IsBigInt(_ctx, _value)) {
        return "bigint";
    } else if (JS_IsBigFloat(_value)) {
        return "bigfloat";
    } else if (JS_IsBigDecimal(_value)) {
        return "bigdecimal";
    } else if (JS_IsNumber(_value)) {
        int tag = JS_VALUE_GET_TAG(_value);
        if (tag == JS_TAG_INT) {
            return "int";
        } else {  // JS_TAG_IS_FLOAT64(tag);
            return "float64";
        }
        // return "number"
    } else if (JS_IsUndefined(_value)) {
        return "undefined";
    } else if (JS_IsBool(_value)) {
        return "boolean";
    } else if (JS_IsString(_value)) {
        return "string";
    } else if (JS_IsObject(_value)) {
        if (JS_IsFunction(_ctx, _value)) {
            return "function";
        } else {
            return "object";
        }
    } else if (JS_IsNull(_value)) {
        return "null";
        // return "object"; // in js, typeof null == 'object'
    } else if (JS_IsSymbol(_value)) {
        return "symbol";
    } else {
        return "unknown";
    }
}

JQArrayBufferView::JQArrayBufferView(JSContext *ctx, JSValueConst value)
    : JQValue(ctx, value), _length(0), _offset(0), _bytes_per_element(0),
    _array_buffer(JS_UNDEFINED), _data(nullptr), _data_size(0)
{
    _array_buffer = JS_GetTypedArrayBuffer(_ctx, _value, &_offset, &_length, &_bytes_per_element);
    if (JS_IsException(_array_buffer)) {
        // may be an arrayBuffer, use default case
        _offset = 0;
        _bytes_per_element = 1;
        _array_buffer = JQ_ToArrayBuffer(ctx, _value);
        _data = JS_GetArrayBuffer(ctx, &_data_size, _array_buffer);
        _length = _data_size;
        JS_ResetUncatchableError(ctx);
    } else {
        _data = JS_GetArrayBuffer(ctx, &_data_size, _array_buffer);
    }
}
JQArrayBufferView::~JQArrayBufferView()
{
    JS_FreeValue(_ctx, _array_buffer);
}

JQObject::~JQObject()
{
    _clearKeyValueMap();
}

void JQObject::_clearKeyValueMap()
{
    for (auto &iter: _keyValueMap) {
        JS_FreeValue(_ctx, iter.second);
    }
    _keyValueMap.clear();
}

bool JQObject::_tryExtract()
{
    bool result;
    uint32_t len, i;
    JSPropertyEnum *tab;
    const char *key;
    JSValue val;

    if (JS_GetOwnPropertyNames(_ctx, &tab, &len, _value,
                               JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0) {
        return false;
    }

    for(i = 0; i < len; i++) {
        key = JS_AtomToCString(_ctx, tab[i].atom);
        if (!key) {
            goto fail;
        }

        if (_keyValueMap.find(key) != _keyValueMap.end()) {
            JS_FreeCString(_ctx, key);
            continue;
        }

        val = JS_GetProperty(_ctx, _value, tab[i].atom);
        if (JS_IsException(val)) {
            JS_FreeCString(_ctx, key);
            goto fail;
        }

        // val will be hold
        _keyValueMap[key] = val;
        JS_FreeCString(_ctx, key);
    }

    result = true;
done:
    for(i = 0; i < len; i++)
        JS_FreeAtom(_ctx, tab[i].atom);
    js_free(_ctx, tab);
    return result;
fail:
    result = false;
    goto done;
}

bool JQObject::getBool(const std::string &prop)
{
    JSValueConst val = getValue(prop);
    if (JS_IsException(val)) return false;
    // NOTE: -1 is exception, treating as false
    return JS_ToBool(_ctx, val) == 1;
}

JSValueConst JQObject::getValue(const std::string &prop)
{
    auto iter = _keyValueMap.find(prop);
    if (iter != _keyValueMap.end()) return iter->second;
    JSValue val = JS_GetPropertyStr(_ctx, _value, prop.c_str());
    _keyValueMap[prop] = val;
    return val;
}

std::string JQObject::getString(const std::string &prop)
{
    JSValueConst val = getValue(prop);
    size_t len;
    const char* str = JS_ToCStringLen(_ctx, &len, val);
    if (!str) return "";
    std::string result = std::string(str, len);
    JS_FreeCString(_ctx, str);
    return result;
}

}  // namespace JQUTIL_NS