#include "CssParser.h"
#include "CssStyle.h"

namespace Epub {

CssParser::CssParser() {}

CssParser::~CssParser() {}

void CssParser::parse(const std::string& css) {
    // Split the CSS into individual rules
    size_t pos = 0;
    while ((pos = css.find('}')) != std::string::npos) {
        std::string rule = css.substr(0, pos);
        css.erase(0, pos + 1);

        // Parse the selector and style
        size_t selectorEnd = rule.find('{');
        std::string selector = rule.substr(0, selectorEnd);
        std::string style = rule.substr(selectorEnd + 1);

        // Handle the "Justify" alignment
        if (style.find("text-align: justify") != std::string::npos) {
            // Apply the centering style
            style += " text-align: center";
        }

        // Create a new CssStyle object and add it to the list
        CssStyle* styleObj = new CssStyle(selector, style);
        m_styles.push_back(styleObj);
    }
}

}  // namespace Epub