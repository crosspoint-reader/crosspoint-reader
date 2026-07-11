#include "CssStyle.h"

namespace Epub {

CssStyle::CssStyle(const std::string& selector, const std::string& style)
    : m_selector(selector), m_style(style) {}

CssStyle::~CssStyle() {}

std::string CssStyle::getSelector() const {
    return m_selector;
}

std::string CssStyle::getStyle() const {
    return m_style;
}

}  // namespace Epub