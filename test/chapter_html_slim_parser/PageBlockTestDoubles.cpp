// Host-test doubles for the parts of Page.cpp / ImageBlock.cpp that the
// chapter parser test never exercises, but whose symbols still must exist:
// PageLine/PageImage/PageHorizontalRule are polymorphic (PageElement has pure
// virtual render()/serialize()), so instantiating them anywhere requires
// their vtables to resolve, even though nothing in these tests ever calls
// render() or serialize() on a page. Reimplementing them as no-ops here (this
// file's TU becomes the "key function" TU instead of the real Page.cpp)
// avoids pulling in FontCacheManager/DirectPixelWriter/ImageDecoderFactory,
// none of which are relevant to chapter-parsing/list-bullet behavior.
//
// ImageBlock's constructor IS called (ChapterHtmlSlimParser.cpp constructs
// one per <img>), so it's reimplemented verbatim from the real one; none of
// ImageBlock's other methods are ever called by these tests (no <img>
// fixtures) and are intentionally left undefined.
//
// ImageDecoderFactory::isFormatSupported/getDecoder and
// Epub::readItemContentsToStream are likewise only reachable from the <img>
// branch (never taken since no fixture contains an <img> tag), but the calls
// still exist in the compiled ChapterHtmlSlimParser.cpp object, so the
// symbols must resolve; a "no image is ever supported" stub is a safe
// default that would degrade gracefully even if a fixture did add one.

#include "Epub.h"
#include "Epub/Page.h"
#include "Epub/blocks/ImageBlock.h"
#include "Epub/converters/ImageDecoderFactory.h"
#include "Epub/converters/ImageDimsProbe.h"

ImageBlock::ImageBlock(const std::string& imagePath, const std::string& srcPath, const int16_t width,
                       const int16_t height)
    : imagePath(imagePath), srcPath(srcPath), width(width), height(height) {}

void PageLine::render(GfxRenderer& /*renderer*/, int /*fontId*/, int /*xOffset*/, int /*yOffset*/) {}
bool PageLine::serialize(HalFile& /*file*/) { return true; }

void PageImage::render(GfxRenderer& /*renderer*/, int /*fontId*/, int /*xOffset*/, int /*yOffset*/) {}
void PageImage::renderPlaceholder(GfxRenderer& /*renderer*/, int /*xOffset*/, int /*yOffset*/) const {}
bool PageImage::serialize(HalFile& /*file*/) { return true; }

void PageHorizontalRule::render(GfxRenderer& /*renderer*/, int /*fontId*/, int /*xOffset*/, int /*yOffset*/) {}
bool PageHorizontalRule::serialize(HalFile& /*file*/) { return true; }

bool ImageDecoderFactory::isFormatSupported(const std::string& /*imagePath*/) { return false; }
ImageToFramebufferDecoder* ImageDecoderFactory::getDecoder(const std::string& /*imagePath*/) { return nullptr; }

size_t ImageDimsProbe::write(uint8_t /*b*/) { return 0; }
size_t ImageDimsProbe::write(const uint8_t* /*data*/, size_t /*len*/) { return 0; }
bool ImageDimsProbe::getDimensions(ImageDimensions& /*out*/) const { return false; }

bool Epub::readItemContentsToStream(const std::string& /*itemHref*/, Print& /*out*/, size_t /*chunkSize*/,
                                    bool /*allowEarlyStop*/) const {
  return false;
}
