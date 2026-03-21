#include <SPI.h>
#include <LoRa.h>
#include "config.h"
#include "packet.h"
#include "event_manager.h"

// === OBJECTS ===
EventManager eventMgr;

// === STATS ===
unsigned long lastStatsTime = 0;
const unsigned long STATS_INTERVAL = 300000;  // Every 5 minutes

void setup() {
    Serial.begin(115200);
    delay(2000);
    
    for(int i = 0; i < 50 && !Serial; i++) {
        delay(100);
    }
    
    Serial.println("\n\n=== SAFECHAIN GATEWAY v2.0 ===");
    Serial.flush();
    
    // LoRa Init
    Serial.println(">>> Init LoRa...");
    Serial.flush();
    
    SPI.begin(SCK_PIN, MISO_PIN, MOSI_PIN, SS_PIN);
    LoRa.setPins(SS_PIN, RST_PIN, DIO0_PIN);
    
    if (!LoRa.begin(LORA_FREQ)) {
        Serial.println("❌ LoRa Init Failed!");
        Serial.flush();
        while (1) delay(1000);
    }
    
    LoRa.setSpreadingFactor(LORA_SF);
    LoRa.setSignalBandwidth(LORA_BW);
    LoRa.setCodingRate4(LORA_CR);
    LoRa.setPreambleLength(LORA_PREAMBLE);
    LoRa.enableCrc();
    LoRa.setSyncWord(LORA_SYNCWORD);
    LoRa.setGain(0);  // Max RX gain
    
    Serial.println("✅ LoRa Ready (SF12, Max Gain)");
    Serial.flush();

    LoRa.receive();    // <--- ADD THIS LINE!
    
    
    // Event Manager Init
    eventMgr.init();
    
    Serial.println("\n=================================");
    Serial.println("   GATEWAY LISTENING");
    Serial.println("=================================");
    Serial.println("Commands: stats | pdr | help");
    Serial.flush();
}

void loop() {
    // LoRa RX
    int packetSize = LoRa.parsePacket();
    if (packetSize) {  // IF WE HEARD ANYTHING AT ALL
        if (packetSize == sizeof(SafeChainPacket)) {
            SafeChainPacket rxPkt;
            LoRa.readBytes((uint8_t*)&rxPkt, sizeof(SafeChainPacket));
            
            if (PacketBuilder::validate(rxPkt)) {
                rxPkt.rssi = LoRa.packetRssi();
                eventMgr.processPacket(rxPkt);
            } else {
                Serial.println(">>> CRC FAIL - Corrupted packet");
            }
        } else {
            // THE DIAGNOSTIC CATCH:
            Serial.printf("\n>>> WARNING: Size Mismatch! Received: %d bytes, Expected: %d bytes\n", packetSize, sizeof(SafeChainPacket));
        }
    }
    // int packetSize = LoRa.parsePacket();
    // if (packetSize == sizeof(SafeChainPacket)) {
    //     SafeChainPacket rxPkt;
    //     LoRa.readBytes((uint8_t*)&rxPkt, sizeof(SafeChainPacket));
        
    //     if (PacketBuilder::validate(rxPkt)) {
    //         rxPkt.rssi = LoRa.packetRssi();
    //         eventMgr.processPacket(rxPkt);
    //     } else {
    //         Serial.println(">>> CRC FAIL - Corrupted packet");
    //     }
    // }
    
    // Periodic stats
    if (millis() - lastStatsTime > STATS_INTERVAL) {
        lastStatsTime = millis();
        eventMgr.printStats();
        eventMgr.printPDR();
    }
    
    // Serial commands
    if (Serial.available()) {
        String cmd = Serial.readStringUntil('\n');
        cmd.trim();
        cmd.toLowerCase();
        
        if (cmd == "stats") {
            eventMgr.printStats();
        } else if (cmd == "pdr") {
            eventMgr.printPDR();
        } else if (cmd == "help") {
            Serial.println("\n--- COMMANDS ---");
            Serial.println("stats - Show packet statistics");
            Serial.println("pdr   - Show PDR per node");
            Serial.println("help  - This menu");
        }
    }
    
    delay(10);
}