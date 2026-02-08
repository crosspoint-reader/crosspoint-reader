#include "test/test_harness.h"

// Serialization.h includes SdFat.h — our stub satisfies the FsFile overloads.
// We only test the std::iostream overloads here.
#include <sstream>

#include "lib/Serialization/Serialization.h"

void testWriteReadPodInt32() {
  std::stringstream ss;
  int32_t written = 42;
  serialization::writePod(ss, written);

  int32_t readBack = 0;
  ss.seekg(0);
  serialization::readPod(ss, readBack);
  ASSERT_EQ(readBack, 42);
}

void testWriteReadPodFloat() {
  std::stringstream ss;
  float written = 3.14f;
  serialization::writePod(ss, written);

  float readBack = 0.0f;
  ss.seekg(0);
  serialization::readPod(ss, readBack);
  ASSERT_NEAR(readBack, 3.14f, 0.001f);
}

void testWriteReadPodUint8() {
  std::stringstream ss;
  uint8_t written = 255;
  serialization::writePod(ss, written);

  uint8_t readBack = 0;
  ss.seekg(0);
  serialization::readPod(ss, readBack);
  ASSERT_EQ(readBack, 255u);
}

void testWriteReadString() {
  std::stringstream ss;
  const std::string written = "hello world";
  serialization::writeString(ss, written);

  std::string readBack;
  ss.seekg(0);
  serialization::readString(ss, readBack);
  ASSERT_STREQ(readBack, "hello world");
}

void testWriteReadEmptyString() {
  std::stringstream ss;
  const std::string written;
  serialization::writeString(ss, written);

  std::string readBack = "notempty";
  ss.seekg(0);
  serialization::readString(ss, readBack);
  ASSERT_TRUE(readBack.empty());
}

void testMultipleValues() {
  std::stringstream ss;
  serialization::writePod<int32_t>(ss, 1);
  serialization::writeString(ss, "two");
  serialization::writePod<float>(ss, 3.0f);

  ss.seekg(0);
  int32_t i;
  std::string s;
  float f;
  serialization::readPod(ss, i);
  serialization::readString(ss, s);
  serialization::readPod(ss, f);

  ASSERT_EQ(i, 1);
  ASSERT_STREQ(s, "two");
  ASSERT_NEAR(f, 3.0f, 0.001f);
}

int main() {
  std::cout << "SerializationTest\n";
  RUN_TEST(testWriteReadPodInt32);
  RUN_TEST(testWriteReadPodFloat);
  RUN_TEST(testWriteReadPodUint8);
  RUN_TEST(testWriteReadString);
  RUN_TEST(testWriteReadEmptyString);
  RUN_TEST(testMultipleValues);
  TEST_SUMMARY();
}
