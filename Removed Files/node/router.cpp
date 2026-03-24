#include "router.h"
#include <LoRa.h>

Router::Router() : cacheIndex(0), relayEnabled(false), isPending(false) {
    memset(seenCache, 0, sizeof(seenCache));
}

void Router::init(const char* nodeID, bool enableRelay) {
    myNodeID = String(nodeID);
    relayEnabled = enableRelay;
}

void Router::setRelayEnabled(bool enabled) {
    relayEnabled = enabled;
}

bool Router::shouldRelay(const SafeChainPacket &pkt) {
    if (!relayEnabled) return false;
    if (isOwnPacket(pkt)) return false;
    if (isDuplicate(pkt.seqNum)) return false;
    if (pkt.hopCount >= pkt.maxHop) return false;
    if (pkt.msgType == MSG_ACK) return false;  // Don't relay ACKs
    
    return true;
}

void Router::queueRelay(const SafeChainPacket &pkt) {
    memcpy(&pendingPacket, &pkt, sizeof(SafeChainPacket));
    
    // Prepare for relay (increment hop, update CRC)
    if (!PacketBuilder::prepareRelay(pendingPacket)) {
        Serial.println(">>> Relay prep failed (max hop)");
        return;
    }
    
    markSeen(pkt.seqNum);
    
    // Random backoff
    relayTriggerTime = millis() + random(RELAY_MIN_DELAY, RELAY_MAX_DELAY);
    isPending = true;
    
    Serial.printf(">>> Queued relay: Seq=%u (Wait %lums)\n", 
        pkt.seqNum, relayTriggerTime - millis());
}

void Router::update() {
    if (!isPending) return;
    if (millis() < relayTriggerTime) return;
    
    Serial.println(">>> RELAYING NOW...");
    
    LoRa.beginPacket();
    LoRa.write((uint8_t*)&pendingPacket, sizeof(SafeChainPacket));
    LoRa.endPacket();  // Async // removed true 
    
    LoRa.receive();

    isPending = false;
    Serial.printf(">>> RELAY SENT: Seq=%u Hop=%u\n", 
        pendingPacket.seqNum, pendingPacket.hopCount);
}

void Router::cancelRelay() {
    if (isPending) {
        Serial.println("!!! CANCELLING RELAY FOR EMERGENCY !!!");
        isPending = false;
    }
}
    
bool Router::isDuplicate(uint16_t seqNum) {
    for (int i = 0; i < DUPLICATE_CACHE_SIZE; i++) {
        if (seenCache[i] == seqNum) return true;
    }
    return false;
}

void Router::markSeen(uint16_t seqNum) {
    seenCache[cacheIndex] = seqNum;
    cacheIndex = (cacheIndex + 1) % DUPLICATE_CACHE_SIZE;
}

bool Router::isOwnPacket(const SafeChainPacket &pkt) {
    return (myNodeID == String(pkt.srcID));
}