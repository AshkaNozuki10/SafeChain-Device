#ifndef EMERGENCY_H
#define EMERGENCY_H

#include <Arduino.h>
#include "config.h"
#include "safechain_protocol.h"
#include "ble_terminal.h"

class Storage;

enum EmergencyState : uint8_t {
    EM_STATE_IDLE       = 0,
    EM_STATE_PENDING_TX = 1,
    EM_STATE_WAITING_ACK= 2,
    EM_STATE_CONFIRMED  = 3,
    EM_STATE_FAILED     = 4
};

class EmergencyManager {
private:
    sc::SafeChainFrameV1 pendingFrame;
    EmergencyState state;
    uint8_t retryCount;

    // [M0-1] Overflow-safe timer
    unsigned long actionScheduledAt;
    uint32_t      actionDelayMs;

    unsigned long stateSince;

    const char* nodeID;
    Storage*    storage;

    uint32_t      currentEventId;
    EmergencyType currentEmergencyType;
    bool          pendingFrameValid;

    // [M3] PSK cache — loaded from storage once on init, held in RAM
    uint8_t psk[sc::PSK_LEN];

public:
    EmergencyManager();

    void init(const char* id, Storage* storageRef);

    // [M3] reloadPSK() — call after setpsk command to pick up new key
    void reloadPSK();

    void trigger(EmergencyType type, float lat, float lon, uint8_t batt);
    void update();
    void handleACK(const sc::SafeChainFrameV1 &ack);
    bool resumePending();

    bool           isWaiting()       const { return state == EM_STATE_WAITING_ACK; }
    EmergencyState getState()        const { return state; }
    bool           hasPendingEvent() const { return pendingFrameValid; }

private:
    void setState(EmergencyState newState);
    void scheduleAction(uint32_t delayMs);
    bool isActionDue() const;

    void transmitNow();
    void scheduleRetry();
    void markFailed();
    void markConfirmed();

    bool saveJournal(uint8_t journalState);
    void clearJournal();

    sc::EventType toProtocolEventType(EmergencyType type) const;
    EmergencyType fromProtocolEventType(uint8_t type) const;
    int32_t       toFixedPointE7(float value) const;
};

#endif