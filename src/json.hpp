#pragma once

#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace simplejson {

class JsonValue;
using JsonObject = std::map<std::string, JsonValue>;
using JsonArray = std::vector<JsonValue>;

class JsonValue {
public:
    using Variant = std::variant<std::nullptr_t, bool, double, std::string, JsonArray, JsonObject>;

    JsonValue() : value_(nullptr) {}
    JsonValue(std::nullptr_t) : value_(nullptr) {}
    JsonValue(bool b) : value_(b) {}
    JsonValue(double d) : value_(d) {}
    JsonValue(std::string s) : value_(std::move(s)) {}
    JsonValue(JsonArray arr) : value_(std::move(arr)) {}
    JsonValue(JsonObject obj) : value_(std::move(obj)) {}

    bool is_null() const { return std::holds_alternative<std::nullptr_t>(value_); }
    bool is_bool() const { return std::holds_alternative<bool>(value_); }
    bool is_number() const { return std::holds_alternative<double>(value_); }
    bool is_string() const { return std::holds_alternative<std::string>(value_); }
    bool is_array() const { return std::holds_alternative<JsonArray>(value_); }
    bool is_object() const { return std::holds_alternative<JsonObject>(value_); }

    const Variant &variant() const { return value_; }

    const std::string &as_string() const { return std::get<std::string>(value_); }
    double as_number() const { return std::get<double>(value_); }
    bool as_bool() const { return std::get<bool>(value_); }
    const JsonArray &as_array() const { return std::get<JsonArray>(value_); }
    const JsonObject &as_object() const { return std::get<JsonObject>(value_); }

    std::string &as_string() { return std::get<std::string>(value_); }
    JsonArray &as_array() { return std::get<JsonArray>(value_); }
    JsonObject &as_object() { return std::get<JsonObject>(value_); }

private:
    Variant value_;
};

class ParseError : public std::runtime_error {
public:
    explicit ParseError(const std::string &message) : std::runtime_error(message) {}
};

class Parser {
public:
    explicit Parser(std::string_view input) : input_(input), pos_(0) {}

    JsonValue parse() {
        skip_whitespace();
        JsonValue value = parse_value();
        skip_whitespace();
        if (!eof()) {
            throw ParseError("Unexpected characters after JSON value");
        }
        return value;
    }

private:
    JsonValue parse_value() {
        if (eof()) {
            throw ParseError("Unexpected end of input");
        }
        char ch = peek();
        switch (ch) {
            case 'n':
                return parse_null();
            case 't':
            case 'f':
                return parse_bool();
            case '"':
                return parse_string();
            case '[':
                return parse_array();
            case '{':
                return parse_object();
            default:
                if (ch == '-' || std::isdigit(static_cast<unsigned char>(ch))) {
                    return parse_number();
                }
                throw ParseError("Invalid JSON value");
        }
    }

    JsonValue parse_null() {
        expect("null");
        return JsonValue(nullptr);
    }

    JsonValue parse_bool() {
        if (match("true")) {
            return JsonValue(true);
        }
        if (match("false")) {
            return JsonValue(false);
        }
        throw ParseError("Invalid boolean value");
    }

    JsonValue parse_number() {
        size_t start = pos_;
        if (peek() == '-') {
            advance();
        }
        if (eof()) {
            throw ParseError("Unexpected end of input in number");
        }

        if (peek() == '0') {
            advance();
        } else if (std::isdigit(static_cast<unsigned char>(peek()))) {
            while (!eof() && std::isdigit(static_cast<unsigned char>(peek()))) {
                advance();
            }
        } else {
            throw ParseError("Invalid number");
        }

        if (!eof() && peek() == '.') {
            advance();
            if (eof() || !std::isdigit(static_cast<unsigned char>(peek()))) {
                throw ParseError("Invalid number fraction");
            }
            while (!eof() && std::isdigit(static_cast<unsigned char>(peek()))) {
                advance();
            }
        }

        if (!eof() && (peek() == 'e' || peek() == 'E')) {
            advance();
            if (!eof() && (peek() == '+' || peek() == '-')) {
                advance();
            }
            if (eof() || !std::isdigit(static_cast<unsigned char>(peek()))) {
                throw ParseError("Invalid number exponent");
            }
            while (!eof() && std::isdigit(static_cast<unsigned char>(peek()))) {
                advance();
            }
        }

        double value = std::stod(std::string(input_.substr(start, pos_ - start)));
        return JsonValue(value);
    }

    JsonValue parse_string() {
        expect('"');
        std::string result;
        while (!eof()) {
            char ch = advance();
            if (ch == '"') {
                return JsonValue(result);
            }
            if (ch == '\\') {
                if (eof()) {
                    throw ParseError("Unterminated escape sequence");
                }
                char escaped = advance();
                switch (escaped) {
                    case '"': result.push_back('"'); break;
                    case '\\': result.push_back('\\'); break;
                    case '/': result.push_back('/'); break;
                    case 'b': result.push_back('\b'); break;
                    case 'f': result.push_back('\f'); break;
                    case 'n': result.push_back('\n'); break;
                    case 'r': result.push_back('\r'); break;
                    case 't': result.push_back('\t'); break;
                    case 'u':
                        result.push_back(parse_unicode_escape());
                        break;
                    default:
                        throw ParseError("Invalid escape sequence");
                }
            } else if (static_cast<unsigned char>(ch) < 0x20) {
                throw ParseError("Invalid control character in string");
            } else {
                result.push_back(ch);
            }
        }
        throw ParseError("Unterminated string");
    }

    char parse_unicode_escape() {
        uint32_t value = 0;
        for (int i = 0; i < 4; ++i) {
            if (eof()) {
                throw ParseError("Unexpected end of input in unicode escape");
            }
            char ch = advance();
            value <<= 4;
            if (ch >= '0' && ch <= '9') {
                value += static_cast<uint32_t>(ch - '0');
            } else if (ch >= 'a' && ch <= 'f') {
                value += static_cast<uint32_t>(10 + ch - 'a');
            } else if (ch >= 'A' && ch <= 'F') {
                value += static_cast<uint32_t>(10 + ch - 'A');
            } else {
                throw ParseError("Invalid unicode escape");
            }
        }

        // Handle basic multilingual plane only. Surrogates are not supported.
        if (value <= 0x7F) {
            return static_cast<char>(value);
        }
        // For simplicity, we only support ASCII output. For higher code points,
        // replace with '?' to avoid multi-byte encoding complexities.
        return '?';
    }

    JsonValue parse_array() {
        expect('[');
        JsonArray array;
        skip_whitespace();
        if (peek() == ']') {
            advance();
            return JsonValue(std::move(array));
        }
        while (true) {
            array.push_back(parse_value());
            skip_whitespace();
            if (peek() == ',') {
                advance();
                skip_whitespace();
                continue;
            }
            if (peek() == ']') {
                advance();
                break;
            }
            throw ParseError("Expected ',' or ']' in array");
        }
        return JsonValue(std::move(array));
    }

    JsonValue parse_object() {
        expect('{');
        JsonObject object;
        skip_whitespace();
        if (peek() == '}') {
            advance();
            return JsonValue(std::move(object));
        }
        while (true) {
            if (peek() != '"') {
                throw ParseError("Expected string key in object");
            }
            std::string key = parse_string().as_string();
            skip_whitespace();
            expect(':');
            skip_whitespace();
            JsonValue value = parse_value();
            object.emplace(std::move(key), std::move(value));
            skip_whitespace();
            if (peek() == ',') {
                advance();
                skip_whitespace();
                continue;
            }
            if (peek() == '}') {
                advance();
                break;
            }
            throw ParseError("Expected ',' or '}' in object");
        }
        return JsonValue(std::move(object));
    }

    bool match(std::string_view text) {
        if (input_.substr(pos_, text.size()) == text) {
            pos_ += text.size();
            return true;
        }
        return false;
    }

    void expect(std::string_view text) {
        if (!match(text)) {
            throw ParseError("Expected '" + std::string(text) + "'");
        }
    }

    void expect(char expected) {
        if (eof() || advance() != expected) {
            throw ParseError(std::string("Expected '") + expected + "'");
        }
    }

    void skip_whitespace() {
        while (!eof() && std::isspace(static_cast<unsigned char>(peek()))) {
            advance();
        }
    }

    char peek() const {
        if (eof()) {
            throw ParseError("Unexpected end of input");
        }
        return input_[pos_];
    }

    char advance() {
        if (eof()) {
            throw ParseError("Unexpected end of input");
        }
        return input_[pos_++];
    }

    bool eof() const {
        return pos_ >= input_.size();
    }

    std::string_view input_;
    size_t pos_;
};

inline JsonValue parse(std::string_view input) {
    Parser parser(input);
    return parser.parse();
}

inline const JsonValue *get(const JsonObject &object, const std::string &key) {
    auto it = object.find(key);
    if (it == object.end()) {
        return nullptr;
    }
    return &it->second;
}

inline const JsonValue *get(const JsonArray &array, size_t index) {
    if (index >= array.size()) {
        return nullptr;
    }
    return &array[index];
}

} // namespace simplejson

