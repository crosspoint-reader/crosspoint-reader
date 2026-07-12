#pragma once

class GfxRenderer;

typedef enum { TEXT_BLOCK, IMAGE_BLOCK } BlockType;

// a block of content in the html - either a paragraph or an image
class Block {
 public:
  virtual ~Block() = default;

  virtual BlockType getType() = 0;
  virtual bool isEmpty() = 0;
  virtual void finish() {}
};
