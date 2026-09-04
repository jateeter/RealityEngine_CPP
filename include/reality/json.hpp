#pragma once

#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <variant>
#include <vector>

namespace reality::json {

// Render a double exactly as ECMAScript Number::toString does (ECMA-262
// 6.1.6.1.20), which is the form JSON.stringify produces and therefore the
// form the TypeScript runtime already emits.  Shortest round-trip digits;
// integer form when the value is whole and within plain range; exponential
// form only outside 1e21 / 1e-7.
//
// This replaces `ostringstream << double`, which defaulted to six significant
// digits and silently corrupted values: 1234567.0 came out as "1.23457e+06"
// (i.e. 1234570) and pi as "3.14159".  Precision loss, not just formatting
// drift (RealityEngine_CI#91).
inline std::string format_double(double value) {
  if (std::isnan(value) || std::isinf(value)) return "null";
  if (value == 0.0) return "0";

  const bool negative = value < 0.0;
  const double magnitude = negative ? -value : value;

  char buf[64];
  auto res = std::to_chars(buf, buf + sizeof(buf), magnitude, std::chars_format::scientific);
  if (res.ec != std::errc{}) return "null";
  const std::string sci(buf, res.ptr);

  const std::size_t epos = sci.find('e');
  std::string digits;
  for (std::size_t i = 0; i < epos; ++i) {
    if (sci[i] != '.') digits.push_back(sci[i]);
  }
  // ECMA-262 names: s = digits, k = digit count, n = decimal point position.
  const int k = static_cast<int>(digits.size());
  const int n = std::atoi(sci.c_str() + epos + 1) + 1;

  std::string out;
  if (negative) out.push_back('-');

  if (k <= n && n <= 21) {
    out += digits;
    out.append(static_cast<std::size_t>(n - k), '0');
  } else if (0 < n && n <= 21) {
    out += digits.substr(0, static_cast<std::size_t>(n));
    out.push_back('.');
    out += digits.substr(static_cast<std::size_t>(n));
  } else if (-6 < n && n <= 0) {
    out += "0.";
    out.append(static_cast<std::size_t>(-n), '0');
    out += digits;
  } else {
    out.push_back(digits[0]);
    if (k > 1) {
      out.push_back('.');
      out += digits.substr(1);
    }
    out.push_back('e');
    out.push_back(n - 1 >= 0 ? '+' : '-');
    out += std::to_string(std::abs(n - 1));
  }
  return out;
}

class Value {
public:
  using Array = std::vector<Value>;
  using Object = std::map<std::string, Value>;
  using Data = std::variant<std::nullptr_t, bool, double, std::string, Array, Object>;

  Data data;

  Value() : data(nullptr) {}
  Value(std::nullptr_t) : data(nullptr) {}
  Value(bool v) : data(v) {}
  Value(int v) : data(static_cast<double>(v)) {}
  Value(long v) : data(static_cast<double>(v)) {}
  Value(double v) : data(v) {}
  Value(const char* v) : data(std::string(v)) {}
  Value(std::string v) : data(std::move(v)) {}
  Value(Array v) : data(std::move(v)) {}
  Value(Object v) : data(std::move(v)) {}

  bool is_null() const { return std::holds_alternative<std::nullptr_t>(data); }
  bool is_bool() const { return std::holds_alternative<bool>(data); }
  bool is_number() const { return std::holds_alternative<double>(data); }
  bool is_string() const { return std::holds_alternative<std::string>(data); }
  bool is_array() const { return std::holds_alternative<Array>(data); }
  bool is_object() const { return std::holds_alternative<Object>(data); }

  const Array& array() const { return std::get<Array>(data); }
  Array& array() { return std::get<Array>(data); }
  const Object& object() const { return std::get<Object>(data); }
  Object& object() { return std::get<Object>(data); }

  std::string as_string(const std::string& fallback = "") const {
    return is_string() ? std::get<std::string>(data) : fallback;
  }
  double as_number(double fallback = 0.0) const {
    return is_number() ? std::get<double>(data) : fallback;
  }
  bool as_bool(bool fallback = false) const {
    return is_bool() ? std::get<bool>(data) : fallback;
  }

  const Value& at(const std::string& key) const {
    static const Value null_value;
    if (!is_object()) return null_value;
    const auto& obj = object();
    auto it = obj.find(key);
    return it == obj.end() ? null_value : it->second;
  }

};

inline std::string escape(const std::string& s) {
  std::ostringstream out;
  for (char c : s) {
    switch (c) {
      case '"': out << "\\\""; break;
      case '\\': out << "\\\\"; break;
      case '\b': out << "\\b"; break;
      case '\f': out << "\\f"; break;
      case '\n': out << "\\n"; break;
      case '\r': out << "\\r"; break;
      case '\t': out << "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) out << "\\u00" << std::hex << static_cast<int>(c);
        else out << c;
    }
  }
  return out.str();
}

inline std::string stringify(const Value& v);

inline std::string stringify_array(const Value::Array& arr) {
  std::ostringstream out;
  out << "[";
  for (size_t i = 0; i < arr.size(); ++i) {
    if (i) out << ",";
    out << stringify(arr[i]);
  }
  out << "]";
  return out.str();
}

inline std::string stringify_object(const Value::Object& obj) {
  std::ostringstream out;
  out << "{";
  bool first = true;
  for (const auto& [k, v] : obj) {
    if (!first) out << ",";
    first = false;
    out << "\"" << escape(k) << "\":" << stringify(v);
  }
  out << "}";
  return out.str();
}

inline std::string stringify(const Value& v) {
  if (v.is_null()) return "null";
  if (v.is_bool()) return std::get<bool>(v.data) ? "true" : "false";
  if (v.is_number()) return format_double(std::get<double>(v.data));
  if (v.is_string()) return "\"" + escape(std::get<std::string>(v.data)) + "\"";
  if (v.is_array()) return stringify_array(v.array());
  return stringify_object(v.object());
}

class Parser {
public:
  explicit Parser(std::string s) : src(std::move(s)) {}

  Value parse() {
    skip_ws();
    Value v = parse_value();
    skip_ws();
    if (pos != src.size()) throw std::runtime_error("Unexpected trailing JSON");
    return v;
  }

private:
  std::string src;
  size_t pos = 0;

  void skip_ws() {
    while (pos < src.size() && std::isspace(static_cast<unsigned char>(src[pos]))) ++pos;
  }
  char peek() const { return pos < src.size() ? src[pos] : '\0'; }
  char get() {
    if (pos >= src.size()) throw std::runtime_error("Unexpected end of JSON");
    return src[pos++];
  }
  void expect(char c) {
    if (get() != c) throw std::runtime_error(std::string("Expected ") + c);
  }
  int hex_digit(char c) const {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    throw std::runtime_error("Invalid JSON unicode escape");
  }
  unsigned parse_hex4() {
    if (pos + 4 > src.size()) throw std::runtime_error("Truncated JSON unicode escape");
    unsigned code = 0;
    for (int i = 0; i < 4; ++i) code = (code << 4) | static_cast<unsigned>(hex_digit(src[pos++]));
    return code;
  }
  void append_utf8(std::string& out, unsigned code) const {
    if (code <= 0x7F) {
      out.push_back(static_cast<char>(code));
    } else if (code <= 0x7FF) {
      out.push_back(static_cast<char>(0xC0 | ((code >> 6) & 0x1F)));
      out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    } else if (code <= 0xFFFF) {
      out.push_back(static_cast<char>(0xE0 | ((code >> 12) & 0x0F)));
      out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    } else if (code <= 0x10FFFF) {
      out.push_back(static_cast<char>(0xF0 | ((code >> 18) & 0x07)));
      out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    } else {
      throw std::runtime_error("Invalid JSON unicode code point");
    }
  }
  bool consume(const std::string& s) {
    if (src.compare(pos, s.size(), s) == 0) {
      pos += s.size();
      return true;
    }
    return false;
  }
  Value parse_value() {
    skip_ws();
    char c = peek();
    if (c == '"') return parse_string();
    if (c == '{') return parse_object();
    if (c == '[') return parse_array();
    if (c == 't' && consume("true")) return Value(true);
    if (c == 'f' && consume("false")) return Value(false);
    if (c == 'n' && consume("null")) return Value(nullptr);
    return parse_number();
  }
  Value parse_string() {
    expect('"');
    std::string out;
    while (true) {
      char c = get();
      if (c == '"') break;
      if (c == '\\') {
        char e = get();
        switch (e) {
          case '"': out += '"'; break;
          case '\\': out += '\\'; break;
          case '/': out += '/'; break;
          case 'b': out += '\b'; break;
          case 'f': out += '\f'; break;
          case 'n': out += '\n'; break;
          case 'r': out += '\r'; break;
          case 't': out += '\t'; break;
          case 'u':
            append_utf8(out, parse_hex4());
            break;
          default: throw std::runtime_error("Invalid JSON escape");
        }
      } else {
        out += c;
      }
    }
    return out;
  }
  Value parse_number() {
    const char* start = src.c_str() + pos;
    char* end = nullptr;
    double n = std::strtod(start, &end);
    if (end == start) throw std::runtime_error("Expected JSON value");
    pos += static_cast<size_t>(end - start);
    return n;
  }
  Value parse_array() {
    expect('[');
    Value::Array arr;
    skip_ws();
    if (peek() == ']') { get(); return arr; }
    while (true) {
      arr.push_back(parse_value());
      skip_ws();
      char c = get();
      if (c == ']') break;
      if (c != ',') throw std::runtime_error("Expected comma in array");
    }
    return arr;
  }
  Value parse_object() {
    expect('{');
    Value::Object obj;
    skip_ws();
    if (peek() == '}') { get(); return obj; }
    while (true) {
      skip_ws();
      std::string key = parse_string().as_string();
      skip_ws();
      expect(':');
      obj[key] = parse_value();
      skip_ws();
      char c = get();
      if (c == '}') break;
      if (c != ',') throw std::runtime_error("Expected comma in object");
    }
    return obj;
  }
};

inline Value parse(const std::string& s) {
  return Parser(s).parse();
}

inline Value::Array numbers(const std::vector<double>& xs) {
  Value::Array arr;
  for (double x : xs) arr.emplace_back(x);
  return arr;
}

inline std::vector<double> to_numbers(const Value& v) {
  std::vector<double> out;
  if (!v.is_array()) return out;
  for (const auto& item : v.array()) out.push_back(item.as_number());
  return out;
}

// The string counterparts of the two above. Added when mergeBatch entries
// started carrying a set of contributing sequence ids instead of one, which put
// the same hand-rolled push_back loop in the RE serialiser, the PE dispatch
// ledger and the PE trigger envelope.
inline Value::Array strings(const std::vector<std::string>& xs) {
  Value::Array arr;
  for (const auto& x : xs) arr.emplace_back(x);
  return arr;
}

inline std::vector<std::string> to_strings(const Value& v) {
  std::vector<std::string> out;
  if (!v.is_array()) return out;
  for (const auto& item : v.array()) if (item.is_string()) out.push_back(item.as_string());
  return out;
}

} // namespace reality::json
