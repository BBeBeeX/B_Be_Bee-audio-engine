#ifndef JSON_HPP
#define JSON_HPP

#include <string>
#include <vector>
#include <map>
#include <sstream>
#include <cctype>
#include <cstdlib>
#include <stdexcept>
#include <iomanip>

namespace audio_engine {

class JsonValue {
public:
    enum Type { TYPE_NULL, TYPE_BOOL, TYPE_NUMBER, TYPE_STRING, TYPE_ARRAY, TYPE_OBJECT };

    Type type = TYPE_NULL;
    bool boolVal = false;
    double numVal = 0.0;
    std::string strVal;
    std::vector<JsonValue> arrVal;
    std::map<std::string, JsonValue> objVal;

    JsonValue() : type(TYPE_NULL) {}
    JsonValue(bool b) : type(TYPE_BOOL), boolVal(b) {}
    JsonValue(double n) : type(TYPE_NUMBER), numVal(n) {}
    JsonValue(int n) : type(TYPE_NUMBER), numVal(static_cast<double>(n)) {}
    JsonValue(int64_t n) : type(TYPE_NUMBER), numVal(static_cast<double>(n)) {}
    JsonValue(const char* s) : type(TYPE_STRING), strVal(s ? s : "") {}
    JsonValue(const std::string& s) : type(TYPE_STRING), strVal(s) {}

    static JsonValue array() {
        JsonValue v;
        v.type = TYPE_ARRAY;
        return v;
    }

    static JsonValue object() {
        JsonValue v;
        v.type = TYPE_OBJECT;
        return v;
    }

    bool isNull() const { return type == TYPE_NULL; }
    bool isBool() const { return type == TYPE_BOOL; }
    bool isNumber() const { return type == TYPE_NUMBER; }
    bool isString() const { return type == TYPE_STRING; }
    bool isArray() const { return type == TYPE_ARRAY; }
    bool isObject() const { return type == TYPE_OBJECT; }

    bool asBool(bool defaultVal = false) const {
        return isBool() ? boolVal : defaultVal;
    }

    double asNumber(double defaultVal = 0.0) const {
        return isNumber() ? numVal : defaultVal;
    }

    int asInt(int defaultVal = 0) const {
        return isNumber() ? static_cast<int>(numVal) : defaultVal;
    }

    int64_t asInt64(int64_t defaultVal = 0) const {
        return isNumber() ? static_cast<int64_t>(numVal) : defaultVal;
    }

    std::string asString(const std::string& defaultVal = "") const {
        return isString() ? strVal : defaultVal;
    }

    bool has(const std::string& key) const {
        if (!isObject()) return false;
        return objVal.find(key) != objVal.end();
    }

    const JsonValue& get(const std::string& key) const {
        static const JsonValue nullVal;
        if (!isObject()) return nullVal;
        auto it = objVal.find(key);
        return (it != objVal.end()) ? it->second : nullVal;
    }

    JsonValue& operator[](const std::string& key) {
        if (type != TYPE_OBJECT) {
            type = TYPE_OBJECT;
            objVal.clear();
        }
        return objVal[key];
    }

    const JsonValue& operator[](size_t index) const {
        static const JsonValue nullVal;
        if (!isArray() || index >= arrVal.size()) return nullVal;
        return arrVal[index];
    }

    void push_back(const JsonValue& val) {
        if (type != TYPE_ARRAY) {
            type = TYPE_ARRAY;
            arrVal.clear();
        }
        arrVal.push_back(val);
    }

    std::string serialize() const {
        std::ostringstream ss;
        serializeInternal(ss);
        return ss.str();
    }

    static JsonValue parse(const std::string& str) {
        size_t pos = 0;
        skipWhitespace(str, pos);
        return parseValue(str, pos);
    }

private:
    static void escapeString(std::ostringstream& ss, const std::string& str) {
        ss << '"';
        for (char c : str) {
            unsigned char uc = static_cast<unsigned char>(c);
            if (c == '"') ss << "\\\"";
            else if (c == '\\') ss << "\\\\";
            else if (c == '\b') ss << "\\b";
            else if (c == '\f') ss << "\\f";
            else if (c == '\n') ss << "\\n";
            else if (c == '\r') ss << "\\r";
            else if (c == '\t') ss << "\\t";
            else if (uc < 0x20) {
                char buf[7];
                std::snprintf(buf, sizeof(buf), "\\u%04x", uc);
                ss << buf;
            } else {
                ss << c;
            }
        }
        ss << '"';
    }

    void serializeInternal(std::ostringstream& ss) const {
        switch (type) {
            case TYPE_NULL:
                ss << "null";
                break;
            case TYPE_BOOL:
                ss << (boolVal ? "true" : "false");
                break;
            case TYPE_NUMBER:
                if (numVal == static_cast<int64_t>(numVal)) {
                    ss << static_cast<int64_t>(numVal);
                } else {
                    ss << std::setprecision(10) << numVal;
                }
                break;
            case TYPE_STRING:
                escapeString(ss, strVal);
                break;
            case TYPE_ARRAY:
                ss << '[';
                for (size_t i = 0; i < arrVal.size(); ++i) {
                    if (i > 0) ss << ',';
                    arrVal[i].serializeInternal(ss);
                }
                ss << ']';
                break;
            case TYPE_OBJECT:
                ss << '{';
                bool first = true;
                for (const auto& kv : objVal) {
                    if (!first) ss << ',';
                    first = false;
                    escapeString(ss, kv.first);
                    ss << ':';
                    kv.second.serializeInternal(ss);
                }
                ss << '}';
                break;
        }
    }

    static void skipWhitespace(const std::string& s, size_t& pos) {
        while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\t' || s[pos] == '\n' || s[pos] == '\r')) {
            ++pos;
        }
    }

    static JsonValue parseValue(const std::string& s, size_t& pos) {
        skipWhitespace(s, pos);
        if (pos >= s.size()) return JsonValue();

        char c = s[pos];
        if (c == 'n') {
            if (s.compare(pos, 4, "null") == 0) { pos += 4; return JsonValue(); }
        } else if (c == 't') {
            if (s.compare(pos, 4, "true") == 0) { pos += 4; return JsonValue(true); }
        } else if (c == 'f') {
            if (s.compare(pos, 5, "false") == 0) { pos += 5; return JsonValue(false); }
        } else if (c == '"') {
            return parseString(s, pos);
        } else if (c == '[') {
            return parseArray(s, pos);
        } else if (c == '{') {
            return parseObject(s, pos);
        } else if (c == '-' || (c >= '0' && c <= '9')) {
            return parseNumber(s, pos);
        }
        return JsonValue();
    }

    static JsonValue parseString(const std::string& s, size_t& pos) {
        ++pos; // skip opening quote
        std::string result;
        auto hexVal = [](char h) -> int {
            if (h >= '0' && h <= '9') return h - '0';
            if (h >= 'a' && h <= 'f') return h - 'a' + 10;
            if (h >= 'A' && h <= 'F') return h - 'A' + 10;
            return -1;
        };

        while (pos < s.size()) {
            char c = s[pos++];
            if (c == '"') return JsonValue(result);
            if (c == '\\' && pos < s.size()) {
                char esc = s[pos++];
                if (esc == '"') result += '"';
                else if (esc == '\\') result += '\\';
                else if (esc == '/') result += '/';
                else if (esc == 'b') result += '\b';
                else if (esc == 'f') result += '\f';
                else if (esc == 'n') result += '\n';
                else if (esc == 'r') result += '\r';
                else if (esc == 't') result += '\t';
                else if (esc == 'u' && pos + 4 <= s.size()) {
                    int d0 = hexVal(s[pos]);
                    int d1 = hexVal(s[pos + 1]);
                    int d2 = hexVal(s[pos + 2]);
                    int d3 = hexVal(s[pos + 3]);
                    if (d0 >= 0 && d1 >= 0 && d2 >= 0 && d3 >= 0) {
                        uint32_t cp = (static_cast<uint32_t>(d0) << 12) |
                                      (static_cast<uint32_t>(d1) << 8) |
                                      (static_cast<uint32_t>(d2) << 4) |
                                      static_cast<uint32_t>(d3);
                        pos += 4;

                        // Check for UTF-16 surrogate pair: high surrogate 0xD800..0xDBFF
                        if (cp >= 0xD800 && cp <= 0xDBFF) {
                            if (pos + 6 <= s.size() && s[pos] == '\\' && s[pos + 1] == 'u') {
                                int l0 = hexVal(s[pos + 2]);
                                int l1 = hexVal(s[pos + 3]);
                                int l2 = hexVal(s[pos + 4]);
                                int l3 = hexVal(s[pos + 5]);
                                if (l0 >= 0 && l1 >= 0 && l2 >= 0 && l3 >= 0) {
                                    uint32_t low = (static_cast<uint32_t>(l0) << 12) |
                                                   (static_cast<uint32_t>(l1) << 8) |
                                                   (static_cast<uint32_t>(l2) << 4) |
                                                   static_cast<uint32_t>(l3);
                                    if (low >= 0xDC00 && low <= 0xDFFF) {
                                        pos += 6;
                                        cp = 0x10000 + (((cp - 0xD800) << 10) | (low - 0xDC00));
                                    }
                                }
                            }
                        }

                        // Encode cp to UTF-8
                        if (cp <= 0x7F) {
                            result += static_cast<char>(cp);
                        } else if (cp <= 0x7FF) {
                            result += static_cast<char>(0xC0 | ((cp >> 6) & 0x1F));
                            result += static_cast<char>(0x80 | (cp & 0x3F));
                        } else if (cp <= 0xFFFF) {
                            if (cp >= 0xD800 && cp <= 0xDFFF) {
                                result += "\xEF\xBF\xBD"; // U+FFFD replacement char
                            } else {
                                result += static_cast<char>(0xE0 | ((cp >> 12) & 0x0F));
                                result += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                                result += static_cast<char>(0x80 | (cp & 0x3F));
                            }
                        } else if (cp <= 0x10FFFF) {
                            result += static_cast<char>(0xF0 | ((cp >> 18) & 0x07));
                            result += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
                            result += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                            result += static_cast<char>(0x80 | (cp & 0x3F));
                        } else {
                            result += "\xEF\xBF\xBD";
                        }
                    } else {
                        result += 'u';
                    }
                } else {
                    result += esc;
                }
            } else {
                result += c;
            }
        }
        return JsonValue(result);
    }

    static JsonValue parseNumber(const std::string& s, size_t& pos) {
        size_t start = pos;
        if (s[pos] == '-') ++pos;
        while (pos < s.size() && ((s[pos] >= '0' && s[pos] <= '9') || s[pos] == '.' || s[pos] == 'e' || s[pos] == 'E' || s[pos] == '+' || s[pos] == '-')) {
            ++pos;
        }
        std::string numStr = s.substr(start, pos - start);
        double val = std::strtod(numStr.c_str(), nullptr);
        // Fallback for locales that interpret comma instead of dot as decimal separator
        if (numStr.find('.') != std::string::npos && val == static_cast<int64_t>(val)) {
            size_t dotPos = numStr.find('.');
            if (dotPos + 1 < numStr.size() && std::isdigit(numStr[dotPos + 1])) {
                std::string alt = numStr;
                alt[dotPos] = ',';
                double altVal = std::strtod(alt.c_str(), nullptr);
                if (altVal != val) val = altVal;
            }
        }
        return JsonValue(val);
    }

    static JsonValue parseArray(const std::string& s, size_t& pos) {
        ++pos; // skip '['
        JsonValue arr = JsonValue::array();
        skipWhitespace(s, pos);
        if (pos < s.size() && s[pos] == ']') {
            ++pos;
            return arr;
        }
        while (pos < s.size()) {
            arr.push_back(parseValue(s, pos));
            skipWhitespace(s, pos);
            if (pos < s.size() && s[pos] == ',') {
                ++pos;
                skipWhitespace(s, pos);
            } else if (pos < s.size() && s[pos] == ']') {
                ++pos;
                break;
            } else {
                break;
            }
        }
        return arr;
    }

    static JsonValue parseObject(const std::string& s, size_t& pos) {
        ++pos; // skip '{'
        JsonValue obj = JsonValue::object();
        skipWhitespace(s, pos);
        if (pos < s.size() && s[pos] == '}') {
            ++pos;
            return obj;
        }
        while (pos < s.size()) {
            skipWhitespace(s, pos);
            if (pos >= s.size() || s[pos] != '"') break;
            JsonValue keyVal = parseString(s, pos);
            std::string key = keyVal.asString();
            skipWhitespace(s, pos);
            if (pos < s.size() && s[pos] == ':') {
                ++pos;
            } else {
                break;
            }
            skipWhitespace(s, pos);
            obj[key] = parseValue(s, pos);
            skipWhitespace(s, pos);
            if (pos < s.size() && s[pos] == ',') {
                ++pos;
                skipWhitespace(s, pos);
            } else if (pos < s.size() && s[pos] == '}') {
                ++pos;
                break;
            } else {
                break;
            }
        }
        return obj;
    }
};

} // namespace audio_engine

#endif // JSON_HPP
