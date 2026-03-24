#include "ble_terminal.h"
#include "config.h"

static BLETerminal *bleInstance = nullptr;  // For callbacks

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

BLETerminal::BLETerminal() 
    : pServer(nullptr), pTxChar(nullptr), connected(false),
      onGPSInject(nullptr), onCommand(nullptr) {
    bleInstance = this;
}

// 👇 ADD THIS NEW FUNCTION
String BLETerminal::getMacAddress() {
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_BT); // Read directly from the hardware chip!
    
    char macStr[18];
    sprintf(macStr, "%02X:%02X:%02X:%02X:%02X:%02X", 
            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
            
    return String(macStr);
}

void BLETerminal::init(const char* nodeName) {
    deviceName = String("SafeChain_") + nodeName;
    
    BLEDevice::init(deviceName.c_str());
    pServer = BLEDevice::createServer();
    pServer->setCallbacks(new MyServerCallbacks());
    
    // Nordic UART Service UUIDs (standard BLE UART)
    BLEService *pService = pServer->createService("6E400001-B5A3-F393-E0A9-E50E24DCCA9E");
    
    // RX Characteristic (Write from phone)
    BLECharacteristic *pRx = pService->createCharacteristic(
        "6E400002-B5A3-F393-E0A9-E50E24DCCA9E",
        BLECharacteristic::PROPERTY_WRITE);
    pRx->setCallbacks(new MyCallbacks());
    
    // TX Characteristic (Notify to phone)
    pTxChar = pService->createCharacteristic(
        "6E400003-B5A3-F393-E0A9-E50E24DCCA9E",
        BLECharacteristic::PROPERTY_NOTIFY);
    pTxChar->addDescriptor(new BLE2902());
    
    pService->start();
    
    BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
    pAdvertising->addServiceUUID("6E400001-B5A3-F393-E0A9-E50E24DCCA9E");
    pAdvertising->setScanResponse(true);

    // 👇 ADD THIS LINE: Frees up 5 bytes in the packet so the Name fits!
    pAdvertising->setMinPreferred(0x00);
    
    BLEDevice::startAdvertising();
    
    Serial.printf("✅ BLE Active: %s\n", deviceName.c_str());

    // 👇 ADD THIS LINE TO PRINT THE MAC ON BOOT
    Serial.printf("🔗 BLE MAC Address: %s\n", getMacAddress().c_str());
}



void BLETerminal::update() {
    // Non-blocking BLE updates handled by callbacks
}

void BLETerminal::send(const String &msg) {
    if (connected && pTxChar) {
        pTxChar->setValue(msg.c_str());
        pTxChar->notify();
    }
}

void BLETerminal::setGPSInjectCallback(void (*cb)(float, float)) {
    onGPSInject = cb;
}

void BLETerminal::setCommandCallback(void (*cb)(String)) {
    onCommand = cb;
}

void BLETerminal::handleConnect() {
    connected = true;
    Serial.println("\n>>> BLE: Connected");
    ledcWriteTone(PIN_BUZZER, 4000);
    ledcWrite(PIN_BUZZER, 128);
    delay(100);
    ledcWrite(PIN_BUZZER, 0);
}

void BLETerminal::handleDisconnect() {
    connected = false;
    Serial.println("\n>>> BLE: Disconnected");
    BLEDevice::startAdvertising();
}

void BLETerminal::handleData(String data) {
    int commaIndex = data.indexOf(',');
    
    // Check if GPS injection: "lat,lon"
    if (commaIndex > 0 && data.indexOf('.') > 0) {
        float lat = data.substring(0, commaIndex).toFloat();
        float lon = data.substring(commaIndex + 1).toFloat();
        
        if (onGPSInject) onGPSInject(lat, lon);
    } else {
        // Command
        if (onCommand) onCommand(data);
    }
}