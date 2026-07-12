// test/tests/test_unit_converter.cpp
// Tests unit conversion logic
// Run: pio test -e native -f test_unit_converter

#include <unity.h>
#include <cmath>
#include <string>

// ---- extracted conversion logic ----

double convertLength(double value, double fromBase, double toBase) {
  return (value * fromBase) / toBase;
}

double celsiusToFahrenheit(double c) { return c * 9.0 / 5.0 + 32.0; }
double fahrenheitToCelsius(double f) { return (f - 32.0) * 5.0 / 9.0; }
double celsiusToKelvin(double c) { return c + 273.15; }
double kelvinToCelsius(double k) { return k - 273.15; }

double convertData(double value, double fromBase, double toBase) {
  return (value * fromBase) / toBase;
}

// Base units: meter, kg, m/s, byte
struct Unit { const char* name; double toBase; };

static const Unit lengthUnits[] = {
  {"m", 1.0}, {"km", 1000.0}, {"cm", 0.01}, {"mm", 0.001},
  {"mi", 1609.344}, {"yd", 0.9144}, {"ft", 0.3048}, {"in", 0.0254}
};

static const Unit weightUnits[] = {
  {"kg", 1.0}, {"g", 0.001}, {"mg", 0.000001},
  {"lb", 0.453592}, {"oz", 0.0283495}, {"t", 1000.0}
};

static const Unit dataUnits[] = {
  {"B", 1.0}, {"KB", 1024.0}, {"MB", 1048576.0},
  {"GB", 1073741824.0}, {"bit", 0.125}
};

#define ASSERT_NEAR(expected, actual, tolerance) \
  TEST_ASSERT_FLOAT_WITHIN(tolerance, expected, actual)

// ---- tests ----

void test_km_to_miles() {
  double result = convertLength(1.0, 1000.0, 1609.344);  // 1km in miles
  ASSERT_NEAR(0.62137, result, 0.001);
}

void test_miles_to_km() {
  double result = convertLength(1.0, 1609.344, 1000.0);  // 1mi in km
  ASSERT_NEAR(1.60934, result, 0.001);
}

void test_feet_to_meters() {
  double result = convertLength(1.0, 0.3048, 1.0);  // 1ft in m
  ASSERT_NEAR(0.3048, result, 0.0001);
}

void test_inch_to_cm() {
  double result = convertLength(1.0, 0.0254, 0.01);  // 1in in cm
  ASSERT_NEAR(2.54, result, 0.001);
}

void test_celsius_to_fahrenheit() {
  ASSERT_NEAR(32.0, celsiusToFahrenheit(0.0), 0.01);
  ASSERT_NEAR(212.0, celsiusToFahrenheit(100.0), 0.01);
  ASSERT_NEAR(98.6, celsiusToFahrenheit(37.0), 0.1);
}

void test_fahrenheit_to_celsius() {
  ASSERT_NEAR(0.0, fahrenheitToCelsius(32.0), 0.01);
  ASSERT_NEAR(100.0, fahrenheitToCelsius(212.0), 0.01);
  ASSERT_NEAR(-40.0, fahrenheitToCelsius(-40.0), 0.01);  // same in both!
}

void test_celsius_to_kelvin() {
  ASSERT_NEAR(273.15, celsiusToKelvin(0.0), 0.01);
  ASSERT_NEAR(373.15, celsiusToKelvin(100.0), 0.01);
  ASSERT_NEAR(0.0, celsiusToKelvin(-273.15), 0.01);
}

void test_kelvin_to_celsius() {
  ASSERT_NEAR(0.0, kelvinToCelsius(273.15), 0.01);
  ASSERT_NEAR(-273.15, kelvinToCelsius(0.0), 0.01);
}

void test_kg_to_lb() {
  double result = convertLength(1.0, 1.0, 0.453592);  // 1kg in lb
  ASSERT_NEAR(2.2046, result, 0.001);
}

void test_lb_to_kg() {
  double result = convertLength(1.0, 0.453592, 1.0);  // 1lb in kg
  ASSERT_NEAR(0.4536, result, 0.001);
}

void test_gb_to_mb() {
  double result = convertData(1.0, 1073741824.0, 1048576.0);  // 1GB in MB
  ASSERT_NEAR(1024.0, result, 0.01);
}

void test_mb_to_bytes() {
  double result = convertData(1.0, 1048576.0, 1.0);  // 1MB in bytes
  ASSERT_NEAR(1048576.0, result, 0.01);
}

void test_byte_to_bits() {
  double result = convertData(1.0, 1.0, 0.125);  // 1 byte in bits
  ASSERT_NEAR(8.0, result, 0.01);
}

void test_identity_conversions() {
  // Converting a unit to itself should return the same value
  for (int i = 0; i < 8; i++) {
    double r = convertLength(42.0, lengthUnits[i].toBase, lengthUnits[i].toBase);
    ASSERT_NEAR(42.0, r, 0.0001);
  }
}

void setUp() {}
void tearDown() {}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_km_to_miles);
  RUN_TEST(test_miles_to_km);
  RUN_TEST(test_feet_to_meters);
  RUN_TEST(test_inch_to_cm);
  RUN_TEST(test_celsius_to_fahrenheit);
  RUN_TEST(test_fahrenheit_to_celsius);
  RUN_TEST(test_celsius_to_kelvin);
  RUN_TEST(test_kelvin_to_celsius);
  RUN_TEST(test_kg_to_lb);
  RUN_TEST(test_lb_to_kg);
  RUN_TEST(test_gb_to_mb);
  RUN_TEST(test_mb_to_bytes);
  RUN_TEST(test_byte_to_bits);
  RUN_TEST(test_identity_conversions);
  return UNITY_END();
}
