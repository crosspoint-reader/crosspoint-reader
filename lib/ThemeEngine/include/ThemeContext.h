#pragma once

#include <cctype>
#include <cstdlib>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace ThemeEngine {

// Token types for expression parsing
struct ExpressionToken {
  enum Type { LITERAL, VARIABLE };
  Type type;
  std::string value;  // Literal text or variable name
};

// Pre-parsed expression for efficient repeated evaluation
struct Expression {
  std::vector<ExpressionToken> tokens;
  std::string rawExpr;  // Original expression string for complex evaluation

  bool empty() const { return tokens.empty() && rawExpr.empty(); }

  static Expression parse(const std::string& str) {
    Expression expr;
    expr.rawExpr = str;

    if (str.empty()) return expr;

    size_t start = 0;
    while (start < str.length()) {
      size_t open = str.find('{', start);
      if (open == std::string::npos) {
        // Remaining literal
        expr.tokens.push_back({ExpressionToken::LITERAL, str.substr(start)});
        break;
      }

      if (open > start) {
        // Literal before variable
        expr.tokens.push_back({ExpressionToken::LITERAL, str.substr(start, open - start)});
      }

      size_t close = str.find('}', open);
      if (close == std::string::npos) {
        // Broken brace, treat as literal
        expr.tokens.push_back({ExpressionToken::LITERAL, str.substr(open)});
        break;
      }

      // Variable
      expr.tokens.push_back({ExpressionToken::VARIABLE, str.substr(open + 1, close - open - 1)});
      start = close + 1;
    }
    return expr;
  }
};

class ThemeContext {
 private:
  std::map<std::string, std::string> strings;
  std::map<std::string, int> ints;
  std::map<std::string, bool> bools;

  const ThemeContext* parent = nullptr;

  // Helper to trim whitespace
  static std::string trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\n\r");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\n\r");
    return s.substr(start, end - start + 1);
  }

  // Helper to check if string is a number
  static bool isNumber(const std::string& s) {
    if (s.empty()) return false;
    size_t start = (s[0] == '-') ? 1 : 0;
    for (size_t i = start; i < s.length(); i++) {
      if (!isdigit(s[i])) return false;
    }
    return start < s.length();
  }

  // Helper to check if string is a hex number (0x..)
  static bool isHexNumber(const std::string& s) {
    if (s.size() < 3) return false;
    if (!(s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))) return false;
    for (size_t i = 2; i < s.length(); i++) {
      char c = s[i];
      if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return false;
    }
    return true;
  }

  static int parseInt(const std::string& s) {
    if (isHexNumber(s)) {
      return static_cast<int>(std::strtol(s.c_str(), nullptr, 16));
    }
    if (isNumber(s)) {
      return static_cast<int>(std::strtol(s.c_str(), nullptr, 10));
    }
    return 0;
  }

  static bool coerceBool(const std::string& s) {
    std::string v = trim(s);
    if (v.empty()) return false;
    if (v == "true" || v == "1") return true;
    if (v == "false" || v == "0") return false;
    if (isHexNumber(v) || isNumber(v)) return parseInt(v) != 0;
    return true;
  }

 public:
  explicit ThemeContext(const ThemeContext* parent = nullptr) : parent(parent) {}

  void setString(const std::string& key, const std::string& value) { strings[key] = value; }
  void setInt(const std::string& key, int value) { ints[key] = value; }
  void setBool(const std::string& key, bool value) { bools[key] = value; }

  // Helper to populate list data efficiently
  void setListItem(const std::string& listName, int index, const std::string& property, const std::string& value) {
    strings[listName + "." + std::to_string(index) + "." + property] = value;
  }
  void setListItem(const std::string& listName, int index, const std::string& property, int value) {
    ints[listName + "." + std::to_string(index) + "." + property] = value;
  }
  void setListItem(const std::string& listName, int index, const std::string& property, bool value) {
    bools[listName + "." + std::to_string(index) + "." + property] = value;
  }
  void setListItem(const std::string& listName, int index, const std::string& property, const char* value) {
    strings[listName + "." + std::to_string(index) + "." + property] = value;
  }

  std::string getString(const std::string& key, const std::string& defaultValue = "") const {
    auto it = strings.find(key);
    if (it != strings.end()) return it->second;
    if (parent) return parent->getString(key, defaultValue);
    return defaultValue;
  }

  int getInt(const std::string& key, int defaultValue = 0) const {
    auto it = ints.find(key);
    if (it != ints.end()) return it->second;
    if (parent) return parent->getInt(key, defaultValue);
    return defaultValue;
  }

  bool getBool(const std::string& key, bool defaultValue = false) const {
    auto it = bools.find(key);
    if (it != bools.end()) return it->second;
    if (parent) return parent->getBool(key, defaultValue);
    return defaultValue;
  }

  bool hasKey(const std::string& key) const {
    if (strings.count(key) || ints.count(key) || bools.count(key)) return true;
    if (parent) return parent->hasKey(key);
    return false;
  }

  // Get any value as string
  std::string getAnyAsString(const std::string& key) const {
    // Check strings first
    auto sit = strings.find(key);
    if (sit != strings.end()) return sit->second;

    // Check ints
    auto iit = ints.find(key);
    if (iit != ints.end()) return std::to_string(iit->second);

    // Check bools
    auto bit = bools.find(key);
    if (bit != bools.end()) return bit->second ? "true" : "false";

    // Check parent
    if (parent) return parent->getAnyAsString(key);

    return "";
  }

  bool getAnyAsBool(const std::string& key, bool defaultValue = false) const {
    auto bit = bools.find(key);
    if (bit != bools.end()) return bit->second;

    auto iit = ints.find(key);
    if (iit != ints.end()) return iit->second != 0;

    auto sit = strings.find(key);
    if (sit != strings.end()) return coerceBool(sit->second);

    if (parent) return parent->getAnyAsBool(key, defaultValue);
    return defaultValue;
  }

  int getAnyAsInt(const std::string& key, int defaultValue = 0) const {
    auto iit = ints.find(key);
    if (iit != ints.end()) return iit->second;

    auto bit = bools.find(key);
    if (bit != bools.end()) return bit->second ? 1 : 0;

    auto sit = strings.find(key);
    if (sit != strings.end()) return parseInt(sit->second);

    if (parent) return parent->getAnyAsInt(key, defaultValue);
    return defaultValue;
  }

  // Evaluate a complex boolean expression
  // Supports: !, &&, ||, ==, !=, <, >, <=, >=, parentheses
  bool evaluateBool(const std::string& expression) const {
    std::string expr = trim(expression);
    if (expr.empty()) return false;

    // Handle literal true/false
    if (expr == "true" || expr == "1") return true;
    if (expr == "false" || expr == "0") return false;

    // Handle {var} wrapper
    if (expr.size() > 2 && expr.front() == '{' && expr.back() == '}') {
      expr = trim(expr.substr(1, expr.size() - 2));
    }

    struct Token {
      enum Type { Identifier, Number, String, Op, LParen, RParen, End };
      Type type;
      std::string text;
    };

    struct Tokenizer {
      const std::string& s;
      size_t pos = 0;
      Token peeked{Token::End, ""};
      bool hasPeek = false;

      explicit Tokenizer(const std::string& input) : s(input) {}

      static std::string trimCopy(const std::string& in) {
        size_t start = in.find_first_not_of(" \t\n\r");
        if (start == std::string::npos) return "";
        size_t end = in.find_last_not_of(" \t\n\r");
        return in.substr(start, end - start + 1);
      }

      void skipWs() {
        while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\t' || s[pos] == '\n' || s[pos] == '\r')) {
          pos++;
        }
      }

      Token readToken() {
        skipWs();
        if (pos >= s.size()) return {Token::End, ""};
        char c = s[pos];

        if (c == '(') {
          pos++;
          return {Token::LParen, "("};
        }
        if (c == ')') {
          pos++;
          return {Token::RParen, ")"};
        }

        if (c == '{') {
          size_t end = s.find('}', pos + 1);
          std::string inner;
          if (end == std::string::npos) {
            inner = s.substr(pos + 1);
            pos = s.size();
          } else {
            inner = s.substr(pos + 1, end - pos - 1);
            pos = end + 1;
          }
          return {Token::Identifier, trimCopy(inner)};
        }

        if (c == '"' || c == '\'') {
          char quote = c;
          pos++;
          std::string out;
          while (pos < s.size()) {
            char ch = s[pos++];
            if (ch == '\\' && pos < s.size()) {
              out.push_back(s[pos++]);
              continue;
            }
            if (ch == quote) break;
            out.push_back(ch);
          }
          return {Token::String, out};
        }

        // Operators
        if (pos + 1 < s.size()) {
          std::string two = s.substr(pos, 2);
          if (two == "&&" || two == "||" || two == "==" || two == "!=" || two == "<=" || two == ">=") {
            pos += 2;
            return {Token::Op, two};
          }
        }
        if (c == '!' || c == '<' || c == '>') {
          pos++;
          return {Token::Op, std::string(1, c)};
        }

        // Number (decimal or hex)
        if (isdigit(c) || (c == '-' && pos + 1 < s.size() && isdigit(s[pos + 1]))) {
          size_t start = pos;
          pos++;
          if (pos + 1 < s.size() && s[start] == '0' && (s[pos] == 'x' || s[pos] == 'X')) {
            pos++;  // consume x
            while (pos < s.size() && isxdigit(s[pos])) pos++;
          } else {
            while (pos < s.size() && isdigit(s[pos])) pos++;
          }
          return {Token::Number, s.substr(start, pos - start)};
        }

        // Identifier
        if (isalpha(c) || c == '_' || c == '.') {
          size_t start = pos;
          pos++;
          while (pos < s.size()) {
            char ch = s[pos];
            if (isalnum(ch) || ch == '_' || ch == '.') {
              pos++;
              continue;
            }
            break;
          }
          return {Token::Identifier, s.substr(start, pos - start)};
        }

        // Unknown char, skip
        pos++;
        return readToken();
      }

      Token next() {
        if (hasPeek) {
          hasPeek = false;
          return peeked;
        }
        return readToken();
      }

      Token peek() {
        if (!hasPeek) {
          peeked = readToken();
          hasPeek = true;
        }
        return peeked;
      }
    };

    Tokenizer tz(expr);

    std::function<bool()> parseOr;
    std::function<bool()> parseAnd;
    std::function<bool()> parseNot;
    std::function<bool()> parseComparison;
    std::function<std::string()> parseValue;

    parseValue = [&]() -> std::string {
      Token t = tz.next();
      if (t.type == Token::LParen) {
        bool inner = parseOr();
        Token close = tz.next();
        if (close.type != Token::RParen) {
          // best-effort: no-op
        }
        return inner ? "true" : "false";
      }
      if (t.type == Token::String) {
        return "'" + t.text + "'";
      }
      if (t.type == Token::Number) {
        return t.text;
      }
      if (t.type == Token::Identifier) {
        return t.text;
      }
      return "";
    };

    auto isComparisonOp = [](const Token& t) {
      if (t.type != Token::Op) return false;
      return t.text == "==" || t.text == "!=" || t.text == "<" || t.text == ">" || t.text == "<=" || t.text == ">=";
    };

    parseComparison = [&]() -> bool {
      std::string left = parseValue();
      Token op = tz.peek();
      if (isComparisonOp(op)) {
        tz.next();
        std::string right = parseValue();
        int cmp = compareValues(left, right);
        if (op.text == "==") return cmp == 0;
        if (op.text == "!=") return cmp != 0;
        if (op.text == "<") return cmp < 0;
        if (op.text == ">") return cmp > 0;
        if (op.text == "<=") return cmp <= 0;
        if (op.text == ">=") return cmp >= 0;
        return false;
      }
      return coerceBool(resolveValue(left));
    };

    parseNot = [&]() -> bool {
      Token t = tz.peek();
      if (t.type == Token::Op && t.text == "!") {
        tz.next();
        return !parseNot();
      }
      return parseComparison();
    };

    parseAnd = [&]() -> bool {
      bool value = parseNot();
      while (true) {
        Token t = tz.peek();
        if (t.type == Token::Op && t.text == "&&") {
          tz.next();
          value = value && parseNot();
          continue;
        }
        break;
      }
      return value;
    };

    parseOr = [&]() -> bool {
      bool value = parseAnd();
      while (true) {
        Token t = tz.peek();
        if (t.type == Token::Op && t.text == "||") {
          tz.next();
          value = value || parseAnd();
          continue;
        }
        break;
      }
      return value;
    };

    return parseOr();
  }

  // Compare two values (handles variables, numbers, strings)
  int compareValues(const std::string& left, const std::string& right) const {
    std::string leftVal = resolveValue(left);
    std::string rightVal = resolveValue(right);

    // Try numeric comparison
    if ((isNumber(leftVal) || isHexNumber(leftVal)) && (isNumber(rightVal) || isHexNumber(rightVal))) {
      int l = parseInt(leftVal);
      int r = parseInt(rightVal);
      return (l < r) ? -1 : (l > r) ? 1 : 0;
    }

    // String comparison
    return leftVal.compare(rightVal);
  }

  // Resolve a value (variable name -> value, or literal)
  std::string resolveValue(const std::string& val) const {
    std::string v = trim(val);

    // Remove quotes for string literals
    if (v.size() >= 2 && v.front() == '"' && v.back() == '"') {
      return v.substr(1, v.size() - 2);
    }
    if (v.size() >= 2 && v.front() == '\'' && v.back() == '\'') {
      return v.substr(1, v.size() - 2);
    }

    // If it's a number, return as-is
    if (isNumber(v)) return v;

    // Check for hex color literals (0x00, 0xFF, etc.)
    if (isHexNumber(v)) {
      return v;
    }

    // Check for known color names - return as-is
    if (v == "black" || v == "white" || v == "gray" || v == "grey") {
      return v;
    }

    // Check for boolean literals
    if (v == "true" || v == "false" || v == "1" || v == "0") {
      return v;
    }

    // Try to look up as variable
    std::string varName = v;
    if (varName.size() >= 2 && varName.front() == '{' && varName.back() == '}') {
      varName = trim(varName.substr(1, varName.size() - 2));
    }

    if (hasKey(varName)) {
      return getAnyAsString(varName);
    }

    // Return as literal if not found as variable
    return v;
  }

  // Evaluate a string expression with variable substitution
  std::string evaluatestring(const Expression& expr) const {
    if (expr.empty()) return "";

    std::string result;
    for (const auto& token : expr.tokens) {
      if (token.type == ExpressionToken::LITERAL) {
        result += token.value;
      } else {
        // Variable lookup - check for comparison expressions inside
        std::string varName = token.value;

        // If the variable contains comparison operators, evaluate as condition
        if (varName.find("==") != std::string::npos || varName.find("!=") != std::string::npos ||
            varName.find("&&") != std::string::npos || varName.find("||") != std::string::npos) {
          result += evaluateBool(varName) ? "true" : "false";
          continue;
        }

        // Handle ternary: condition ? trueVal : falseVal
        size_t qPos = varName.find('?');
        if (qPos != std::string::npos) {
          size_t cPos = varName.find(':', qPos);
          if (cPos != std::string::npos) {
            std::string condition = trim(varName.substr(0, qPos));
            std::string trueVal = trim(varName.substr(qPos + 1, cPos - qPos - 1));
            std::string falseVal = trim(varName.substr(cPos + 1));

            bool condResult = evaluateBool(condition);
            result += resolveValue(condResult ? trueVal : falseVal);
            continue;
          }
        }

        // Normal variable lookup
        std::string strVal = getAnyAsString(varName);
        result += strVal;
      }
    }
    return result;
  }

  // Legacy method for backward compatibility
  std::string evaluateString(const std::string& expression) const {
    if (expression.empty()) return "";
    Expression expr = Expression::parse(expression);
    return evaluatestring(expr);
  }
};

}  // namespace ThemeEngine
