#ifndef EVENT_MANAGER_H
#define EVENT_MANAGER_H

#include <Arduino.h>
#include <LoRa.h>
#include "safechain_protocol.h"
#include "gateway_journal.h"
#include "config.h"

// ============================================================
// [M5] NodeRegistry — tracks liveness of all known nodes.
//
// A node is registered the first time any V1 frame arrives
// from it (EVENT or HEARTBEAT). Its last_seen_ms is updated
// on every subsequent frame. The gateway runs a periodic
// check and transitions status to OFFLINE when silence
// exceeds NODE_OFFLINE_THRESHOLD_MS.
// ============================================================
enum NodeStatus : uint8_t {
    NODE_UNKNOWN = 0,
    NODE_ONLINE  = 1,
    NODE_OFFLINE = 2
};

struct NodeRecord {
    bool     used;
    char     origin_id[sc::DEVICE_ID_LEN];
    unsigned long last_seen_ms;
    uint8_t  last_battery;
    int16_t  last_rssi;
    NodeStatus status;
    uint32_t frames_received;
    uint32_t heartbeats_received;
};

static const uint8_t MAX_NODES = 20;

class NodeRegistry {
private:
    NodeRecord nodes[MAX_NODES];
    uint8_t    count;

public:
    NodeRegistry();
    void init();

    // Update or register a node — called on every received V1 frame
    void update(const char* originId, int16_t rssi, uint8_t battery,
                bool isHeartbeat);

    // Check all nodes for offline status — call every NODE_OFFLINE_CHECK_MS
    void checkOffline();

    // Print all known nodes to Serial
    void printAll() const;

    uint8_t size() const { return count; }
    const NodeRecord* getRecord(uint8_t idx) const;
};


class EventManager {
private:
    GatewayJournal journal;
    NodeRegistry   nodeReg;    // [M5]

    uint32_t totalPackets;
    uint32_t duplicatePackets;
    uint32_t corruptedPackets;
    uint32_t ackSent;
    uint32_t journaledEvents;
    uint32_t replayedEvents;
    uint32_t heartbeatsReceived; // [M5]

    // [M0-3] Non-blocking ACK scheduling
    bool                 pendingAckValid;
    sc::SafeChainFrameV1 pendingAckFrame;
    unsigned long        ackScheduledAt;
    uint32_t             ackDelayMs;

    // [M3] PSK for building ACK frames
    uint8_t psk[sc::PSK_LEN];
    size_t  pskLen;

    // [M5] Offline check timer
    unsigned long lastOfflineCheckMs;

    const char* getEventTypeNameV1(uint8_t eventType);
    void        printEventAlert(const sc::SafeChainFrameV1 &frame);
    void        printEventAlertFromRecord(const GatewayJournalRecord &rec);

    // [M5] Handle a received heartbeat frame
    void handleHeartbeat(const sc::SafeChainFrameV1 &frame);

public:
    EventManager();

    void init();
    void replayUncommitted();

    // Must be called every loop() tick
    void update();

    // V1 path — now handles FRAME_EVENT, FRAME_HEARTBEAT, ignores FRAME_ACK
    bool processFrameV1(sc::SafeChainFrameV1 &frame);

    void scheduleAckV1(const sc::SafeChainFrameV1 &frame);

    void reloadPSK(const uint8_t* key, size_t len);

    bool hostCommit(const char* originId, uint32_t eventId);

    void printStats();

    // [M5] Print node registry table
    void printNodes() const;
};

#endif