// SPDX-License-Identifier: GPL-3.0-or-later
#include "identity.h"
#include <stdexcept>
#include <iostream>
using namespace controlfloor;
void require(bool condition) { if (!condition) throw std::runtime_error("identity parser assertion failed"); }
void line(OCRResult &ocr, const std::string &text, int y) { ocr.words.push_back({text, 10, y, 500, y+20}); }
int main() {
    OCRResult ocr;
    line(ocr, "Keyboard identifier (display only)", 0);
    line(ocr, "4523452345234", 40); require(keyboardID(ocr).empty());
    line(ocr, "X=4523452345234=X", 80); require(keyboardID(ocr) == "4523452345234");
    line(ocr, "X=other=X", 120); require(keyboardID(ocr).empty());
    ocr = OCRResult(); line(ocr, "X=../danger=X", 0); require(keyboardID(ocr).empty());
    ocr = OCRResult(); line(ocr, "X = keyboard-01:ABC = X", 0); require(keyboardID(ocr) == "keyboard-01:ABC");
    // Words intentionally delivered in column order, as Tesseract does on About.
    ocr = OCRResult();
    ocr.words = {{"Name", 0, 0, 80, 20}, {"Model Name", 0, 40, 150, 60},
                 {"Serial Number", 0, 80, 150, 100}, {"Test phone", 200, 0, 350, 20},
                 {"iPhone SE", 200, 40, 350, 60}, {"TEST23456789", 200, 80, 350, 100}};
    auto id = aboutIdentity(ocr);
    require(id.complete() && id.name == "Test phone" && id.serial == "TEST23456789");
    ocr.words.back().text = "TEST2345?789"; require(!aboutIdentity(ocr).complete());
    ocr.words.back().text = "test23456789"; require(!aboutIdentity(ocr).complete());
    ocr.words.back().text = "TEST23456789";
    line(ocr, "Serial Number DIFFERENT123", 120); require(!aboutIdentity(ocr).complete());
    std::cout << "PASS decorated/ambiguous keyboard markers, column order, invalid and conflicting serials\n";
}
