#include "Epub.h"
#include "CssParser.h"

namespace Epub {

Epub::Epub() {}

Epub::~Epub() {}

void Epub::parse(const std::string& css) {
    CssParser parser;
    parser.parse(css);
}

}  // namespace Epub