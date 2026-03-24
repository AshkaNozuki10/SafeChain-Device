#include "event_manager.h"
#include <string.h>
#include "debug_log.h"

// ============================================================
// NodeRegistry implementation [M5]
// ============================================================
NodeRegistry::NodeRegistry() : count(0) {
    memset(nodes, 0, sizeof(nodes));
}

void NodeRegistry::init() {
    count = 0;
    memset(nodes, 0, sizeof(nodes));
}

void NodeRegistry::update(const char* originId, int16_t rssi,
                           uint8_t battery, bool isHeartbeat) {
    // Find existing record
    for (uint8_t i = 0; i < MAX_NODES; i++) {
        if (!nodes[i].used) continue;
        if (strncmp(nodes[i].origin_id, originId, sc::DEVICE_ID_LEN) == 0) {
            nodes[i].last_seen_ms = millis();
            nodes[i].last_rssi    = rssi;
            nodes[i].last_battery = battery;
            nodes[i].frames_received++;
            if (isHeartbeat) nodes[i].heartbeats_received++;

            if (nodes[i].status != NODE_ONLINE) {
                Serial.printf(">>> NODE ONLINE: %s (was %s)\n",
                    originId,
                    nodes[i].status == NODE_OFFLINE ? "OFFLINE" : "UNKNOWN");
                nodes[i].status = NODE_ONLINE;
            }
            return;
        }
    }

    // New node — register it
    for (uint8_t i = 0; i < MAX_NODES; i++) {
        if (nodes[i].used) continue;
        nodes[i].used             = true;
        strncpy(nodes[i].origin_id, originId, sc::DEVICE_ID_LEN - 1);
        nodes[i].origin_id[sc::DEVICE_ID_LEN - 1] = '\0';
        nodes[i].last_seen_ms     = millis();
        nodes[i].last_rssi        = rssi;
        nodes[i].last_battery     = battery;
        nodes[i].status           = NODE_ONLINE;
        nodes[i].frames_received  = 1;
        nodes[i].heartbeats_received = isHeartbeat ? 1 : 0;
        count++;
        Serial.printf(">>> NODE REGISTERED: %s RSSI=%d batt=%u%%\n",
            originId, rssi, battery);
        return;
    }

    Serial.println(">>> WARNING: NodeRegistry full — cannot register new node");
}

void NodeRegistry::checkOffline() {
    unsigned long now = millis();
    for (uint8_t i = 0; i < MAX_NODES; i++) {
        if (!nodes[i].used) continue;
        if (nodes[i].status == NODE_OFFLINE) continue;

        // [M0-1] Overflow-safe elapsed check
        if ((now - nodes[i].last_seen_ms) >= NODE_OFFLINE_THRESHOLD_MS) {
            nodes[i].status = NODE_OFFLINE;
            Serial.printf(">>> NODE OFFLINE: %s — silent for >%lus | last batt=%u%% RSSI=%d\n",
                nodes[i].origin_id,
                (unsigned long)(NODE_OFFLINE_THRESHOLD_MS / 1000),
                nodes[i].last_battery,
                nodes[i].last_rssi);
        }
    }
}

void NodeRegistry::printAll() const {
    Serial.println("\n===== NODE REGISTRY =====");
    if (count == 0) {
        Serial.println("  (no nodes seen yet)");
    }
    for (uint8_t i = 0; i < MAX_NODES; i++) {
        if (!nodes[i].used) continue;
        unsigned long age_s = (millis() - nodes[i].last_seen_ms) / 1000;
        const char* statusStr =
            nodes[i].status == NODE_ONLINE  ? "ONLINE"  :
            nodes[i].status == NODE_OFFLINE ? "OFFLINE" : "UNKNOWN";
        Serial.printf("  %-6s  %-7s  batt=%3u%%  RSSI=%4d  age=%lus  frames=%lu  hb=%lu\n",
            nodes[i].origin_id, statusStr,
            nodes[i].last_battery, nodes[i].last_rssi,
            age_s,
            (unsigned long)nodes[i].frames_received,
            (unsigned long)nodes[i].heartbeats_received);
    }
    Serial.println("=========================\n");
}

const NodeRecord* NodeRegistry::getRecord(uint8_t idx) const {
    if (idx >= MAX_NODES || !nodes[idx].used) return nullptr;
    return &nodes[idx];
}

// ============================================================
// EventManager implementation
// ============================================================
EventManager::EventManager()
    : totalPackets(0), duplicatePackets(0), corruptedPackets(0),
      ackSent(0), journaledEvents(0), replayedEvents(0),
      heartbeatsReceived(0),
      pendingAckValid(false), ackScheduledAt(0), ackDelayMs(0),
      pskLen(sc::PSK_LEN),
      lastOfflineCheckMs(0) {
    memset(&pendingAckFrame, 0, sizeof(pendingAckFrame));
    memcpy(psk, sc::PSK_DEFAULT, sc::PSK_LEN);
}

// ---------------------------------------------------------------------------
// init() — loads NVS journal, replays uncommitted, inits node registry
// ---------------------------------------------------------------------------
void EventManager::init() {
    nodeReg.init(); // [M5]
    uint16_t uncommitted = journal.init();
    if (uncommitted > 0) {
        Serial.printf(">>> EventManager: %u uncommitted events — replaying\n", uncommitted);
        replayUncommitted();
    }
    lastOfflineCheckMs = millis();
}

// ---------------------------------------------------------------------------
// reloadPSK [M3]
// ---------------------------------------------------------------------------
void EventManager::reloadPSK(const uint8_t* key, size_t len) {
    if (len > sc::PSK_LEN) len = sc::PSK_LEN;
    memcpy(psk, key, len);
    pskLen = len;
    Serial.println(">>> Gateway PSK updated");
}

// ---------------------------------------------------------------------------
// replayUncommitted [M2]
// ---------------------------------------------------------------------------
void EventManager::replayUncommitted() {
    uint16_t n = journal.replayCount();
    if (n == 0) return;

    Serial.println(">>> ===== JOURNAL REPLAY START =====");

    for (uint16_t i = 0; i < n; i++) {
        const GatewayJournalRecord* rec = journal.getReplayRecord(i);
        if (!rec) continue;

        float lat = rec->lat_e7 / 10000000.0f;
        float lon = rec->lon_e7 / 10000000.0f;

        Serial.printf("REPLAY_CSV_V1,%s,%lu,%s,%.6f,%.6f,%u,%d,%u,%u\n",
            rec->origin_id, (unsigned long)rec->event_id,
            getEventTypeNameV1(rec->event_type), lat, lon,
            rec->battery_pct, rec->rssi_dbm, rec->hop_count, rec->attempt);

        printEventAlertFromRecord(*rec);

        if (rec->status == GW_EVT_RECEIVED)
            journal.markHostQueued(rec->origin_id, rec->event_id);

        replayedEvents++;

        if (!pendingAckValid) {
            sc::SafeChainFrameV1 syn{};
            syn.protocol_version = sc::PROTOCOL_VERSION;
            syn.frame_type       = sc::FRAME_EVENT;
            syn.event_type       = rec->event_type;
            syn.flags            = sc::FLAG_REQUIRE_ACK;
            strncpy(syn.origin_id, rec->origin_id, sc::DEVICE_ID_LEN - 1);
            syn.event_id  = rec->event_id;
            syn.max_hops  = sc::MAX_HOPS_DEFAULT;
            sc::Protocol::finalizeFrame(syn, psk, pskLen);

            pendingAckFrame = syn;
            ackScheduledAt  = millis() + 500 + (i * 300);
            ackDelayMs      = 0;
            pendingAckValid = true;
        }
    }

    Serial.printf(">>> ===== JOURNAL REPLAY END (%u events) =====\n", n);
}

// ---------------------------------------------------------------------------
// update() — ACK scheduler + offline check [M0-3, M5]
// ---------------------------------------------------------------------------
void EventManager::update() {
    // [M0-3] Fire pending ACK
    if (pendingAckValid && (millis() - ackScheduledAt) >= ackDelayMs) {
        pendingAckValid = false;

        sc::SafeChainFrameV1 ackFrame;
        sc::Protocol::buildAckFrame(
            ackFrame, DEFAULT_NODE_ID,
            pendingAckFrame.origin_id, pendingAckFrame.event_id,
            static_cast<sc::EventType>(pendingAckFrame.event_type),
            psk, pskLen,
            pendingAckFrame.max_hops);

        Serial.printf(">>> SENDING V1 ACK TO: %s (EventID: %lu)\n",
            pendingAckFrame.origin_id, (unsigned long)pendingAckFrame.event_id);

        LoRa.beginPacket();
        LoRa.write((const uint8_t*)&ackFrame, sizeof(sc::SafeChainFrameV1));
        LoRa.endPacket();
        LoRa.receive();
        ackSent++;
    }

    // [M5] Periodic offline check
    if ((millis() - lastOfflineCheckMs) >= NODE_OFFLINE_CHECK_MS) {
        lastOfflineCheckMs = millis();
        nodeReg.checkOffline();
    }
}

// ---------------------------------------------------------------------------
// scheduleAckV1 [M0-3]
// ---------------------------------------------------------------------------
void EventManager::scheduleAckV1(const sc::SafeChainFrameV1 &frame) {
    pendingAckFrame = frame;
    ackScheduledAt  = millis();
    ackDelayMs      = random(100, 300);
    pendingAckValid = true;
}

// ---------------------------------------------------------------------------
// [M5] handleHeartbeat — updates node registry, no journal entry, no ACK
// ---------------------------------------------------------------------------
void EventManager::handleHeartbeat(const sc::SafeChainFrameV1 &frame) {
    heartbeatsReceived++;

    nodeReg.update(frame.origin_id, frame.last_rssi_dbm,
                   frame.battery_pct, true);

    float lat = frame.lat_e7 / 10000000.0f;
    float lon = frame.lon_e7 / 10000000.0f;

    Serial.printf("[HEARTBEAT] node=%-6s  batt=%3u%%  RSSI=%4d  GPS=%s",
        frame.origin_id,
        frame.battery_pct,
        frame.last_rssi_dbm,
        (frame.flags & sc::FLAG_GPS_VALID) ? "valid" : "none");

    if (frame.flags & sc::FLAG_GPS_VALID)
        Serial.printf("  loc=%.5f,%.5f", lat, lon);

    if (frame.flags & sc::FLAG_LOW_BATTERY)
        Serial.print("  [LOW BATT]");

    Serial.println();

    // CSV line for host software
    Serial.printf("HB_V1,%s,%.6f,%.6f,%u,%d\n",
        frame.origin_id, lat, lon,
        frame.battery_pct, frame.last_rssi_dbm);
}

// ---------------------------------------------------------------------------
// processFrameV1 — handles EVENT, HEARTBEAT, ignores ACK
// ---------------------------------------------------------------------------
bool EventManager::processFrameV1(sc::SafeChainFrameV1 &frame) {
    totalPackets++;

    // [M5] Update node registry on every valid frame
    nodeReg.update(frame.origin_id, frame.last_rssi_dbm,
                   frame.battery_pct,
                   frame.frame_type == sc::FRAME_HEARTBEAT);

    if (frame.frame_type == sc::FRAME_ACK) {
        LOGD(">>> V1 ACK at gateway - ignored\n");
        return false;
    }

    // [M5] Heartbeat path — no journal, no ACK
    if (frame.frame_type == sc::FRAME_HEARTBEAT) {
        handleHeartbeat(frame);
        return true;
    }

    if (frame.frame_type != sc::FRAME_EVENT) {
        Serial.printf(">>> Unsupported V1 frame type: %u\n", frame.frame_type);
        return false;
    }

    // EVENT_TEST — validate auth but skip journaling
    if (frame.event_type == sc::EVENT_TEST) {
        Serial.printf("[TEST PKT] node=%s RSSI=%d\n",
            frame.origin_id, frame.last_rssi_dbm);
        return true;
    }

    // Emergency event path
    bool isNew = journal.appendIfNew(frame);

    if (!isNew) {
        duplicatePackets++;
        Serial.printf(">>> DUPLICATE V1: origin=%s event=%lu\n",
            frame.origin_id, (unsigned long)frame.event_id);
        if (frame.flags & sc::FLAG_REQUIRE_ACK)
            scheduleAckV1(frame);
        return false;
    }

    journaledEvents++;

    float lat = frame.lat_e7 / 10000000.0f;
    float lon = frame.lon_e7 / 10000000.0f;

    printEventAlert(frame);

    Serial.printf("CSV_V1,%s,%lu,%s,%.6f,%.6f,%u,%d,%u,%u\n",
        frame.origin_id, (unsigned long)frame.event_id,
        getEventTypeNameV1(frame.event_type), lat, lon,
        frame.battery_pct, frame.last_rssi_dbm,
        frame.hop_count, frame.attempt);

    journal.markHostQueued(frame.origin_id, frame.event_id);

    if (frame.flags & sc::FLAG_REQUIRE_ACK)
        scheduleAckV1(frame);

    return true;
}

// ---------------------------------------------------------------------------
// hostCommit [M2]
// ---------------------------------------------------------------------------
bool EventManager::hostCommit(const char* originId, uint32_t eventId) {
    bool ok = journal.markHostCommitted(originId, eventId);
    if (ok)
        Serial.printf(">>> HOST COMMIT: origin=%s event=%lu — NVS cleared\n",
            originId, (unsigned long)eventId);
    else
        Serial.printf(">>> HOST COMMIT FAILED: origin=%s event=%lu not found\n",
            originId, (unsigned long)eventId);
    return ok;
}

// ---------------------------------------------------------------------------
// Printers
// ---------------------------------------------------------------------------
const char* EventManager::getEventTypeNameV1(uint8_t eventType) {
    switch (eventType) {
        case sc::EVENT_FIRE:  return "FIRE";
        case sc::EVENT_FLOOD: return "FLOOD";
        case sc::EVENT_CRIME: return "CRIME";
        case sc::EVENT_SAFE:  return "SAFE";
        case sc::EVENT_TEST:  return "TEST";
        default:              return "UNKNOWN";
    }
}

void EventManager::printEventAlert(const sc::SafeChainFrameV1 &frame) {
    float lat = frame.lat_e7 / 10000000.0f;
    float lon = frame.lon_e7 / 10000000.0f;
    Serial.println("\n EMERGENCY ALERT");
    switch (frame.event_type) {
        case sc::EVENT_FIRE:  Serial.println("   ** FIRE ALARM **");      break;
        case sc::EVENT_FLOOD: Serial.println("   ** FLOOD ALERT **");     break;
        case sc::EVENT_CRIME: Serial.println("   ** CRIME / SOS **");     break;
        case sc::EVENT_SAFE:  Serial.println("   ** USER MARKED SAFE **");break;
        default:              Serial.println("   ** UNKNOWN EVENT **");   break;
    }
    Serial.printf("   NODE ID  : %s\n",  frame.origin_id);
    Serial.printf("   EVENT ID : %lu\n", (unsigned long)frame.event_id);
    Serial.printf("   LOCATION : %.6f, %.6f\n", lat, lon);
    Serial.printf("   BATTERY  : %u%%\n",  frame.battery_pct);
    Serial.printf("   NETWORK  : RSSI %d dBm | Hops: %u\n",
        frame.last_rssi_dbm, frame.hop_count);
    Serial.printf("   TYPE     : %s\n",   getEventTypeNameV1(frame.event_type));
    Serial.printf("   ATTEMPT  : %u\n",   frame.attempt);
    Serial.printf("   GPS VALID: %s\n",   (frame.flags & sc::FLAG_GPS_VALID) ? "yes" : "no");
    Serial.printf("   LOW BATT : %s\n",   (frame.flags & sc::FLAG_LOW_BATTERY) ? "yes" : "no");
    Serial.println("  ===================================================");
}

void EventManager::printEventAlertFromRecord(const GatewayJournalRecord &rec) {
    float lat = rec.lat_e7 / 10000000.0f;
    float lon = rec.lon_e7 / 10000000.0f;
    Serial.println("\n [REPLAY] EMERGENCY ALERT");
    switch (rec.event_type) {
        case sc::EVENT_FIRE:  Serial.println("   ** FIRE ALARM **");      break;
        case sc::EVENT_FLOOD: Serial.println("   ** FLOOD ALERT **");     break;
        case sc::EVENT_CRIME: Serial.println("   ** CRIME / SOS **");     break;
        case sc::EVENT_SAFE:  Serial.println("   ** USER MARKED SAFE **");break;
        default:              Serial.println("   ** UNKNOWN EVENT **");   break;
    }
    Serial.printf("   NODE ID  : %s\n",  rec.origin_id);
    Serial.printf("   EVENT ID : %lu\n", (unsigned long)rec.event_id);
    Serial.printf("   LOCATION : %.6f, %.6f\n", lat, lon);
    Serial.printf("   BATTERY  : %u%%\n", rec.battery_pct);
    Serial.printf("   NETWORK  : RSSI %d dBm | Hops: %u\n",
        rec.rssi_dbm, rec.hop_count);
    Serial.printf("   TYPE     : %s\n",  getEventTypeNameV1(rec.event_type));
    Serial.printf("   ATTEMPT  : %u\n",  rec.attempt);
    Serial.println("  ===================================================");
}

void EventManager::printNodes() const {
    nodeReg.printAll();
}

void EventManager::printStats() {
    Serial.println("\n===== GATEWAY STATS =====");
    Serial.printf("Total Packets      : %lu\n", (unsigned long)totalPackets);
    Serial.printf("Duplicate Packets  : %lu\n", (unsigned long)duplicatePackets);
    Serial.printf("Corrupted Packets  : %lu\n", (unsigned long)corruptedPackets);
    Serial.printf("ACKs Sent          : %lu\n", (unsigned long)ackSent);
    Serial.printf("Journaled Events   : %lu\n", (unsigned long)journaledEvents);
    Serial.printf("Replayed Events    : %lu\n", (unsigned long)replayedEvents);
    Serial.printf("Heartbeats Rcvd    : %lu\n", (unsigned long)heartbeatsReceived);
    Serial.printf("Journal Size       : %u\n",  journal.size());
    Serial.printf("Nodes Tracked      : %u\n",  nodeReg.size());
    Serial.printf("Pending ACK        : %s\n",  pendingAckValid ? "yes" : "no");
    Serial.println("=========================\n");
}