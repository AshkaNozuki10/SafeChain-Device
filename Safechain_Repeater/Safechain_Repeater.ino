#include <SPI.h>
#include <LoRa.h>
#include <Adafruit_NeoPixel.h>

#include "config.h"
#include "packet.h"
#include "router.h"
#include "storage.h"       // <-- FIX: Tells the main file what Storage is
#include "ble_terminal.h"

// === OBJECTS ===
RepeaterRouter router;
Adafruit_NeoPixel strip(1, PIN_RGB, NEO_GRB + NEO_KHZ800);
Storage storage;
BLETerminal ble;

// === STATS ===
unsigned long lastStatsTime = 0;
const unsigned long STATS_INTERVAL = 60000;  // Every 1 minute

unsigned long lastKeepAlive = 0;

// === BLE NOTIFICATION HELPER ===
void notifyBLE(String msg) {
    if (ble.isConnected()) {
        ble.send(msg);
    }
}

// === BLE COMMAND HANDLER ===
void onBLECommand(String cmd) {
    cmd.trim(); cmd.toLowerCase();
    if (cmd == "stats") router.printStats();
    else if (cmd == "ping") notifyBLE("PONG: Repeater is alive and listening!");
    else notifyBLE("Commands: stats, ping");
}

// === HELPERS ===
void setRGB(int r, int g, int b) {
    strip.setPixelColor(0, strip.Color(r, g, b));
    strip.show();
}

void flashRGB(int r, int g, int b, int times) {
    for (int i = 0; i < times; i++) {
        setRGB(r, g, b);
        delay(100);
        setRGB(0, 0, 0);
        delay(100);
    }
}

void setup() {
    Serial.begin(115200);
    delay(2000);
    
    for(int i = 0; i < 50 && !Serial; i++) {
        delay(100);
    }
    
    Serial.println("\n\n=== SAFECHAIN REPEATER v2.0 ===");
    Serial.flush();
    
    // Init Storage and BLE
    storage.init();
    ble.init(storage.getNodeID().c_str());
    ble.setCommandCallback(onBLECommand);

    // LED Init
    strip.begin();
    strip.setBrightness(50);
    flashRGB(50, 0, 50, 2);  // Purple boot
    
    // LoRa Init
    Serial.println(">>> Init LoRa...");
    Serial.flush();
    
    SPI.begin(SCK_PIN, MISO_PIN, MOSI_PIN, SS_PIN);
    LoRa.setPins(SS_PIN, RST_PIN, DIO0_PIN);
    
    if (!LoRa.begin(LORA_FREQ)) {
        Serial.println("❌ LoRa Init Failed!");
        Serial.flush();
        while (1) {
            flashRGB(255, 0, 0, 1);
            delay(1000);
        }
    }
    
    LoRa.setSpreadingFactor(LORA_SF);
    LoRa.setSignalBandwidth(LORA_BW);
    LoRa.setCodingRate4(LORA_CR);
    LoRa.setPreambleLength(LORA_PREAMBLE);
    LoRa.enableCrc();
    LoRa.setSyncWord(LORA_SYNCWORD);
    LoRa.setTxPower(LORA_TXPOWER);

    LoRa.setGain(0);
    
    randomSeed(analogRead(0));
    
    Serial.println("✅ LoRa Ready (SF12, 14dBm)");
    Serial.flush();
    
    // Router Init
    router.init();
    
    setRGB(0, 0, 0);  // LED off to save power
    LoRa.receive();  // <--- ADD THIS HERE!
    
    Serial.println("\n=================================");
    Serial.println("   REPEATER ACTIVE");
    Serial.println("=================================");
    Serial.flush();
}

void loop() {
    // Router update
    router.update();

    // 👇 ADD THIS ENTIRE KEEP-ALIVE BLOCK
    if (millis() - lastKeepAlive > 12000) {  // Trigger every 12 seconds
        lastKeepAlive = millis();
        
        // 1. Build a dummy Heartbeat packet
        SafeChainPacket keepAlivePkt;
        PacketBuilder::build(keepAlivePkt, storage.getNodeID().c_str(), 
                             MSG_HEARTBEAT, EM_NONE, 0.0, 0.0, 100);
        
        // 2. Blast it at 20dBm to force a 120mA power draw from the battery!
        LoRa.beginPacket();
        LoRa.write((uint8_t*)&keepAlivePkt, sizeof(SafeChainPacket));
        LoRa.endPacket(); // Blocking send
        
        // 3. Immediately turn the listening ears back on
        LoRa.receive(); 
    }
    // 👆 END OF KEEP-ALIVE BLOCK
    
    // LoRa RX
    int packetSize = LoRa.parsePacket();
    if (packetSize == sizeof(SafeChainPacket)) {
        SafeChainPacket rxPkt;
        LoRa.readBytes((uint8_t*)&rxPkt, sizeof(SafeChainPacket));
        
        if (PacketBuilder::validate(rxPkt)) {
            rxPkt.rssi = LoRa.packetRssi();
            
            Serial.printf("\n[RX] ");
            PacketBuilder::print(rxPkt);
            
            // Purple flash on receive
            flashRGB(50, 0, 50, 1);
            
            // Check if should relay
            if (router.shouldRelay(rxPkt)) {
                // Magenta flash for relay queue
                flashRGB(255, 0, 255, 1);
                router.queueRelay(rxPkt);
            }
        } else {
            Serial.println(">>> CRC FAIL - Dropped");
        }
    }
    
    // Periodic stats
    if (millis() - lastStatsTime > STATS_INTERVAL) {
        lastStatsTime = millis();
        router.printStats();
    }
    
    // Serial commands
    if (Serial.available()) {
        String cmd = Serial.readStringUntil('\n');
        cmd.trim();
        cmd.toLowerCase();
        
        if (cmd == "stats") {
            router.printStats();
        } else if (cmd == "help") {
            Serial.println("\n--- COMMANDS ---");
            Serial.println("stats - Show statistics");
            Serial.println("help  - This menu");
        }
    }
    
    delay(10);
}

