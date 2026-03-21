#ifndef BLE_TERMINAL_H
#define BLE_TERMINAL_H

#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

class BLETerminal {
private:
    BLEServer *pServer;
    BLECharacteristic *pTxChar;
    bool connected;
    String deviceName;
    void (*onCommand)(String);

public:
    BLETerminal();
    void init(const char* nodeName);
    bool isConnected() const { return connected; }
    void send(const String &msg);
    void setCommandCallback(void (*cb)(String));
    void handleConnect();
    void handleDisconnect();
    void handleData(String data);
};

static BLETerminal *bleInstance = nullptr;

class MyServerCallbacks: public BLEServerCallbacks {
    void onConnect(BLEServer* pServer) {
        if (bleInstance) bleInstance->handleConnect();
    }
    void onDisconnect(BLEServer* pServer) {
        if (bleInstance) bleInstance->handleDisconnect();
    }
};

class MyCallbacks: public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *pChar) {
        String value = pChar->getValue();
        if (bleInstance && value.length() > 0) {
            bleInstance->handleData(value);
        }
    }
};

BLETerminal::BLETerminal() : pServer(nullptr), pTxChar(nullptr), connected(false), onCommand(nullptr) {
    bleInstance = this;
}

void BLETerminal::init(const char* nodeName) {
    deviceName = String("SafeChain_") + nodeName;
    BLEDevice::init(deviceName.c_str());
    pServer = BLEDevice::createServer();
    pServer->setCallbacks(new MyServerCallbacks());
    
    BLEService *pService = pServer->createService("6E400001-B5A3-F393-E0A9-E50E24DCCA9E");
    
    BLECharacteristic *pRx = pService->createCharacteristic(
        "6E400002-B5A3-F393-E0A9-E50E24DCCA9E", BLECharacteristic::PROPERTY_WRITE);
    pRx->setCallbacks(new MyCallbacks());
    
    pTxChar = pService->createCharacteristic(
        "6E400003-B5A3-F393-E0A9-E50E24DCCA9E", BLECharacteristic::PROPERTY_NOTIFY);
    pTxChar->addDescriptor(new BLE2902());
    
    pService->start();
    BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
    pAdvertising->addServiceUUID("6E400001-B5A3-F393-E0A9-E50E24DCCA9E");
    pAdvertising->setScanResponse(true);
    BLEDevice::startAdvertising();
    Serial.printf("✅ BLE Active: %s\n", deviceName.c_str());
}

void BLETerminal::send(const String &msg) {
   if (connected && pTxChar) {
        // Add carriage return and newline for terminal formatting
        String outMsg = msg + "\r\n"; 
        int len = outMsg.length();
        int offset = 0;

        // Slice the long string into 20-byte BLE chunks
        while (offset < len) {
            int chunkSize = (len - offset > 20) ? 20 : (len - offset);
            
            pTxChar->setValue((uint8_t*)outMsg.c_str() + offset, chunkSize);
            pTxChar->notify();
            
            offset += chunkSize;
            
            // Critical: Give the ESP32 BLE radio 20ms to push the packet 
            // before shoving the next chunk into the buffer!
            delay(20); 
        }
    }
}

void BLETerminal::setCommandCallback(void (*cb)(String)) { onCommand = cb; }
void BLETerminal::handleConnect() { connected = true; Serial.println("\n>>> BLE: Connected"); }
void BLETerminal::handleDisconnect() { connected = false; Serial.println("\n>>> BLE: Disconnected"); BLEDevice::startAdvertising(); }
void BLETerminal::handleData(String data) { if (onCommand) onCommand(data); }

#endif