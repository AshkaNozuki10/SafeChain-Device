#include "router.h"
#include <LoRa.h>

RepeaterRouter::RepeaterRouter() 
    : cacheIndex(0), isPending(false), 
      packetsReceived(0), packetsRelayed(0), packetsDropped(0) {
    memset(seenCache, 0, sizeof(seenCache));
     memset(recentEmergencies, 0, sizeof(recentEmergencies)); // <-- ADD THIS
}

void RepeaterRouter::init() {
    Serial.println("✅ Repeater Router Ready");
}

extern void notifyBLE(String msg);

bool RepeaterRouter::shouldRelay(const SafeChainPacket &pkt) {
    packetsReceived++;
    
    // 👇 FIX: Now we check ID, Sequence, AND the Message Type!
    if (isDuplicate(pkt.srcID, pkt.seqNum, pkt.msgType)) {
        Serial.println(">>> Duplicate detected");
        if (isPending && pendingPacket.seqNum == pkt.seqNum) {
            Serial.println(">>> SMART CANCEL: Overheard another repeater.");
            notifyBLE("🛑 CANCELLED: Overheard Repeater relaying Seq " + String(pkt.seqNum));
            isPending = false; 
        }
        packetsDropped++;
        return false;
    }
    
    // 2. Handle ACKs (CRITICAL: Do NOT return false here!)
    if (pkt.msgType == MSG_ACK) {
        Serial.println(">>> Overheard ACK flowing to Node");
        notifyBLE("✅ HEARD ACK: Gateway confirmed Seq " + String(pkt.seqNum));
        
        if (isPending && pendingPacket.seqNum == pkt.seqNum) {
            Serial.println(">>> SMART CANCEL: Gateway already ACKed this!");
            notifyBLE("🛑 CANCELLED: Gateway already ACKed Seq " + String(pkt.seqNum));
            isPending = false; 
        }
        // Notice we do NOT return false here. The Repeater MUST relay the ACK!
    }
    
    // 3. Check hop limit
    if (pkt.hopCount >= pkt.maxHop) {
        Serial.println(">>> Max hop reached");
        packetsDropped++;
        return false;
    }
    
    return true;
}

bool RepeaterRouter::isRecentEmergency(const SafeChainPacket &pkt) {
    // Only apply to emergencies
    if (pkt.msgType < EM_FIRE || pkt.msgType > EM_CRIME) {
        return false;
    }
    
    for (int i = 0; i < 10; i++) {
        if (strcmp(recentEmergencies[i].srcID, pkt.srcID) == 0 &&
            recentEmergencies[i].msgType == pkt.msgType &&
            (millis() - recentEmergencies[i].timestamp) < 30000) {
            return true;
        }
    }
    return false;
}

void RepeaterRouter::markRecentEmergency(const SafeChainPacket &pkt) {
    // Only mark emergencies
    if (pkt.msgType >= EM_FIRE && pkt.msgType <= EM_CRIME) {
        strncpy(recentEmergencies[dedupIndex].srcID, pkt.srcID, 5);
        recentEmergencies[dedupIndex].srcID[5] = '\0'; // Ensure null termination
        recentEmergencies[dedupIndex].msgType = pkt.msgType;
        recentEmergencies[dedupIndex].timestamp = millis();
        dedupIndex = (dedupIndex + 1) % 10;
    }
}

void RepeaterRouter::queueRelay(const SafeChainPacket &pkt) {
    memcpy(&pendingPacket, &pkt, sizeof(SafeChainPacket));
    
    if (!PacketBuilder::prepareRelay(pendingPacket)) {
        packetsDropped++;
        return;
    }
    
    markSeen(pkt.srcID, pkt.seqNum, pkt.msgType);
    relayTriggerTime = millis() + random(RELAY_MIN_DELAY, RELAY_MAX_DELAY);
    isPending = true;
    
    // Send to BLE
    String msg = "⏳ QUEUED: Node " + String(pkt.srcID) + " Seq " + String(pkt.seqNum) + " (Wait " + String(relayTriggerTime - millis()) + "ms)";
    Serial.println(msg);
    notifyBLE(msg);
}

void RepeaterRouter::update() {
    if (!isPending) return;
    if (millis() < relayTriggerTime) return;
    
    LoRa.beginPacket();
    LoRa.write((uint8_t*)&pendingPacket, sizeof(SafeChainPacket));
    LoRa.endPacket(); // Blocking send
    LoRa.receive();   // Turn ears back on
    
    isPending = false;
    packetsRelayed++;
    
    // Send to BLE
    String msg = "📡 RELAYED: Seq " + String(pendingPacket.seqNum) + " (Hop " + String(pendingPacket.hopCount) + ")";
    Serial.println(msg);
    notifyBLE(msg);
}

void RepeaterRouter::printStats() {
    String stats = "\n=== REPEATER STATS ===\n";
    stats += "Received: " + String(packetsReceived) + "\n";
    stats += "Relayed:  " + String(packetsRelayed) + "\n";
    stats += "Dropped:  " + String(packetsDropped) + "\n";
    
    if (packetsReceived > 0) {
        float relayRate = (float)packetsRelayed / packetsReceived * 100.0;
        stats += "Relay Rate: " + String(relayRate, 1) + "%\n";
    }
    
    Serial.println(stats);
    notifyBLE(stats);
}

bool RepeaterRouter::isDuplicate(const char* srcID, uint16_t seqNum, uint8_t msgType) {
    for (int i = 0; i < DUPLICATE_CACHE_SIZE; i++) {
        if (seenCache[i].seqNum == seqNum && 
            seenCache[i].msgType == msgType &&
            strcmp(seenCache[i].srcID, srcID) == 0) {
            return true;
        }
    }
    return false;
}

void RepeaterRouter::markSeen(const char* srcID, uint16_t seqNum, uint8_t msgType) {
    strncpy(seenCache[cacheIndex].srcID, srcID, 5);
    seenCache[cacheIndex].seqNum = seqNum;
    // 👇 FIX: Save the msgType
    seenCache[cacheIndex].msgType = msgType;
    cacheIndex = (cacheIndex + 1) % DUPLICATE_CACHE_SIZE;
}