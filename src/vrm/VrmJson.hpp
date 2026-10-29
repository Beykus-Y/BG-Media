#pragma once

// Internal to src/vrm: the bounded JSON DOM shared by the VRM 0.x and VRM 1.0
// schema readers. Not part of the public include tree.

#include "bgmedia/math/Math.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace bgmedia::vrm::json {

using math::Vec3;
using math::Vec4;

// ---------------------------------------------------------------------------
// Minimal JSON DOM, used only for the VRM extension block. The glTF core is
// handled by cgltf. Keeping this parser small and bounded means a corrupted
// VRM cannot recurse without limit or read outside the JSON chunk.
// ---------------------------------------------------------------------------
struct JsonValue {
    enum class Type { Null, Bool, Number, String, Array, Object };

    Type type = Type::Null;
    bool boolean = false;
    double number = 0.0;
    std::string string;
    std::vector<JsonValue> items;
    std::vector<std::pair<std::string, JsonValue>> fields;

    [[nodiscard]] const JsonValue* find(std::string_view key) const {
        for (auto it = fields.rbegin(); it != fields.rend(); ++it) {
            if (it->first == key) {
                return &it->second;
            }
        }
        return nullptr;
    }
    [[nodiscard]] const JsonValue* at(std::size_t index) const {
        return index < items.size() ? &items[index] : nullptr;
    }
    [[nodiscard]] bool isObject() const { return type == Type::Object; }
    [[nodiscard]] bool isArray() const { return type == Type::Array; }
    [[nodiscard]] bool isString() const { return type == Type::String; }
    [[nodiscard]] bool isNumber() const { return type == Type::Number; }
};

class JsonParser {
public:
    explicit JsonParser(std::string_view text) : text_(text) {}

    bool parse(JsonValue& out, std::string& error) {
        skipWhitespace();
        if (!parseValue(out, 0)) {
            error = lastError_;
            return false;
        }
        skipWhitespace();
        if (position_ != text_.size()) {
            error = "unexpected trailing JSON data at byte " +
                    std::to_string(position_);
            return false;
        }
        return true;
    }

private:
    bool fail(std::string message, int depth) {
        lastError_ = std::move(message) + " at byte " + std::to_string(position_);
        if (depth > 0) {
            lastError_ += " (depth " + std::to_string(depth) + ")";
        }
        return false;
    }

    void skipWhitespace() {
        while (position_ < text_.size()) {
            const char c = text_[position_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                ++position_;
            } else {
                break;
            }
        }
    }

    [[nodiscard]] char peek() const {
        return position_ < text_.size() ? text_[position_] : '\0';
    }

    bool parseValue(JsonValue& out, int depth) {
        if (depth > 128) {
            lastError_ = "JSON nesting exceeds 128 levels";
            return false;
        }
        skipWhitespace();
        if (position_ >= text_.size()) {
            return fail("unexpected end of JSON", depth);
        }
        const char c = text_[position_];
        if (c == '{') {
            return parseObject(out, depth);
        }
        if (c == '[') {
            return parseArray(out, depth);
        }
        if (c == '"') {
            out.type = JsonValue::Type::String;
            return parseString(out.string, depth);
        }
        if (c == 't') {
            return parseLiteral("true", out, true, depth);
        }
        if (c == 'f') {
            return parseLiteral("false", out, false, depth);
        }
        if (c == 'n') {
            return parseNull(out, depth);
        }
        if (c == '-' || (c >= '0' && c <= '9')) {
            return parseNumber(out, depth);
        }
        return fail("invalid JSON value", depth);
    }

    bool parseObject(JsonValue& out, int depth) {
        out.type = JsonValue::Type::Object;
        ++position_;
        skipWhitespace();
        if (peek() == '}') {
            ++position_;
            return true;
        }
        while (true) {
            skipWhitespace();
            if (peek() != '"') {
                return fail("expected object key", depth);
            }
            std::string key;
            if (!parseString(key, depth)) {
                return false;
            }
            skipWhitespace();
            if (peek() != ':') {
                return fail("expected ':' after object key", depth);
            }
            ++position_;
            JsonValue value;
            if (!parseValue(value, depth + 1)) {
                return false;
            }
            out.fields.emplace_back(std::move(key), std::move(value));
            skipWhitespace();
            if (peek() == ',') {
                ++position_;
                continue;
            }
            if (peek() == '}') {
                ++position_;
                return true;
            }
            return fail("expected ',' or '}' in object", depth);
        }
    }

    bool parseArray(JsonValue& out, int depth) {
        out.type = JsonValue::Type::Array;
        ++position_;
        skipWhitespace();
        if (peek() == ']') {
            ++position_;
            return true;
        }
        while (true) {
            JsonValue value;
            if (!parseValue(value, depth + 1)) {
                return false;
            }
            out.items.push_back(std::move(value));
            skipWhitespace();
            if (peek() == ',') {
                ++position_;
                continue;
            }
            if (peek() == ']') {
                ++position_;
                return true;
            }
            return fail("expected ',' or ']' in array", depth);
        }
    }

    bool parseLiteral(std::string_view literal, JsonValue& out, bool value,
                      int depth) {
        if (text_.substr(position_, literal.size()) != literal) {
            return fail("invalid literal", depth);
        }
        position_ += literal.size();
        out.type = JsonValue::Type::Bool;
        out.boolean = value;
        return true;
    }

    bool parseNull(JsonValue& out, int depth) {
        if (text_.substr(position_, 4) != "null") {
            return fail("invalid literal", depth);
        }
        position_ += 4;
        out.type = JsonValue::Type::Null;
        return true;
    }

    bool parseNumber(JsonValue& out, int depth) {
        const std::size_t start = position_;
        if (peek() == '-') {
            ++position_;
        }
        if (peek() == '0') {
            ++position_;
        } else if (peek() >= '1' && peek() <= '9') {
            while (peek() >= '0' && peek() <= '9') {
                ++position_;
            }
        } else {
            return fail("invalid number", depth);
        }
        if (peek() == '.') {
            ++position_;
            if (!(peek() >= '0' && peek() <= '9')) {
                return fail("invalid number fraction", depth);
            }
            while (peek() >= '0' && peek() <= '9') {
                ++position_;
            }
        }
        if (peek() == 'e' || peek() == 'E') {
            ++position_;
            if (peek() == '+' || peek() == '-') {
                ++position_;
            }
            if (!(peek() >= '0' && peek() <= '9')) {
                return fail("invalid number exponent", depth);
            }
            while (peek() >= '0' && peek() <= '9') {
                ++position_;
            }
        }
        const std::string token(text_.substr(start, position_ - start));
        char* end = nullptr;
        const double value = std::strtod(token.c_str(), &end);
        if (!end || *end != '\0' || !std::isfinite(value)) {
            return fail("number is not finite", depth);
        }
        out.type = JsonValue::Type::Number;
        out.number = value;
        return true;
    }

    bool parseString(std::string& out, int depth) {
        if (peek() != '"') {
            return fail("expected string", depth);
        }
        ++position_;
        out.clear();
        while (position_ < text_.size()) {
            const unsigned char c = static_cast<unsigned char>(text_[position_++]);
            if (c == '"') {
                return true;
            }
            if (c < 0x20) {
                return fail("control character in string", depth);
            }
            if (c != '\\') {
                out.push_back(static_cast<char>(c));
                continue;
            }
            if (position_ >= text_.size()) {
                return fail("truncated escape", depth);
            }
            const char escape = text_[position_++];
            switch (escape) {
            case '"':
                out.push_back('"');
                break;
            case '\\':
                out.push_back('\\');
                break;
            case '/':
                out.push_back('/');
                break;
            case 'b':
                out.push_back('\b');
                break;
            case 'f':
                out.push_back('\f');
                break;
            case 'n':
                out.push_back('\n');
                break;
            case 'r':
                out.push_back('\r');
                break;
            case 't':
                out.push_back('\t');
                break;
            case 'u': {
                std::uint32_t code = 0;
                if (!readHex4(code, depth)) {
                    return false;
                }
                if (code >= 0xD800 && code <= 0xDBFF) {
                    if (position_ + 2 > text_.size() || text_[position_] != '\\' ||
                        text_[position_ + 1] != 'u') {
                        return fail("unpaired high surrogate", depth);
                    }
                    position_ += 2;
                    std::uint32_t low = 0;
                    if (!readHex4(low, depth)) {
                        return false;
                    }
                    if (low < 0xDC00 || low > 0xDFFF) {
                        return fail("invalid low surrogate", depth);
                    }
                    code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                } else if (code >= 0xDC00 && code <= 0xDFFF) {
                    return fail("unpaired low surrogate", depth);
                }
                appendUtf8(out, code);
                break;
            }
            default:
                return fail("unknown escape", depth);
            }
        }
        return fail("unterminated string", depth);
    }

    bool readHex4(std::uint32_t& value, int depth) {
        if (position_ + 4 > text_.size()) {
            return fail("truncated unicode escape", depth);
        }
        value = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = text_[position_++];
            value <<= 4;
            if (c >= '0' && c <= '9') {
                value |= static_cast<std::uint32_t>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                value |= static_cast<std::uint32_t>(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                value |= static_cast<std::uint32_t>(c - 'A' + 10);
            } else {
                return fail("invalid unicode escape", depth);
            }
        }
        return true;
    }

    static void appendUtf8(std::string& out, std::uint32_t code) {
        if (code <= 0x7F) {
            out.push_back(static_cast<char>(code));
        } else if (code <= 0x7FF) {
            out.push_back(static_cast<char>(0xC0 | (code >> 6)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else if (code <= 0xFFFF) {
            out.push_back(static_cast<char>(0xE0 | (code >> 12)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (code >> 18)));
            out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        }
    }

    std::string_view text_;
    std::size_t position_ = 0;
    std::string lastError_;
};

inline const JsonValue* objectField(const JsonValue* object, std::string_view key) {
    return object && object->isObject() ? object->find(key) : nullptr;
}

inline std::string stringField(const JsonValue* object, std::string_view key) {
    const JsonValue* value = objectField(object, key);
    return value && value->isString() ? value->string : std::string{};
}

inline double numberField(const JsonValue* object, std::string_view key,
                   double fallback = 0.0) {
    const JsonValue* value = objectField(object, key);
    return value && value->isNumber() ? value->number : fallback;
}

inline Vec3 vec3Field(const JsonValue* object, std::string_view key, Vec3 fallback) {
    const JsonValue* value = objectField(object, key);
    if (!value) {
        return fallback;
    }
    if (value->isObject()) {
        return {static_cast<float>(numberField(value, "x", fallback.x)),
                static_cast<float>(numberField(value, "y", fallback.y)),
                static_cast<float>(numberField(value, "z", fallback.z))};
    }
    if (value->isArray() && value->items.size() >= 3 &&
        value->items[0].isNumber() && value->items[1].isNumber() &&
        value->items[2].isNumber()) {
        return {static_cast<float>(value->items[0].number),
                static_cast<float>(value->items[1].number),
                static_cast<float>(value->items[2].number)};
    }
    return fallback;
}

inline Vec4 vec4Field(const JsonValue* object, std::string_view key, Vec4 fallback) {
    const JsonValue* value = objectField(object, key);
    if (!value || !value->isArray() || value->items.size() < 4) {
        return fallback;
    }
    if (!value->items[0].isNumber() || !value->items[1].isNumber() ||
        !value->items[2].isNumber() || !value->items[3].isNumber()) {
        return fallback;
    }
    return {static_cast<float>(value->items[0].number),
            static_cast<float>(value->items[1].number),
            static_cast<float>(value->items[2].number),
            static_cast<float>(value->items[3].number)};
}

} // namespace bgmedia::vrm::json
