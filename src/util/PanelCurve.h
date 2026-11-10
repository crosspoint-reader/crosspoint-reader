#pragma once

#include <cstdint>
#include <string>

// Tone curve for this panel, from the SD card: panel_curve.txt holds 256 integers 0..255 (whitespace or comma
// separated) — wanted on-screen tone → value fed to the 4-gray ditherer, the inverse of the measured response. Lets
// images be prepared flat, as for a computer screen. The KEINKT Calibrator page (github.com/MikalaiKryvusha/KEINKT)
// makes the file from a photo of the calibration card: KEINKT Calibrator, https://github.com/MikalaiKryvusha/KEINKT
// (calibrator/index.html).
namespace panelcurve {
// Curves live beside the firmware settings. Root paths remain readable for older configurations.
constexpr char ROOT_PATH[] = "/panel_curve.txt";
constexpr char PATH[] = "/.crosspoint/panel_curve.txt";

constexpr char BOOK_ROOT_PATH[] = "/panel_curve_book.txt";
constexpr char BOOK_PATH[] = "/.crosspoint/panel_curve_book.txt";

// Pictures and custom sleep images: identity when disabled or no valid card curve exists.
bool load(uint8_t table[256], uint32_t& hash);

// The books curve (images inside EPUBs): with «Калибровка книг» on, panel_curve_book.txt or identity;
// off — nullptr (books draw as stock). The table lives as long as the program; `hash` is 0 when off.
const uint8_t* bookCurve(uint32_t& hash);
// Drop both cached curves: the next draw reads the card files again. Called when «Калибровка картинок» or
// «Калибровка книг» is switched (on the device or over the web), so a new file on the card needs no restart.
void forget();
}  // namespace panelcurve
