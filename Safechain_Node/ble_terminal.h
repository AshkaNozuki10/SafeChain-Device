#ifndef BLE_TERMINAL_H
#define BLE_TERMINAL_H

#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <esp_mac.h>    
#include "config.h"

class BLETerminal {
private:
    BLEServer *pServer;
    BLECharacteristic *pTxChar;
    bool connected;
    String deviceName;
    
    // Callback handlers (set by main)
    void (*onGPSInject)(float, float);
    void (*onCommand)(String);
    
public:
    BLETerminal();
    
    void init(const char* nodeName);
    void update();
    
    bool isConnected() const { return connected; }
    void send(const String &msg);
    
    // 👇 ADD THIS LINE
    String getMacAddress();
    
    // Register callbacks
    void setGPSInjectCallback(void (*cb)(float, float));
    void setCommandCallback(void (*cb)(String));
    
    // Called by BLE callbacks
    void handleConnect();
    void handleDisconnect();
    void handleData(String data);
};

#endif