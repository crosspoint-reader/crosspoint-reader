#ifndef EPUB_H
#define EPUB_H

#include <string>
#include <vector>

namespace Epub {

class Epub {
public:
    Epub();
    ~Epub();

    void parse(const std::string& css);

private:
    std::vector<CssStyle*> m_styles;
};

}  // namespace Epub

#endif  // EPUB_H