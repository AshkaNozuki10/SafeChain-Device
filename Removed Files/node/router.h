#ifndef ROUTER_H
#define ROUTER_H

#include <Arduino.h>
#include "config.h"
#include "packet.h"

class Router {
private:
    uint16_t seenCache[DUPLICATE_CACHE_SIZE];
    uint8_t cacheIndex;
    String myNodeID;
    bool relayEnabled;
    
    // Pending relay state
    SafeChainPacket pendingPacket;
    bool isPending;
    unsigned long relayTriggerTime;
    
public:
    Router();
    
    void init(const char* nodeID, bool enableRelay);
    void setRelayEnabled(bool enabled);
    bool getRelayEnabled() const { return relayEnabled; }
    
    // Check if packet should be relayed
    bool shouldRelay(const SafeChainPacket &pkt);
    
    // Queue packet for relay
    void queueRelay(const SafeChainPacket &pkt);
    
    // Process pending relay (call in loop)
    void update();
    
    // Cancel pending relay (for emergency override)
    void cancelRelay();
    
private:
    bool isDuplicate(uint16_t seqNum);
    void markSeen(uint16_t seqNum);
    bool isOwnPacket(const SafeChainPacket &pkt);
};

#endif