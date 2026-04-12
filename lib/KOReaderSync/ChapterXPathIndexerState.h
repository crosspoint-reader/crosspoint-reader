#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "ChapterXPathIndexerInternal.h"

namespace ChapterXPathIndexerInternal {

// Shared parser state used by both forward and reverse mappers.
// It centralizes DOM-stack bookkeeping and XPath reconstruction so each mapper
// only implements its own match/emit logic.

struct StackNode {
  std::string tag;
  int index = 1;
  // Reserved for future text-node heuristics; intentionally unused for now.
  bool hasText = false;
};

struct StackState {
  int skipDepth = -1;
  size_t totalTextBytes = 0;
  std::vector<StackNode> stack;
  std::vector<std::unordered_map<std::string, int>> siblingCounters;

  StackState() { siblingCounters.emplace_back(); }

  void pushElement(const XML_Char* rawName) {
    std::string name = toLowerStr(rawName ? rawName : "");
    const size_t depth = stack.size();
    if (siblingCounters.size() <= depth) {
      siblingCounters.resize(depth + 1);
    }
    const int sibIdx = ++siblingCounters[depth][name];
    stack.push_back({name, sibIdx, false});
    siblingCounters.emplace_back();
    if (skipDepth < 0 && isSkippableTag(name)) {
      skipDepth = static_cast<int>(stack.size()) - 1;
    }
  }

  void popElement() {
    if (stack.empty()) {
      return;
    }
    if (skipDepth == static_cast<int>(stack.size()) - 1) {
      skipDepth = -1;
    }
    stack.pop_back();
    if (!siblingCounters.empty()) {
      siblingCounters.pop_back();
    }
  }

  int bodyIdx() const {
    for (int i = static_cast<int>(stack.size()) - 1; i >= 0; i--) {
      if (stack[i].tag == "body") {
        return i;
      }
    }
    return -1;
  }

  bool insideBody() const { return bodyIdx() >= 0; }

  std::string currentXPath(const int spineIndex) const {
    const int bi = bodyIdx();
    std::string xpath = "/body/DocFragment[" + std::to_string(spineIndex + 1) + "]/body";
    if (bi < 0) {
      return xpath;
    }
    for (size_t i = static_cast<size_t>(bi + 1); i < stack.size(); i++) {
      xpath += "/" + stack[i].tag + "[" + std::to_string(stack[i].index) + "]";
    }
    return xpath;
  }

  bool shouldSkipText(const int len) const { return skipDepth >= 0 || len <= 0 || !insideBody(); }
};

template <typename StateT>
void XMLCALL parserStartCb(void* ud, const XML_Char* name, const XML_Char**) {
  static_cast<StateT*>(ud)->onStartElement(name);
}

template <typename StateT>
void XMLCALL parserEndCb(void* ud, const XML_Char*) {
  static_cast<StateT*>(ud)->onEndElement();
}

template <typename StateT>
void XMLCALL parserCharCb(void* ud, const XML_Char* text, const int len) {
  static_cast<StateT*>(ud)->onCharData(text, len);
}

template <typename StateT>
void XMLCALL parserDefaultCb(void* ud, const XML_Char* text, const int len) {
  if (isEntityRef(text, len)) {
    static_cast<StateT*>(ud)->onCharData(text, len);
  }
}

}  // namespace ChapterXPathIndexerInternal
