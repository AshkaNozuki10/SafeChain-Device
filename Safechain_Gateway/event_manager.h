#ifndef EVENT_MANAGER_H
#define EVENT_MANAGER_H

#include <Arduino.h>
#include "config.h"
#include "packet.h"

struct EmergencyDedup {
    char srcID[6];
    uint8_t msgType;
    unsigned long timestamp;
};

struct EmergencyEvent {
    uint32_t eventID;
    char srcID[6];
    uint8_t msgType;
    uint16_t seqNum;
    float latitude;
    float longitude;
    uint8_t hopCount;
    uint8_t battery;
    int16_t rssi;
    unsigned long timestamp;
    bool active;
};

struct NodeStats {
    char nodeID[6];
    uint16_t lastSeq;
    uint16_t packetsReceived;
    uint16_t packetsMissed;
    unsigned long lastSeen;
    int16_t avgRSSI;
    float avgHops;
};

class EventManager {
private:
    EmergencyEvent events[MAX_EVENTS];
    uint8_t eventIndex;
    
    NodeStats nodes[40];  // Track up to 10 nodes
    uint8_t nodeCount;

    struct CacheEntry {
        char srcID[6];
        uint16_t seqNum;
    };
    CacheEntry seenCache[DUPLICATE_CACHE_SIZE];
    uint8_t cacheIndex;
    
    // --- NEW: 30-Second Deduplication Tracking ---
    EmergencyDedup recentEmergencies[40];
    uint8_t dedupIndex;
    static const uint32_t DEDUP_WINDOW_MS = 30000;  // 30 seconds
    // ---------------------------------------------
    
    // Statistics
    uint32_t totalPackets;
    uint32_t duplicatePackets;
    uint32_t corruptedPackets;
    
public:
    EventManager();
    
    void init();
    bool processPacket(SafeChainPacket &pkt);
    void sendACK(const SafeChainPacket &pkt);
    void displayAlert(const SafeChainPacket &pkt);
    void logCSV(const SafeChainPacket &pkt);
    void printStats();
    void printPDR();
    
private:
    bool isDuplicate(const char* srcID, uint16_t seqNum);
    void markSeen(const char* srcID, uint16_t seqNum);
    
    // --- NEW: Deduplication Logic Handlers ---
    bool isRecentDuplicate(const SafeChainPacket &pkt);
    void markRecentEmergency(const SafeChainPacket &pkt);
    // -----------------------------------------
    
    void storeEvent(const SafeChainPacket &pkt);
    void updateNodeStats(const SafeChainPacket &pkt);
    NodeStats* findNode(const char* nodeID);
    const char* getTypeName(uint8_t type);
};

#endif