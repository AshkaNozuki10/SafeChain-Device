#include "router.h"
#include <LoRa.h>
#include <string.h>
#include "debug_log.h"

// [M4] Legacy SafeChainPacket path removed entirely.
// All relay logic is now V1 (SafeChainFrameV1) only.

RepeaterRouter::RepeaterRouter()
    : v1SeenIndex(0),
      pendingV1Valid(false),
      pendingV1ScheduledAt(0),
      pendingV1DelayMs(0),
      pendingV1QueuedAt(0),
      pskLen(sc::PSK_LEN),
      packetsReceived(0),
      packetsRelayed(0),
      packetsDropped(0),
      duplicatesDropped(0),
      ttlExpired(0) {
    memset(ownNodeID, 0, sizeof(ownNodeID));
    memset(v1Seen,    0, sizeof(v1Seen));
    memset(&pendingV1, 0, sizeof(pendingV1));
    // Default dev PSK — overwritten by reloadPSK() after storage.init()
    memcpy(psk, sc::PSK_DEFAULT, sc::PSK_LEN);
}

// [M0-5] Store NVS-sourced node ID
void RepeaterRouter::init(const char* nodeId) {
    strncpy(ownNodeID, nodeId, sc::DEVICE_ID_LEN - 1);
    ownNodeID[sc::DEVICE_ID_LEN - 1] = '\0';
    pendingV1Valid = false;
    Serial.printf(">>> RepeaterRouter (V1-only): ownNodeID='%s'\n", ownNodeID);
}

// [M3] Update PSK cache
void RepeaterRouter::reloadPSK(const uint8_t* key, size_t len) {
    if (len > sc::PSK_LEN) len = sc::PSK_LEN;
    memcpy(psk, key, len);
    pskLen = len;
    Serial.println(">>> Router PSK updated");
}

// ---------------------------------------------------------------------------
// V1 duplicate cache
// ---------------------------------------------------------------------------
bool RepeaterRouter::isDuplicateV1(const sc::SafeChainFrameV1 &frame) {
    for (uint8_t i = 0; i < DUPLICATE_CACHE_SIZE; i++) {
        if (!v1Seen[i].used) continue;
        if (strncmp(v1Seen[i].originID, frame.origin_id, sc::DEVICE_ID_LEN) == 0 &&
            v1Seen[i].eventId   == frame.event_id &&
            v1Seen[i].frameType == frame.frame_type)
            return true;
    }
    return false;
}

void RepeaterRouter::markSeenV1(const sc::SafeChainFrameV1 &frame) {
    strncpy(v1Seen[v1SeenIndex].originID, frame.origin_id, sc::DEVICE_ID_LEN);
    v1Seen[v1SeenIndex].originID[sc::DEVICE_ID_LEN - 1] = '\0';
    v1Seen[v1SeenIndex].eventId   = frame.event_id;
    v1Seen[v1SeenIndex].frameType = frame.frame_type;
    v1Seen[v1SeenIndex].used      = true;
    v1SeenIndex = (v1SeenIndex + 1) % DUPLICATE_CACHE_SIZE;
}

// ---------------------------------------------------------------------------
// shouldRelayV1
// ---------------------------------------------------------------------------
bool RepeaterRouter::shouldRelayV1(const sc::SafeChainFrameV1 &frame) {
    packetsReceived++;

    // [M0-5] Filter by sender_id (our last-hop ID)
    if (strncmp(frame.sender_id, ownNodeID, sc::DEVICE_ID_LEN) == 0) {
        packetsDropped++;
        return false;
    }

    if (isDuplicateV1(frame)) {
        duplicatesDropped++;
        packetsDropped++;
        LOGD(">>> Dup V1: origin=%s event=%lu\n",
            frame.origin_id, (unsigned long)frame.event_id);
        // Smart cancel: if we already queued this exact frame, drop it too
        if (pendingV1Valid &&
            strncmp(pendingV1.origin_id, frame.origin_id, sc::DEVICE_ID_LEN) == 0 &&
            pendingV1.event_id   == frame.event_id &&
            pendingV1.frame_type == frame.frame_type) {
            LOGD(">>> SMART CANCEL V1 pending relay\n");
            pendingV1Valid = false;
        }
        return false;
    }

    if (frame.hop_count >= frame.max_hops) { packetsDropped++; return false; }

    // [M5] Allow FRAME_HEARTBEAT relay so gateway can track node liveness
    if (!(frame.frame_type == sc::FRAME_EVENT  ||
          frame.frame_type == sc::FRAME_ACK    ||
          frame.frame_type == sc::FRAME_HEARTBEAT)) {
        packetsDropped++;
        return false;
    }

    return true;
}

// ---------------------------------------------------------------------------
// queueRelayV1
// ---------------------------------------------------------------------------
void RepeaterRouter::queueRelayV1(const sc::SafeChainFrameV1 &frame) {
    pendingV1 = frame;

    // [M0-5] Use ownNodeID as new sender for this hop
    if (!sc::Protocol::prepareRelay(pendingV1, ownNodeID, psk, pskLen)) {
        packetsDropped++;
        return;
    }

    markSeenV1(frame);

    // [BONUS] ACK frames get priority backoff so node stops retrying sooner
    uint32_t delayMs = (frame.frame_type == sc::FRAME_ACK)
        ? random(RELAY_MIN_DELAY / 3, RELAY_MIN_DELAY)
        : random(RELAY_MIN_DELAY, RELAY_MAX_DELAY);

    // [M0-1] Overflow-safe timer
    pendingV1ScheduledAt = millis();
    pendingV1DelayMs     = delayMs;
    pendingV1QueuedAt    = millis();
    pendingV1Valid       = true;

    Serial.printf(">>> Queued V1 relay: origin=%s event=%lu frame=%s hop=%u wait=%lums\n",
        pendingV1.origin_id,
        (unsigned long)pendingV1.event_id,
        sc::Protocol::frameTypeName(pendingV1.frame_type),
        pendingV1.hop_count,
        (unsigned long)delayMs);
}

// ---------------------------------------------------------------------------
// update() — called every loop() tick
// ---------------------------------------------------------------------------
void RepeaterRouter::update() {
    unsigned long now = millis();

    if (pendingV1Valid) {
        // [TTL] Discard stale entries (e.g. queued during radio fault)
        if ((now - pendingV1QueuedAt) > RELAY_TTL_MS) {
            LOGD(">>> V1 relay TTL expired: origin=%s event=%lu\n",
                pendingV1.origin_id, (unsigned long)pendingV1.event_id);
            pendingV1Valid = false;
            ttlExpired++;
            packetsDropped++;
        }
        // [M0-1] Overflow-safe elapsed check
        else if ((now - pendingV1ScheduledAt) >= pendingV1DelayMs) {
            LOGD(">>> RELAYING V1 NOW...\n");
            LoRa.beginPacket();
            LoRa.write((uint8_t*)&pendingV1, sizeof(sc::SafeChainFrameV1));
            LoRa.endPacket();
            LoRa.receive();
            pendingV1Valid = false;
            packetsRelayed++;
            Serial.printf(">>> V1 relayed: origin=%s event=%lu frame=%s hop=%u\n",
                pendingV1.origin_id,
                (unsigned long)pendingV1.event_id,
                sc::Protocol::frameTypeName(pendingV1.frame_type),
                pendingV1.hop_count);
        }
    }
}

// ---------------------------------------------------------------------------
// printStats
// ---------------------------------------------------------------------------
void RepeaterRouter::printStats() {
    Serial.println("\n===== REPEATER STATS =====");
    Serial.printf("Own ID             : %s\n",  ownNodeID);
    Serial.printf("Packets Received   : %lu\n", (unsigned long)packetsReceived);
    Serial.printf("Packets Relayed    : %lu\n", (unsigned long)packetsRelayed);
    Serial.printf("Packets Dropped    : %lu\n", (unsigned long)packetsDropped);
    Serial.printf("Duplicates Dropped : %lu\n", (unsigned long)duplicatesDropped);
    Serial.printf("TTL Expired        : %lu\n", (unsigned long)ttlExpired);
    Serial.println("==========================\n");
}