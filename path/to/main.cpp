#include "Epub.h"

int main() {
    std::string css = ".orn {text-indent: 0; text-align: center; margin: 1em 0 1em 0}";
    Epub epub;
    epub.parse(css);
    return 0;
}