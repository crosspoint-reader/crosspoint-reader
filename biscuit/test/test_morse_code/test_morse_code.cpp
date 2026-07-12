// test/tests/test_morse_code.cpp
// Tests MorseCodeActivity encode/decode logic
// Run: pio test -e native -f test_morse_code

#include <unity.h>
#include <string>
#include <cctype>

// ---- extracted logic ----

static const char* morseTable[36] = {
    ".-","-...","-.-.","-..",".","..-.","--.","....","..",
    ".---","-.-",".-..","--","-.","---",".--.","--.-",".-.",
    "...","-","..-","...-",".--","-..-","-.--","--..",
    "-----",".----","..---","...--","....-",".....","-....","--...","---..",
    "----."
};
static const char morseChars[36] = {
    'A','B','C','D','E','F','G','H','I','J','K','L','M','N','O','P','Q','R',
    'S','T','U','V','W','X','Y','Z','0','1','2','3','4','5','6','7','8','9'
};

std::string textToMorse(const std::string& text) {
  std::string result;
  for (char c : text) {
    if (c == ' ') { result += "/ "; continue; }
    char upper = toupper(c);
    for (int i = 0; i < 36; i++) {
      if (morseChars[i] == upper) {
        if (!result.empty() && result.back() != ' ') result += ' ';
        result += morseTable[i];
        break;
      }
    }
  }
  return result;
}

char morseToChar(const std::string& morse) {
  for (int i = 0; i < 36; i++) {
    if (morse == morseTable[i]) return morseChars[i];
  }
  return '?';
}

// ---- tests ----

void test_encode_sos() {
  TEST_ASSERT_EQUAL_STRING("... --- ...", textToMorse("SOS").c_str());
}

void test_encode_hello() {
  TEST_ASSERT_EQUAL_STRING(".... . .-.. .-.. ---", textToMorse("HELLO").c_str());
}

void test_encode_with_space() {
  std::string result = textToMorse("HI MOM");
  TEST_ASSERT_TRUE(result.find("/ ") != std::string::npos);  // word separator
}

void test_encode_numbers() {
  TEST_ASSERT_EQUAL_STRING(".---- ..--- ...--", textToMorse("123").c_str());
}

void test_encode_lowercase() {
  TEST_ASSERT_EQUAL_STRING("... --- ...", textToMorse("sos").c_str());
}

void test_decode_single_chars() {
  TEST_ASSERT_EQUAL('A', morseToChar(".-"));
  TEST_ASSERT_EQUAL('B', morseToChar("-..."));
  TEST_ASSERT_EQUAL('S', morseToChar("..."));
  TEST_ASSERT_EQUAL('O', morseToChar("---"));
  TEST_ASSERT_EQUAL('0', morseToChar("-----"));
  TEST_ASSERT_EQUAL('9', morseToChar("----."));
}

void test_decode_invalid() {
  TEST_ASSERT_EQUAL('?', morseToChar("........"));
  TEST_ASSERT_EQUAL('?', morseToChar(""));
}

void test_roundtrip_alphabet() {
  for (int i = 0; i < 26; i++) {
    std::string encoded = morseTable[i];
    char decoded = morseToChar(encoded);
    TEST_ASSERT_EQUAL(morseChars[i], decoded);
  }
}

void test_roundtrip_digits() {
  for (int i = 26; i < 36; i++) {
    char decoded = morseToChar(morseTable[i]);
    TEST_ASSERT_EQUAL(morseChars[i], decoded);
  }
}

void setUp() {}
void tearDown() {}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_encode_sos);
  RUN_TEST(test_encode_hello);
  RUN_TEST(test_encode_with_space);
  RUN_TEST(test_encode_numbers);
  RUN_TEST(test_encode_lowercase);
  RUN_TEST(test_decode_single_chars);
  RUN_TEST(test_decode_invalid);
  RUN_TEST(test_roundtrip_alphabet);
  RUN_TEST(test_roundtrip_digits);
  return UNITY_END();
}
