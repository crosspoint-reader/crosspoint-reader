#include "OpdsSearchTemplate.h"

#include <cctype>

namespace {

// Strip a namespace prefix ("atom:searchTerms"), RFC 6570 modifiers
// (":n", "*") and the OpenSearch optional marker ("?") from a variable name.
std::string bareVarName(const std::string& var) {
  size_t begin = var.find(':');
  // "atom:searchTerms" has the colon before the name; ":n" prefix modifiers
  // have it after. Treat a colon followed by a digit as a modifier.
  if (begin != std::string::npos && (begin + 1 >= var.size() || !isdigit(static_cast<unsigned char>(var[begin + 1])))) {
    begin += 1;
  } else {
    begin = 0;
  }
  size_t end = var.size();
  while (end > begin && (var[end - 1] == '?' || var[end - 1] == '*')) --end;
  const size_t modifier = var.find(':', begin);
  if (modifier != std::string::npos && modifier < end) end = modifier;
  return var.substr(begin, end - begin);
}

bool isQueryVar(const std::string& name) { return name == "query" || name == "searchTerms" || name == "q"; }

}  // namespace

std::string expandOpdsSearchTemplate(const std::string& templateUrl, const std::string& encodedQuery) {
  std::string out;
  out.reserve(templateUrl.size() + encodedQuery.size());

  size_t pos = 0;
  while (pos < templateUrl.size()) {
    const size_t open = templateUrl.find('{', pos);
    if (open == std::string::npos) {
      out.append(templateUrl, pos, std::string::npos);
      break;
    }
    out.append(templateUrl, pos, open - pos);
    const size_t close = templateUrl.find('}', open + 1);
    if (close == std::string::npos) {
      // Unbalanced brace: keep the rest literally.
      out.append(templateUrl, open, std::string::npos);
      break;
    }
    const std::string expr = templateUrl.substr(open + 1, close - open - 1);
    pos = close + 1;

    if (!expr.empty() && (expr[0] == '?' || expr[0] == '&')) {
      // Form-style expansion: emit only the query variable.
      const char separator = expr[0];
      size_t varStart = 1;
      bool expanded = false;
      while (varStart <= expr.size() && !expanded) {
        size_t varEnd = expr.find(',', varStart);
        if (varEnd == std::string::npos) varEnd = expr.size();
        const std::string name = bareVarName(expr.substr(varStart, varEnd - varStart));
        if (isQueryVar(name)) {
          out += separator;
          out += name;
          out += '=';
          out += encodedQuery;
          expanded = true;
        }
        varStart = varEnd + 1;
      }
    } else {
      // Simple expression: the search term variable expands to the query,
      // anything else (optional OpenSearch parameters) to an empty string.
      if (isQueryVar(bareVarName(expr))) out += encodedQuery;
    }
  }
  return out;
}
