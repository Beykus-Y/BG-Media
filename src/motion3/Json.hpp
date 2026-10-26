#pragma once

// Minimal JSON reader used for BG Motion clip resources. It exists because the
// repository has no JSON dependency; it reports line/column on errors so a
// hand-edited clip gives an actionable message. Not a general purpose library.

#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace bgmedia::motion::json {

struct Value {
    enum class Type { Null, Bool, Number, String, Array, Object };
    Type type = Type::Null;
    bool boolean = false;
    double number = 0.0;
    std::string string;
    std::vector<Value> array;
    std::vector<std::pair<std::string, Value>> object;

    [[nodiscard]] bool isNumber() const { return type == Type::Number; }
    [[nodiscard]] bool isString() const { return type == Type::String; }
    [[nodiscard]] bool isArray() const { return type == Type::Array; }
    [[nodiscard]] bool isObject() const { return type == Type::Object; }

    [[nodiscard]] const Value* find(const std::string& key) const {
        for (const auto& entry : object) {
            if (entry.first == key) {
                return &entry.second;
            }
        }
        return nullptr;
    }
};

class Parser {
public:
    explicit Parser(const std::string& text) : text_(text) {}

    bool parse(Value& out, std::string& error) {
        skip();
        if (!value(out)) {
            error = error_;
            return false;
        }
        skip();
        if (pos_ != text_.size()) {
            fail("unexpected trailing characters");
            error = error_;
            return false;
        }
        return true;
    }

private:
    void fail(const std::string& message) {
        if (!error_.empty()) {
            return;
        }
        int line = 1;
        int column = 1;
        for (std::size_t i = 0; i < pos_ && i < text_.size(); ++i) {
            if (text_[i] == '\n') {
                ++line;
                column = 1;
            } else {
                ++column;
            }
        }
        error_ = "JSON error at line " + std::to_string(line) + ", column " +
                 std::to_string(column) + ": " + message;
    }

    void skip() {
        while (pos_ < text_.size()) {
            const char c = text_[pos_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                ++pos_;
            } else {
                break;
            }
        }
    }

    bool literal(const char* word) {
        std::size_t i = 0;
        while (word[i]) {
            if (pos_ + i >= text_.size() || text_[pos_ + i] != word[i]) {
                return false;
            }
            ++i;
        }
        pos_ += i;
        return true;
    }

    bool value(Value& out) {
        if (depth_ > 32) {
            fail("nesting too deep");
            return false;
        }
        if (pos_ >= text_.size()) {
            fail("unexpected end of input");
            return false;
        }
        const char c = text_[pos_];
        if (c == '{') {
            return object(out);
        }
        if (c == '[') {
            return array(out);
        }
        if (c == '"') {
            out.type = Value::Type::String;
            return string(out.string);
        }
        if (c == 't' && literal("true")) {
            out.type = Value::Type::Bool;
            out.boolean = true;
            return true;
        }
        if (c == 'f' && literal("false")) {
            out.type = Value::Type::Bool;
            out.boolean = false;
            return true;
        }
        if (c == 'n' && literal("null")) {
            out.type = Value::Type::Null;
            return true;
        }
        if (c == '-' || (c >= '0' && c <= '9')) {
            return number(out);
        }
        fail(std::string("unexpected character '") + c + "'");
        return false;
    }

    bool number(Value& out) {
        const char* begin = text_.c_str() + pos_;
        char* end = nullptr;
        const double parsed = std::strtod(begin, &end);
        if (end == begin) {
            fail("malformed number");
            return false;
        }
        pos_ += static_cast<std::size_t>(end - begin);
        out.type = Value::Type::Number;
        out.number = parsed;
        return true;
    }

    bool string(std::string& out) {
        ++pos_;  // opening quote
        out.clear();
        while (pos_ < text_.size()) {
            const char c = text_[pos_++];
            if (c == '"') {
                return true;
            }
            if (c == '\\') {
                if (pos_ >= text_.size()) {
                    break;
                }
                const char e = text_[pos_++];
                switch (e) {
                case 'n': out.push_back('\n'); break;
                case 't': out.push_back('\t'); break;
                case 'r': out.push_back('\r'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case '/': out.push_back('/'); break;
                case '\\': out.push_back('\\'); break;
                case '"': out.push_back('"'); break;
                case 'u': {
                    // Basic Multilingual Plane only; enough for names/notes.
                    unsigned code = 0;
                    for (int i = 0; i < 4 && pos_ < text_.size(); ++i) {
                        const char h = text_[pos_++];
                        code <<= 4;
                        if (h >= '0' && h <= '9') code |= static_cast<unsigned>(h - '0');
                        else if (h >= 'a' && h <= 'f') code |= static_cast<unsigned>(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F') code |= static_cast<unsigned>(h - 'A' + 10);
                        else { fail("bad \\u escape"); return false; }
                    }
                    if (code < 0x80) {
                        out.push_back(static_cast<char>(code));
                    } else if (code < 0x800) {
                        out.push_back(static_cast<char>(0xC0 | (code >> 6)));
                        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
                    } else {
                        out.push_back(static_cast<char>(0xE0 | (code >> 12)));
                        out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
                        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
                    }
                    break;
                }
                default: fail("bad escape sequence"); return false;
                }
            } else {
                out.push_back(c);
            }
        }
        fail("unterminated string");
        return false;
    }

    bool array(Value& out) {
        out.type = Value::Type::Array;
        ++pos_;
        ++depth_;
        skip();
        if (pos_ < text_.size() && text_[pos_] == ']') {
            ++pos_;
            --depth_;
            return true;
        }
        while (true) {
            skip();
            Value element;
            if (!value(element)) {
                return false;
            }
            out.array.push_back(std::move(element));
            skip();
            if (pos_ >= text_.size()) {
                fail("unterminated array");
                return false;
            }
            if (text_[pos_] == ',') {
                ++pos_;
                continue;
            }
            if (text_[pos_] == ']') {
                ++pos_;
                --depth_;
                return true;
            }
            fail("expected ',' or ']' in array");
            return false;
        }
    }

    bool object(Value& out) {
        out.type = Value::Type::Object;
        ++pos_;
        ++depth_;
        skip();
        if (pos_ < text_.size() && text_[pos_] == '}') {
            ++pos_;
            --depth_;
            return true;
        }
        while (true) {
            skip();
            if (pos_ >= text_.size() || text_[pos_] != '"') {
                fail("expected a string key");
                return false;
            }
            std::string key;
            if (!string(key)) {
                return false;
            }
            skip();
            if (pos_ >= text_.size() || text_[pos_] != ':') {
                fail("expected ':' after key");
                return false;
            }
            ++pos_;
            skip();
            Value element;
            if (!value(element)) {
                return false;
            }
            out.object.emplace_back(std::move(key), std::move(element));
            skip();
            if (pos_ >= text_.size()) {
                fail("unterminated object");
                return false;
            }
            if (text_[pos_] == ',') {
                ++pos_;
                continue;
            }
            if (text_[pos_] == '}') {
                ++pos_;
                --depth_;
                return true;
            }
            fail("expected ',' or '}' in object");
            return false;
        }
    }

    const std::string& text_;
    std::size_t pos_ = 0;
    int depth_ = 0;
    std::string error_;
};

} // namespace bgmedia::motion::json
