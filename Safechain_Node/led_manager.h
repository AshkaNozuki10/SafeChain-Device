#ifndef LED_MANAGER_H
#define LED_MANAGER_H

#include <Arduino.h>
#include <Adafruit_NeoPixel.h>
#include "config.h"

class LEDManager {
private:
    Adafruit_NeoPixel strip;
    
public:
    LEDManager();
    
    void init();
    void setColor(int r, int g, int b);
    void flash(int r, int g, int b, int times);
    void updateStatus(bool bleConn, bool gpsValid);
    void off();
};

#endif