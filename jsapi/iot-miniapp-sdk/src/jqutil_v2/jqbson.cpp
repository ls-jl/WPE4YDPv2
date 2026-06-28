/* Copyright (c) 2013 Dropbox, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "jqutil_v2/jqbson.h"
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <limits>

namespace JQUTIL_NS {

static const int max_depth = 200;

using std::map;
using std::string;
using std::vector;
//using std::make_shared;
//using std::initializer_list;
//using std::move;

/* Helper for representing null - just a do-nothing struct, plus comparison
 * operators so the helpers in BsonValue work. We can't use NULL_t because
 * it may not be orderable.
 */
struct NullStruct {
    bool operator==(NullStruct) const
    {
        return true;
    }
    bool operator<(NullStruct) const
    {
        return false;
    }
};

/* * * * * * * * * * * * * * * * * * * *
 * Serialization
 */

static void dump(NullStruct, string& out)
{
    out += "null";
}

static void dump(double value, string& out)
{
    if (std::isfinite(value)) {
        char buf[32];
        snprintf(buf, sizeof buf, "%.17g", value);
        out += buf;
    } else {
        out += "null";
    }
}

static void dump(int value, string& out)
{
    char buf[32];
    snprintf(buf, sizeof buf, "%d", value);
    out += buf;
}

static void dump(bool value, string& out)
{
    out += value ? "true" : "false";
}

static void dump(const string& value, string& out)
{
    out += '"';
    for (size_t i = 0; i < value.length(); i++) {
        const char ch = value[i];
        if (ch == '\\') {
            out += "\\\\";
        } else if (ch == '"') {
            out += "\\\"";
        } else if (ch == '\b') {
            out += "\\b";
        } else if (ch == '\f') {
            out += "\\f";
        } else if (ch == '\n') {
            out += "\\n";
        } else if (ch == '\r') {
            out += "\\r";
        } else if (ch == '\t') {
            out += "\\t";
        } else if (static_cast< uint8_t >(ch) <= 0x1f) {
            char buf[8];
            snprintf(buf, sizeof buf, "\\u%04x", ch);
            out += buf;
        } else if (static_cast< uint8_t >(ch) == 0xe2 && static_cast< uint8_t >(value[i + 1]) == 0x80 && static_cast< uint8_t >(value[i + 2]) == 0xa8) {
            out += "\\u2028";
            i += 2;
        } else if (static_cast< uint8_t >(ch) == 0xe2 && static_cast< uint8_t >(value[i + 1]) == 0x80 && static_cast< uint8_t >(value[i + 2]) == 0xa9) {
            out += "\\u2029";
            i += 2;
        } else {
            out += ch;
        }
    }
    out += '"';
}

static void dump(const Bson::array& values, string& out)
{
    bool first = true;
    out += "[";

    for (unsigned i = 0; i < values.size(); i++) {
        const Bson& value = values[i];

        if (!first) {
            out += ", ";
        }
        value.dump(out);
        first = false;
    }

    out += "]";
}

static void dump(const Bson::object& values, string& out)
{
    bool first = true;
    out += "{";

    for (std::map< std::string, Bson >::const_iterator iter = values.begin(); iter != values.end(); iter++) {
        if (!first) {
            out += ", ";
        }

        dump(iter->first, out);
        out += ": ";
        iter->second.dump(out);
        first = false;
    }

    out += "}";
}

static void dump(const Bson::binary& value, string& out)
{
    // TODO: b64'xxxx' ?
    out += "null";
}

void Bson::dump(string& out) const
{
    m_ptr->dump(out);
}

/* * * * * * * * * * * * * * * * * * * *
 * Value wrappers
 */

template < Bson::Type tag, typename T >
class Value : public BsonValue
{  //!OCLint
protected:
    // Constructors
    explicit Value(const T& value) :
            m_value(value) {}

    // Get type tag
    Bson::Type type() const
    {
        return tag;
    }

    // Comparisons
    bool equals(const BsonValue* other) const
    {
        return m_value == static_cast< const Value< tag, T >* >(other)->m_value;
    }
    bool less(const BsonValue* other) const
    {
        return m_value < static_cast< const Value< tag, T >* >(other)->m_value;
    }

    const T m_value;
    void dump(string& out) const
    {
        JQUTIL_NS::dump(m_value, out);
    }
};

class BsonDouble : public Value< Bson::DOUBLE, double >
{
    double number_value() const
    {
        return m_value;
    }
    double double_value() const
    {
        return m_value;
    }
    int int_value() const
    {
        return static_cast< int >(m_value);
    }
    bool equals(const BsonValue* other) const
    {
        return m_value == other->number_value();
    }
    bool less(const BsonValue* other) const
    {
        return m_value < other->number_value();
    }

public:
    explicit BsonDouble(double value) :
            Value(value) {}
};

class BsonInt : public Value< Bson::INT, int >
{
    double number_value() const
    {
        return m_value;
    }
    double double_value() const
    {
        return m_value;
    }
    int int_value() const
    {
        return m_value;
    }
    bool equals(const BsonValue* other) const
    {
        return m_value == other->number_value();
    }
    bool less(const BsonValue* other) const
    {
        return m_value < other->number_value();
    }

public:
    explicit BsonInt(int value) :
            Value(value) {}
};

class BsonBoolean : public Value< Bson::BOOL, bool >
{
    bool bool_value() const
    {
        return m_value;
    }

public:
    explicit BsonBoolean(bool value) :
            Value(value) {}
};

class BsonString : public Value< Bson::STRING, string >
{
    const string& string_value() const
    {
        return m_value;
    }

public:
    explicit BsonString(const string& value) :
            Value(value) {}
};

class BsonArray : public Value< Bson::ARRAY, Bson::array >
{
    const Bson::array& array_items() const
    {
        return m_value;
    }
    const Bson& operator[](size_t i) const;

public:
    explicit BsonArray(const Bson::array& value) :
            Value(value) {}
};

class BsonObject : public Value< Bson::OBJECT, Bson::object >
{
    const Bson::object& object_items() const
    {
        return m_value;
    }
    const Bson& operator[](const string& key) const;

public:
    explicit BsonObject(const Bson::object& value) :
            Value(value) {}
};

class BsonNull : public Value< Bson::NUL, NullStruct >
{
public:
    BsonNull() :
            Value(NullStruct()) {}
};

class BsonBinary: public Value< Bson::BINARY, Bson::binary >
{
    const Bson::binary& binary_value() const
    {
        return m_value;
    }

public:
    explicit BsonBinary(const Bson::binary& value) :
            Value(value) {}
};

/* * * * * * * * * * * * * * * * * * * *
 * Static globals - static-init-safe
 */
struct Statics {
    //    const std::shared_ptr<BsonValue> null = make_shared<BsonNull>();
    //    const std::shared_ptr<BsonValue> t = make_shared<BsonBoolean>(true);
    //    const std::shared_ptr<BsonValue> f = make_shared<BsonBoolean>(false);
    const static JQuick::sp< BsonValue > null;
    const static JQuick::sp< BsonValue > t;
    const static JQuick::sp< BsonValue > f;
    const string empty_string;
    const vector< Bson > empty_vector;
    const map< string, Bson > empty_map;
    const vector< uint8_t > empty_binary;
    Statics() {}
};

const JQuick::sp< BsonValue > Statics::null = new BsonNull;
const JQuick::sp< BsonValue > Statics::t = new BsonBoolean(true);
const JQuick::sp< BsonValue > Statics::f = new BsonBoolean(false);

static const Statics& statics()
{
    static const Statics* s = new Statics();
    return *s;
}

static const Bson& static_null()
{
    // This has to be separate, not in Statics, because Bson() accesses statics().null.
    static const Bson* json_null = new Bson();
    return *json_null;
}

/* * * * * * * * * * * * * * * * * * * *
 * Constructors
 */

Bson::Bson() :
        m_ptr(statics().null) {}
Bson::Bson(double value) :
        m_ptr(new BsonDouble(value)) {}
Bson::Bson(int value) :
        m_ptr(new BsonInt(value)) {}
Bson::Bson(bool value) :
        m_ptr(value ? statics().t : statics().f) {}
Bson::Bson(const string& value) :
        m_ptr(new BsonString(value)) {}
Bson::Bson(const char* value) :
        m_ptr(new BsonString(value)) {}
Bson::Bson(const Bson::array& values) :
        m_ptr(new BsonArray(values)) {}
Bson::Bson(const Bson::object& values) :
        m_ptr(new BsonObject(values)) {}
Bson::Bson(const Bson::binary& value) :
        m_ptr(new BsonBinary(value)) {}

/* * * * * * * * * * * * * * * * * * * *
 * Accessors
 */

Bson::Type Bson::type() const
{
    return m_ptr->type();
}
double Bson::number_value() const
{
    return m_ptr->number_value();
}
double Bson::double_value() const
{
    return m_ptr->double_value();
}
int Bson::int_value() const
{
    return m_ptr->int_value();
}
bool Bson::bool_value() const
{
    return m_ptr->bool_value();
}
const string& Bson::string_value() const
{
    return m_ptr->string_value();
}
const vector< Bson >& Bson::array_items() const
{
    return m_ptr->array_items();
}
const map< string, Bson >& Bson::object_items() const
{
    return m_ptr->object_items();
}
const Bson& Bson::operator[](size_t i) const
{
    return (*m_ptr)[i];
}
const Bson& Bson::operator[](const string& key) const
{
    return (*m_ptr)[key];
}
const Bson::binary& Bson::binary_value() const
{
    return m_ptr->binary_value();
}

double BsonValue::number_value() const
{
    return 0;
}
double BsonValue::double_value() const
{
    return 0;
}
int BsonValue::int_value() const
{
    return 0;
}
bool BsonValue::bool_value() const
{
    return false;
}
const string& BsonValue::string_value() const
{
    return statics().empty_string;
}
const vector< Bson >& BsonValue::array_items() const
{
    return statics().empty_vector;
}
const map< string, Bson >& BsonValue::object_items() const
{
    return statics().empty_map;
}
const Bson& BsonValue::operator[](size_t) const
{
    return static_null();
}
const Bson& BsonValue::operator[](const string&) const
{
    return static_null();
}
const std::vector<uint8_t>& BsonValue::binary_value() const
{
    return statics().empty_binary;
}

const Bson& BsonObject::operator[](const string& key) const
{
    Bson::object::const_iterator iter = m_value.find(key);
    return (iter == m_value.end()) ? static_null() : iter->second;
}
const Bson& BsonArray::operator[](size_t i) const
{
    if (i >= m_value.size())
        return static_null();
    else
        return m_value[i];
}

/* * * * * * * * * * * * * * * * * * * *
 * Comparison
 */

bool Bson::operator==(const Bson& other) const
{
    if (m_ptr == other.m_ptr)
        return true;
    if (m_ptr->type() != other.m_ptr->type())
        return false;

    return m_ptr->equals(other.m_ptr.get());
}

bool Bson::operator<(const Bson& other) const
{
    if (m_ptr == other.m_ptr)
        return false;
    if (m_ptr->type() != other.m_ptr->type())
        return m_ptr->type() < other.m_ptr->type();

    return m_ptr->less(other.m_ptr.get());
}

/* * * * * * * * * * * * * * * * * * * *
 * Parsing
 */

/* esc(c)
 *
 * Format char c suitable for printing in an error message.
 */
static inline string esc(char c)
{
    char buf[12];
    if (static_cast< uint8_t >(c) >= 0x20 && static_cast< uint8_t >(c) <= 0x7f) {
        snprintf(buf, sizeof buf, "'%c' (%d)", c, c);
    } else {
        snprintf(buf, sizeof buf, "(%d)", c);
    }
    return string(buf);
}

static inline bool in_range(long x, long lower, long upper)
{
    return (x >= lower && x <= upper);
}

// namespace {
/* BsonParser
 *
 * Object that tracks all state of an in-progress parse.
 */
struct BsonParser {
    /* State
     */
    const string& str;
    size_t i;
    string& err;
    bool failed;
    const BsonParse strategy;

    /* fail(msg, err_ret = Bson())
     *
     * Mark this parse as failed.
     */
    Bson fail(const string& msg)
    {
        return fail(msg, Bson());
    }

    template < typename T >
    T fail(const string& msg, const T err_ret)
    {
        if (!failed)
            err = msg;
        failed = true;
        return err_ret;
    }

    /* consume_whitespace()
     *
     * Advance until the current character is non-whitespace.
     */
    void consume_whitespace()
    {
        while (str[i] == ' ' || str[i] == '\r' || str[i] == '\n' || str[i] == '\t')
            i++;
    }

    /* consume_comment()
     *
     * Advance comments (c-style inline and multiline).
     */
    bool consume_comment()
    {
        bool comment_found = false;
        if (str[i] == '/') {
            i++;
            if (i == str.size())
                return fail("unexpected end of input after start of comment", false);
            if (str[i] == '/') {  // inline comment
                i++;
                // advance until next line, or end of input
                while (i < str.size() && str[i] != '\n') {
                    i++;
                }
                comment_found = true;
            } else if (str[i] == '*') {  // multiline comment
                i++;
                if (i > str.size() - 2)
                    return fail("unexpected end of input inside multi-line comment", false);
                // advance until closing tokens
                while (!(str[i] == '*' && str[i + 1] == '/')) {
                    i++;
                    if (i > str.size() - 2)
                        return fail(
                                "unexpected end of input inside multi-line comment", false);
                }
                i += 2;
                comment_found = true;
            } else
                return fail("malformed comment", false);
        }
        return comment_found;
    }

    /* consume_garbage()
     *
     * Advance until the current character is non-whitespace and non-comment.
     */
    void consume_garbage()
    {
        consume_whitespace();
        if (strategy == COMMENTS) {
            bool comment_found = false;
            do {
                comment_found = consume_comment();
                if (failed)
                    return;
                consume_whitespace();
            } while (comment_found);  //!OCLint
        }
    }

    /* get_next_token()
     *
     * Return the next non-whitespace character. If the end of the input is reached,
     * flag an error and return 0.
     */
    char get_next_token()
    {
        consume_garbage();
        if (failed)
            return (char)0;
        if (i == str.size())
            return fail("unexpected end of input", (char)0);

        return str[i++];
    }

    /* encode_utf8(pt, out)
     *
     * Encode pt as UTF-8 and add it to out.
     */
    void encode_utf8(long pt, string& out)
    {
        if (pt < 0)
            return;

        if (pt < 0x80) {
            out += static_cast< char >(pt);
        } else if (pt < 0x800) {
            out += static_cast< char >((pt >> 6) | 0xC0);
            out += static_cast< char >((pt & 0x3F) | 0x80);
        } else if (pt < 0x10000) {
            out += static_cast< char >((pt >> 12) | 0xE0);
            out += static_cast< char >(((pt >> 6) & 0x3F) | 0x80);
            out += static_cast< char >((pt & 0x3F) | 0x80);
        } else {
            out += static_cast< char >((pt >> 18) | 0xF0);
            out += static_cast< char >(((pt >> 12) & 0x3F) | 0x80);
            out += static_cast< char >(((pt >> 6) & 0x3F) | 0x80);
            out += static_cast< char >((pt & 0x3F) | 0x80);
        }
    }

    /* parse_string()
     *
     * Parse a string, starting at the current position.
     */
    string parse_string()
    {
        string out;
        long last_escaped_codepoint = -1;
        while (true) {
            if (i == str.size())
                return fail("unexpected end of input in string", "");

            char ch = str[i++];

            if (ch == '"') {
                encode_utf8(last_escaped_codepoint, out);
                return out;
            }

            if (in_range(ch, 0, 0x1f))
                return fail("unescaped " + esc(ch) + " in string", "");

            // The usual case: non-escaped characters
            if (ch != '\\') {
                encode_utf8(last_escaped_codepoint, out);
                last_escaped_codepoint = -1;
                out += ch;
                continue;
            }

            // Handle escapes
            if (i == str.size())
                return fail("unexpected end of input in string", "");

            ch = str[i++];

            if (ch == 'u') {
                // Extract 4-byte escape sequence
                string esc = str.substr(i, 4);
                // Explicitly check length of the substring. The following loop
                // relies on std::string returning the terminating NUL when
                // accessing str[length]. Checking here reduces brittleness.
                if (esc.length() < 4) {
                    return fail("bad \\u escape: " + esc, "");
                }
                for (size_t j = 0; j < 4; j++) {
                    if (!in_range(esc[j], 'a', 'f') && !in_range(esc[j], 'A', 'F') && !in_range(esc[j], '0', '9'))
                        return fail("bad \\u escape: " + esc, "");
                }

                long codepoint = strtol(esc.data(), NULL, 16);

                // JSON specifies that characters outside the BMP shall be encoded as a pair
                // of 4-hex-digit \u escapes encoding their surrogate pair components. Check
                // whether we're in the middle of such a beast: the previous codepoint was an
                // escaped lead (high) surrogate, and this is a trail (low) surrogate.
                if (in_range(last_escaped_codepoint, 0xD800, 0xDBFF) && in_range(codepoint, 0xDC00, 0xDFFF)) {
                    // Reassemble the two surrogate pairs into one astral-plane character, per
                    // the UTF-16 algorithm.
                    encode_utf8((((last_escaped_codepoint - 0xD800) << 10) | (codepoint - 0xDC00)) + 0x10000, out);
                    last_escaped_codepoint = -1;
                } else {
                    encode_utf8(last_escaped_codepoint, out);
                    last_escaped_codepoint = codepoint;
                }

                i += 4;
                continue;
            }

            encode_utf8(last_escaped_codepoint, out);
            last_escaped_codepoint = -1;

            if (ch == 'b') {
                out += '\b';
            } else if (ch == 'f') {
                out += '\f';
            } else if (ch == 'n') {
                out += '\n';
            } else if (ch == 'r') {
                out += '\r';
            } else if (ch == 't') {
                out += '\t';
            } else if (ch == '"' || ch == '\\' || ch == '/') {
                out += ch;
            } else {
                return fail("invalid escape character " + esc(ch), "");
            }
        }
    }

    /* parse_number()
     *
     * Parse a double.
     */
    Bson parse_number()
    {
        size_t start_pos = i;

        if (str[i] == '-')
            i++;

        // Integer part
        if (str[i] == '0') {
            i++;
            if (in_range(str[i], '0', '9'))
                return fail("leading 0s not permitted in numbers");
        } else if (in_range(str[i], '1', '9')) {
            i++;
            while (in_range(str[i], '0', '9'))
                i++;
        } else {
            return fail("invalid " + esc(str[i]) + " in number");
        }

        if (str[i] != '.' && str[i] != 'e' && str[i] != 'E' && (i - start_pos) <= static_cast< size_t >(std::numeric_limits< int >::digits10)) {
            return std::atoi(str.c_str() + start_pos);
        }

        // Decimal part
        if (str[i] == '.') {
            i++;
            if (!in_range(str[i], '0', '9'))
                return fail("at least one digit required in fractional part");

            while (in_range(str[i], '0', '9'))
                i++;
        }

        // Exponent part
        if (str[i] == 'e' || str[i] == 'E') {
            i++;

            if (str[i] == '+' || str[i] == '-')
                i++;

            if (!in_range(str[i], '0', '9'))
                return fail("at least one digit required in exponent");

            while (in_range(str[i], '0', '9'))
                i++;
        }

        return std::strtod(str.c_str() + start_pos, NULL);
    }

    /* expect(str, res)
     *
     * Expect that 'str' starts at the character that was just read. If it does, advance
     * the input and return res. If not, flag an error.
     */
    Bson expect(const string& expected, Bson res)
    {
        assert(i != 0);
        i--;
        if (str.compare(i, expected.length(), expected) == 0) {
            i += expected.length();
            return res;
        } else {
            return fail("parse error: expected " + expected + ", got " + str.substr(i, expected.length()));
        }
    }

    /* parse_json()
     *
     * Parse a JSON object.
     */
    Bson parse_json(int depth)
    {
        if (depth > max_depth) {
            return fail("exceeded maximum nesting depth");
        }

        char ch = get_next_token();
        if (failed)
            return Bson();

        if (ch == '-' || (ch >= '0' && ch <= '9')) {
            i--;
            return parse_number();
        }

        if (ch == 't')
            return expect("true", true);

        if (ch == 'f')
            return expect("false", false);

        if (ch == 'n')
            return expect("null", Bson());

        if (ch == '"')
            return parse_string();

        if (ch == '{') {
            map< string, Bson > data;
            ch = get_next_token();
            if (ch == '}')
                return data;

            while (1) {
                if (ch != '"')
                    return fail("expected '\"' in object, got " + esc(ch));

                string key = parse_string();
                if (failed)
                    return Bson();

                ch = get_next_token();
                if (ch != ':')
                    return fail("expected ':' in object, got " + esc(ch));

                data[/*std::move(key)*/ key] = parse_json(depth + 1);
                if (failed)
                    return Bson();

                ch = get_next_token();
                if (ch == '}')
                    break;
                if (ch != ',')
                    return fail("expected ',' in object, got " + esc(ch));

                ch = get_next_token();
            }
            return data;
        }

        if (ch == '[') {
            vector< Bson > data;
            ch = get_next_token();
            if (ch == ']')
                return data;

            while (1) {
                i--;
                data.push_back(parse_json(depth + 1));
                if (failed)
                    return Bson();

                ch = get_next_token();
                if (ch == ']')
                    break;
                if (ch != ',')
                    return fail("expected ',' in list, got " + esc(ch));

                ch = get_next_token();
                (void)ch;
            }
            return data;
        }

        return fail("expected value, got " + esc(ch));
    }
};
// }  // namespace

Bson Bson::parse(const string& in, string& err, BsonParse strategy)
{
    BsonParser parser = {in, 0, err, false, strategy};
    Bson result = parser.parse_json(0);

    // Check for any trailing garbage
    parser.consume_garbage();
    if (parser.failed)
        return Bson();
    if (parser.i != in.size())
        return parser.fail("unexpected trailing " + esc(in[parser.i]));

    return result;
}

// Documented in json11.hpp
vector< Bson > Bson::parse_multi(const string& in,
                                 std::string::size_type& parser_stop_pos,
                                 string& err,
                                 BsonParse strategy)
{
    BsonParser parser = {in, 0, err, false, strategy};
    parser_stop_pos = 0;
    vector< Bson > json_vec;
    while (parser.i != in.size() && !parser.failed) {
        json_vec.push_back(parser.parse_json(0));
        if (parser.failed)
            break;

        // Check for another object
        parser.consume_garbage();
        if (parser.failed)
            break;
        parser_stop_pos = parser.i;
    }
    return json_vec;
}

/* * * * * * * * * * * * * * * * * * * *
 * Shape-checking
 */

//bool Bson::has_shape(const shape & types, string & err) const {
//    if (!is_object()) {
//        err = "expected JSON object, got " + dump();
//        return false;
//    }
//
//    for (auto & item : types) {
//        if ((*this)[item.first].type() != item.second) {
//            err = "bad type for " + item.first + " in " + dump();
//            return false;
//        }
//    }
//
//    return true;
//}

}  // namespace JQUTIL_NS
