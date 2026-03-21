#include "emergency.h"
#include <LoRa.h>
#include <OneButton.h>

// 👇 Bring the buttons over from the main file!
extern OneButton btnFlood;
extern OneButton btnFire;
extern OneButton btnCrime;

extern void flashRGB(int r, int g, int b, int times);  // Forward declare
extern BLETerminal ble;                                // Forward declare BLE for terminal messages

EmergencyManager::EmergencyManager() 
    : state(EM_STATE_IDLE), retryCount(0), lastSendTime(0) {}

void EmergencyManager::init(const char* id) {
    nodeID = id;
}

void EmergencyManager::trigger(EmergencyType type, float lat, float lon, uint8_t batt) {
    PacketBuilder::build(pendingPacket, nodeID, MSG_EMERGENCY, 
                         type, lat, lon, batt);
    
    // If it is a SAFE signal, "Fire and Forget"!
    if (type == EM_SAFE) {
        Serial.println("📡 SENDING: Safe Signal Broadcast");
        
        // Rapid-fire 3 times to guarantee delivery without waiting for ACKs
        for(int i = 0; i < 3; i++) {
            LoRa.beginPacket();
            LoRa.write((uint8_t*)&pendingPacket, sizeof(SafeChainPacket));
            LoRa.endPacket();
            delay(100); 
        }
        LoRa.receive();
        
        // INSTANTLY kill the emergency loop and silence the Node
        state = EM_STATE_IDLE; 
        
    } else {
        // Normal Emergency (Wait for ACK and run retry loops)
        state = EM_STATE_SENDING;
        retryCount = 0;
        transmit();
    }
}

void EmergencyManager::transmit() {
    String statusMsg = "📡 SENDING: Seq " + String(pendingPacket.seqNum);
    Serial.println(statusMsg);
    ble.send(statusMsg);
    
    // 1. ALARMS FIRST (Isolate the power draw)
    tone(PIN_BUZZER, 4000, 100); 
    delay(100); // Let the buzzer completely finish
    
    // Use a low-power color (Dim Cyan) instead of maximum-power White!
    flashRGB(0, 50, 50, 2); 
    
    // 2. TRANSMIT SECOND (Now 100% of the battery power can go to the LoRa antenna)
    LoRa.beginPacket();
    LoRa.write((uint8_t*)&pendingPacket, sizeof(SafeChainPacket));
    LoRa.endPacket(); // Blocking send
    
    LoRa.receive(); // Turn ears back on
    
    state = EM_STATE_WAITING_ACK;
    lastSendTime = millis();


    // String statusMsg = "📡 SENDING: Seq " + String(pendingPacket.seqNum);
    // Serial.println(statusMsg);
    // ble.send(statusMsg);
    
    // // 👇 LOUD, PIERCING BEEP FOR SENDING (High 4000Hz tone)
    // tone(PIN_BUZZER, 4000, 200); 
    
    // flashRGB(255, 255, 255, 2);  // White flash
    
    // LoRa.beginPacket();
    // LoRa.write((uint8_t*)&pendingPacket, sizeof(SafeChainPacket));
    // LoRa.endPacket();  // Blocking send
    
    // LoRa.receive();    // Turn ears back on to listen for ACK
    
    // state = EM_STATE_WAITING_ACK;
    // lastSendTime = millis();
    
}

void EmergencyManager::update() {
    if (state != EM_STATE_WAITING_ACK) return;
    
    // Check timeout
    if (millis() - lastSendTime > ACK_TIMEOUT_MS) {
        retry();
    }
}

void EmergencyManager::retry() {
    if (retryCount < MAX_RETRIES) {
        retryCount++;
        
        // Exponential backoff with jitter
        uint32_t minDelay = 4000 * (1 << (retryCount - 1));
        uint32_t maxDelay = minDelay * 2;
        uint32_t backoff = random(minDelay, maxDelay);
        
        String retryMsg = "⚠️ RETRY " + String(retryCount) + "/" + String(MAX_RETRIES) + 
                          " (Wait: " + String(backoff / 1000) + "s)";
        Serial.println(retryMsg);
        ble.send(retryMsg);
        
        flashRGB(255, 100, 0, 2); // Orange flash
        
        // 👇 THE NEW HEARTBEAT TIMER LOOP
        unsigned long startWait = millis();
        while (millis() - startWait < backoff) {
            // "Ba-bum" heartbeat sound (Low 800Hz tone)
            tone(PIN_BUZZER, 800, 50);
            delay(100);
            
            // Check if time is up before the second beat
            if (millis() - startWait >= backoff) break;
            tone(PIN_BUZZER, 800, 50); 
            
            // Gap between heartbeats (approx 850ms)
            unsigned long gapStart = millis();
            while (millis() - gapStart < 850) {
                
                // 👇 FIX 2: Keep the buttons alive while waiting!
                btnFlood.tick();
                btnFire.tick();
                btnCrime.tick();
                
                // If the user double-clicked SAFE, triggerSafe() instantly sets 
                // the state to IDLE. If we see IDLE, abort the retry immediately!
                if (state == EM_STATE_IDLE) return; 

                if (millis() - startWait >= backoff) break;
                delay(10);
            }
        }
        
        // Final safety catch just in case it was pressed at the last millisecond
        if (state == EM_STATE_IDLE) return;
        
        // Generate new sequence number
        pendingPacket.seqNum = PacketBuilder::seqCounter++;
        pendingPacket.crc = PacketBuilder::calcCRC16((uint8_t*)&pendingPacket, sizeof(SafeChainPacket) - 2);
        
        transmit();
    } else {
        fail();
    }
}

void EmergencyManager::fail() {
    Serial.println(">>> FAILED - No ACK after retries");
    // Send failure to the Bluetooth Terminal
    ble.send("❌ FAILED - No network acknowledgment received.");
    flashRGB(255, 0, 0, 5);  // Red
    // Sad "Failed" Low-Beep (from our previous buzzer upgrade)
    tone(PIN_BUZZER, 500, 1000);    
    state = EM_STATE_FAILED;
}

void EmergencyManager::confirm() {
    Serial.println(">>> ACK RECEIVED - SUCCESS!");
    // Send success to the Bluetooth Terminal
    ble.send("✅ ALERT CONFIRMED - Help is on the way!");
    flashRGB(0, 255, 0, 3);  // Green
    // Happy "Success" Double-Beep (from our previous buzzer upgrade)
    tone(PIN_BUZZER, 2500, 150); delay(200);
    state = EM_STATE_CONFIRMED;
}

void EmergencyManager::handleACK(const SafeChainPacket &ack) {
    if (state != EM_STATE_WAITING_ACK) return;
    if (ack.seqNum != pendingPacket.seqNum) return;
    
    confirm();
}