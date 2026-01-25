#pragma once

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

 public:
  ThemeContext(const ThemeContext* parent = nullptr) : parent(parent) {}

  void setString(const std::string& key, const std::string& value) { strings[key] = value; }
  void setInt(const std::string& key, int value) { ints[key] = value; }
  void setBool(const std::string& key, bool value) { bools[key] = value; }

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
      expr = expr.substr(1, expr.size() - 2);
    }

    // Handle negation
    if (!expr.empty() && expr[0] == '!') {
      return !evaluateBool(expr.substr(1));
    }

    // Handle parentheses
    if (!expr.empty() && expr[0] == '(') {
      int depth = 1;
      size_t closePos = 1;
      while (closePos < expr.length() && depth > 0) {
        if (expr[closePos] == '(') depth++;
        if (expr[closePos] == ')') depth--;
        closePos++;
      }
      if (closePos <= expr.length()) {
        std::string inner = expr.substr(1, closePos - 2);
        std::string rest = trim(expr.substr(closePos));
        bool innerResult = evaluateBool(inner);

        // Check for && or ||
        if (rest.length() >= 2 && rest.substr(0, 2) == "&&") {
          return innerResult && evaluateBool(rest.substr(2));
        }
        if (rest.length() >= 2 && rest.substr(0, 2) == "||") {
          return innerResult || evaluateBool(rest.substr(2));
        }
        return innerResult;
      }
    }

    // Handle && and || (lowest precedence)
    size_t andPos = expr.find("&&");
    size_t orPos = expr.find("||");

    // Process || first (lower precedence than &&)
    if (orPos != std::string::npos && (andPos == std::string::npos || orPos < andPos)) {
      return evaluateBool(expr.substr(0, orPos)) || evaluateBool(expr.substr(orPos + 2));
    }
    if (andPos != std::string::npos) {
      return evaluateBool(expr.substr(0, andPos)) && evaluateBool(expr.substr(andPos + 2));
    }

    // Handle comparisons
    size_t eqPos = expr.find("==");
    if (eqPos != std::string::npos) {
      std::string left = trim(expr.substr(0, eqPos));
      std::string right = trim(expr.substr(eqPos + 2));
      return compareValues(left, right) == 0;
    }

    size_t nePos = expr.find("!=");
    if (nePos != std::string::npos) {
      std::string left = trim(expr.substr(0, nePos));
      std::string right = trim(expr.substr(nePos + 2));
      return compareValues(left, right) != 0;
    }

    size_t gePos = expr.find(">=");
    if (gePos != std::string::npos) {
      std::string left = trim(expr.substr(0, gePos));
      std::string right = trim(expr.substr(gePos + 2));
      return compareValues(left, right) >= 0;
    }

    size_t lePos = expr.find("<=");
    if (lePos != std::string::npos) {
      std::string left = trim(expr.substr(0, lePos));
      std::string right = trim(expr.substr(lePos + 2));
      return compareValues(left, right) <= 0;
    }

    size_t gtPos = expr.find('>');
    if (gtPos != std::string::npos) {
      std::string left = trim(expr.substr(0, gtPos));
      std::string right = trim(expr.substr(gtPos + 1));
      return compareValues(left, right) > 0;
    }

    size_t ltPos = expr.find('<');
    if (ltPos != std::string::npos) {
      std::string left = trim(expr.substr(0, ltPos));
      std::string right = trim(expr.substr(ltPos + 1));
      return compareValues(left, right) < 0;
    }

    // Simple variable lookup
    return getBool(expr, false);
  }

  // Compare two values (handles variables, numbers, strings)
  int compareValues(const std::string& left, const std::string& right) const {
    std::string leftVal = resolveValue(left);
    std::string rightVal = resolveValue(right);

    // Try numeric comparison
    if (isNumber(leftVal) && isNumber(rightVal)) {
      int l = std::stoi(leftVal);
      int r = std::stoi(rightVal);
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
    if (v.size() > 2 && v[0] == '0' && (v[1] == 'x' || v[1] == 'X')) {
      return v;
    }

    // Check for known color names - return as-is
    if (v == "black" || v == "white" || v == "gray" || v == "grey") {
      return v;
    }

    // Check for boolean literals
    if (v == "true" || v == "false") {
      return v;
    }

    // Try to look up as variable
    if (hasKey(v)) {
      return getAnyAsString(v);
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
