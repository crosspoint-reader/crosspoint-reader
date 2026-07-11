#ifndef CSS_STYLE_H
#define CSS_STYLE_H

#include <string>

namespace Epub {

class CssStyle {
public:
    CssStyle(const std::string& selector, const std::string& style);
    ~CssStyle();

    std::string getSelector() const;
    std::string getStyle() const;

private:
    std::string m_selector;
    std::string m_style;
};

}  // namespace Epub

#endif  // CSS_STYLE_H