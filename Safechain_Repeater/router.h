#ifndef ROUTER_H
#define ROUTER_H

#include <Arduino.h>
#include "config.h"
#include "packet.h"

class RepeaterRouter {
private:
    struct CacheEntry {
        char srcID[6];
        uint16_t seqNum;
        uint8_t msgType;  // 👇 FIX: Added msgType to the memory!
    };
    CacheEntry seenCache[DUPLICATE_CACHE_SIZE];
    uint8_t cacheIndex;
    
    // --- NEW: Time-Based Deduplication (Spam Protection) ---
    struct EmergencyDedup {
        char srcID[6];
        uint8_t msgType;
        unsigned long timestamp;
    };
    EmergencyDedup recentEmergencies[10];
    uint8_t dedupIndex;
    // -------------------------------------------------------
    
    // Pending relay state
    SafeChainPacket pendingPacket;
    bool isPending;
    unsigned long relayTriggerTime;
    
    // Statistics
    uint32_t packetsReceived;
    uint32_t packetsRelayed;
    uint32_t packetsDropped;

    // 👇 FIX: Update these two lines to include uint8_t msgType
    bool isDuplicate(const char* srcID, uint16_t seqNum, uint8_t msgType);
    void markSeen(const char* srcID, uint16_t seqNum, uint8_t msgType);
    
public:
    RepeaterRouter();
    
    void init();
    bool shouldRelay(const SafeChainPacket &pkt);
    void queueRelay(const SafeChainPacket &pkt);
    void update();
    void printStats();
    
private:
    bool isDuplicate(const char* srcID, uint16_t seqNum);
    void markSeen(const char* srcID, uint16_t seqNum);
    
    // --- NEW: Time-Based Methods ---
    bool isRecentEmergency(const SafeChainPacket &pkt);
    void markRecentEmergency(const SafeChainPacket &pkt);
};

#endif