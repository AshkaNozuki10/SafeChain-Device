#ifndef EMERGENCY_H
#define EMERGENCY_H

#include <Arduino.h>
#include "config.h"
#include "packet.h"
#include "ble_terminal.h" // Include this if not already in emergency.h

enum EmergencyState {
    EM_STATE_IDLE,
    EM_STATE_SENDING,
    EM_STATE_WAITING_ACK,
    EM_STATE_CONFIRMED,
    EM_STATE_FAILED
};

class EmergencyManager {
private:
    SafeChainPacket pendingPacket;
    EmergencyState state;
    uint8_t retryCount;
    unsigned long lastSendTime;
    const char* nodeID;
    
public:
    EmergencyManager();
    
    void init(const char* id);
    
    // Trigger emergency send
    void trigger(EmergencyType type, float lat, float lon, uint8_t batt);
    
    // Update state machine (call in loop)
    void update();
    
    // Handle incoming ACK
    void handleACK(const SafeChainPacket &ack);
    
    // Check if waiting for ACK
    bool isWaiting() const { return state == EM_STATE_WAITING_ACK; }
    
    // Get current state
    EmergencyState getState() const { return state; }
    
private:
    void transmit();
    void retry();
    void fail();
    void confirm();
};

#endif