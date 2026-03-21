#include "led_manager.h"

LEDManager::LEDManager() : strip(1, PIN_RGB, NEO_GRB + NEO_KHZ800) {}

void LEDManager::init() {
    strip.begin();
    strip.setBrightness(50);
    off();
}

void LEDManager::setColor(int r, int g, int b) {
    strip.setPixelColor(0, strip.Color(r, g, b));
    strip.show();
}

void LEDManager::flash(int r, int g, int b, int times) {
    for (int i = 0; i < times; i++) {
        setColor(r, g, b);
        delay(100);
        off();
        delay(100);
    }
}

void LEDManager::updateStatus(bool bleConn, bool gpsValid) {
    if (bleConn) setColor(0, 0, 50);        // Dim Blue
    else if (gpsValid) setColor(0, 50, 0);  // Dim Green
    else off();
}

void LEDManager::off() {
    setColor(0, 0, 0);
}