// UploadBuffer: what /upload and /api/fonts/upload gather their chunks in. Taken with nothrow new, so a heap without
// the block refuses the upload instead of aborting the device (-fno-exceptions), and kept or freed as the web server
// asks.
#include <UploadBuffer.h>
#include <gtest/gtest.h>

#include <cstdlib>
#include <new>
#include <vector>

namespace {
// The nothrow array news are refused while set, as on a heap without UploadBuffer::kBytes in one block.
bool refuseNothrowArrays = false;
size_t nothrowArrays = 0;
}  // namespace

void* operator new[](const std::size_t size) {
  if (void* block = std::malloc(size != 0 ? size : 1)) return block;
  throw std::bad_alloc();
}
void* operator new[](const std::size_t size, const std::nothrow_t&) noexcept {
  if (refuseNothrowArrays) return nullptr;
  ++nothrowArrays;
  return std::malloc(size != 0 ? size : 1);
}
void operator delete[](void* block) noexcept { std::free(block); }
void operator delete[](void* block, std::size_t) noexcept { std::free(block); }

namespace {

class UploadBufferTest : public ::testing::Test {
 protected:
  void SetUp() override {
    refuseNothrowArrays = false;
    nothrowArrays = 0;
  }
  void TearDown() override { refuseNothrowArrays = false; }
};

TEST_F(UploadBufferTest, AHeapWithoutTheBlockRefusesTheUpload) {
  UploadBuffer buffer;
  refuseNothrowArrays = true;

  EXPECT_FALSE(buffer.take());
  EXPECT_EQ(nullptr, buffer.data());
  EXPECT_EQ(0u, buffer.size());

  // The next upload asks the heap again.
  refuseNothrowArrays = false;
  EXPECT_TRUE(buffer.take());
  EXPECT_NE(nullptr, buffer.data());
}

TEST_F(UploadBufferTest, ChunksAreGatheredUntilTheBufferIsFull) {
  UploadBuffer buffer;
  ASSERT_TRUE(buffer.take());
  std::vector<uint8_t> chunk(1500);
  for (size_t i = 0; i < chunk.size(); ++i) chunk[i] = static_cast<uint8_t>(i);

  std::vector<uint8_t> written;
  size_t sent = 0;
  // Three chunks: the third fills the buffer and is taken in part, the rest after the buffer goes to the card.
  for (int c = 0; c < 3; ++c) {
    const uint8_t* src = chunk.data();
    size_t remaining = chunk.size();
    while (remaining > 0) {
      const size_t copied = buffer.fill(src, remaining);
      src += copied;
      remaining -= copied;
      if (buffer.full()) {
        written.insert(written.end(), buffer.data(), buffer.data() + buffer.size());
        buffer.clear();
      }
    }
    sent += chunk.size();
  }

  EXPECT_EQ(UploadBuffer::kBytes, written.size());
  EXPECT_EQ(sent - UploadBuffer::kBytes, buffer.size());
  for (size_t i = 0; i < written.size(); ++i) ASSERT_EQ(chunk[i % chunk.size()], written[i]) << i;
}

TEST_F(UploadBufferTest, ABufferKeptFromTheUploadBeforeIsReusedEmpty) {
  UploadBuffer buffer;
  ASSERT_TRUE(buffer.take());
  const uint8_t byte = 7;
  buffer.fill(&byte, 1);

  // An upload whose end never came (a dropped client) left the block: the next one takes it as it is, emptied.
  refuseNothrowArrays = true;
  EXPECT_TRUE(buffer.take());
  EXPECT_EQ(0u, buffer.size());
  EXPECT_EQ(1u, nothrowArrays);
}

TEST_F(UploadBufferTest, ReleaseFreesTheBlock) {
  UploadBuffer buffer;
  ASSERT_TRUE(buffer.take());
  buffer.release();

  EXPECT_EQ(nullptr, buffer.data());
  EXPECT_EQ(0u, buffer.size());
  ASSERT_TRUE(buffer.take());
  EXPECT_EQ(2u, nothrowArrays);  // taken again, not kept
}

}  // namespace
