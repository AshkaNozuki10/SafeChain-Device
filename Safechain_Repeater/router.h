#ifndef ROUTER_H
#define ROUTER_H

#include <Arduino.h>
#include "config.h"
#include "safechain_protocol.h"

// [M4] Legacy SafeChainPacket path fully removed.
// RepeaterRouter now handles V1 frames only.

class RepeaterRouter {
private:
    // [M0-5] ownNodeID sourced from NVS via init()
    char ownNodeID[sc::DEVICE_ID_LEN];

    // -------- V1 duplicate cache --------
    struct V1CacheEntry {
        char     originID[sc::DEVICE_ID_LEN];
        uint32_t eventId;
        uint8_t  frameType;
        bool     used;
    };
    V1CacheEntry v1Seen[DUPLICATE_CACHE_SIZE];
    uint8_t      v1SeenIndex;

    // -------- Pending V1 relay (overflow-safe timers + TTL) --------
    sc::SafeChainFrameV1 pendingV1;
    bool                 pendingV1Valid;
    unsigned long        pendingV1ScheduledAt; // [M0-1]
    uint32_t             pendingV1DelayMs;
    unsigned long        pendingV1QueuedAt;    // [TTL]

    // [M3] PSK for prepareRelay (recomputes crc16 only — auth_tag stable)
    uint8_t psk[sc::PSK_LEN];
    size_t  pskLen;

    // -------- Stats --------
    uint32_t packetsReceived;
    uint32_t packetsRelayed;
    uint32_t packetsDropped;
    uint32_t duplicatesDropped;
    uint32_t ttlExpired;

    bool isDuplicateV1(const sc::SafeChainFrameV1 &frame);
    void markSeenV1(const sc::SafeChainFrameV1 &frame);

public:
    RepeaterRouter();

    // [M0-5] nodeId MUST be the NVS-sourced string
    void init(const char* nodeId);

    // [M3] Update PSK cache after setpsk command
    void reloadPSK(const uint8_t* key, size_t len);

    bool shouldRelayV1(const sc::SafeChainFrameV1 &frame);
    void queueRelayV1(const sc::SafeChainFrameV1 &frame);

    void update();
    void printStats();
};

#endif