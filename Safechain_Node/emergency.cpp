#include "emergency.h"
#include "storage.h"
#include <LoRa.h>
#include <OneButton.h>
#include <string.h>

extern OneButton btnFlood;
extern OneButton btnFire;
extern OneButton btnCrime;
extern BLETerminal ble;

static constexpr uint8_t JOURNAL_PENDING_ACK = 1;
static constexpr uint8_t JOURNAL_ACKED       = 2;
static constexpr uint8_t JOURNAL_CLEARED     = 3;

EmergencyManager::EmergencyManager()
    : state(EM_STATE_IDLE), retryCount(0),
      actionScheduledAt(0), actionDelayMs(0), stateSince(0),
      storage(nullptr),
      currentEventId(0), currentEmergencyType(EM_NONE),
      pendingFrameValid(false) {
    memset(&pendingFrame, 0, sizeof(pendingFrame));
    memset(psk, 0, sizeof(psk));
    memset(nodeID, 0, sc::DEVICE_ID_LEN); // 👇 FIX: Safe permanent memory
}

void EmergencyManager::init(const char* id, Storage* storageRef) {
    memset(nodeID, 0, sc::DEVICE_ID_LEN);
    strncpy(nodeID, id, sc::DEVICE_ID_LEN - 1); // 👇 FIX: Copy the ID safely!
    storage = storageRef;
    reloadPSK();
    setState(EM_STATE_IDLE);
}

// [M3] Load PSK from NVS (or default if not provisioned)
void EmergencyManager::reloadPSK() {
    if (storage) {
        storage->getPSK(psk, sc::PSK_LEN);
        bool custom = storage->hasPSK();
        Serial.printf(">>> PSK loaded (%s)\n", custom ? "provisioned" : "dev default");
    }
}

void EmergencyManager::setState(EmergencyState newState) {
    state      = newState;
    stateSince = millis();
}

void EmergencyManager::scheduleAction(uint32_t delayMs) {
    actionScheduledAt = millis();
    actionDelayMs     = delayMs;
}

bool EmergencyManager::isActionDue() const {
    return (millis() - actionScheduledAt) >= actionDelayMs;
}

sc::EventType EmergencyManager::toProtocolEventType(EmergencyType type) const {
    switch (type) {
        case EM_FIRE:  return sc::EVENT_FIRE;
        case EM_FLOOD: return sc::EVENT_FLOOD;
        case EM_CRIME: return sc::EVENT_CRIME;
        case EM_SAFE:  return sc::EVENT_SAFE;
        default:       return sc::EVENT_NONE;
    }
}

EmergencyType EmergencyManager::fromProtocolEventType(uint8_t type) const {
    switch (type) {
        case sc::EVENT_FIRE:  return EM_FIRE;
        case sc::EVENT_FLOOD: return EM_FLOOD;
        case sc::EVENT_CRIME: return EM_CRIME;
        case sc::EVENT_SAFE:  return EM_SAFE;
        default:              return EM_NONE;
    }
}

int32_t EmergencyManager::toFixedPointE7(float value) const {
    return (int32_t)(value * 10000000.0f);
}

bool EmergencyManager::saveJournal(uint8_t journalState) {
    if (!storage || !pendingFrameValid) return false;
    NodeEventRecord rec{};
    rec.event_id    = pendingFrame.event_id;
    rec.event_type  = pendingFrame.event_type;
    rec.attempt     = pendingFrame.attempt;
    rec.state       = journalState;
    rec.battery_pct = pendingFrame.battery_pct;
    rec.lat_e7      = pendingFrame.lat_e7;
    rec.lon_e7      = pendingFrame.lon_e7;
    rec.created_ms  = pendingFrame.event_time_ms;
    rec.updated_ms  = millis();
    bool ok = storage->savePendingEvent(rec);
    if (!ok) Serial.println(">>> ERROR: journal save failed");
    return ok;
}

void EmergencyManager::clearJournal() {
    if (storage) storage->clearPendingEvent();
}

// ---------------------------------------------------------------------------
// trigger — [M3] passes PSK to buildEventFrame
// ---------------------------------------------------------------------------
void EmergencyManager::trigger(EmergencyType type, float lat, float lon, uint8_t batt) {
    if (!storage || !nodeID) {
        Serial.println(">>> ERROR: EmergencyManager not initialized");
        return;
    }

    currentEmergencyType = type;
    currentEventId       = storage->nextEventCounter();
    retryCount           = 0;

    bool gpsValid   = !(lat == 0.0f && lon == 0.0f);
    bool lowBattery = (batt <= 15);

    // [M3] PSK passed to buildEventFrame — auth_tag computed inside
    sc::Protocol::buildEventFrame(
        pendingFrame,
        nodeID, nodeID,
        currentEventId,
        toProtocolEventType(type),
        0,
        millis(),
        toFixedPointE7(lat),
        toFixedPointE7(lon),
        batt, gpsValid, lowBattery,
        psk, sc::PSK_LEN
    );

    pendingFrameValid = true;
    if (!saveJournal(JOURNAL_PENDING_ACK))
        Serial.println(">>> WARNING: event created but journal save failed");

    Serial.printf("EVENT CREATED: ID %lu TYPE %s (auth_tag=0x%08lX)\n",
        (unsigned long)pendingFrame.event_id,
        sc::Protocol::eventTypeName(pendingFrame.event_type),
        (unsigned long)pendingFrame.auth_tag);
    ble.send("EVENT CREATED");

    setState(EM_STATE_PENDING_TX);
    scheduleAction(0);
}

// ---------------------------------------------------------------------------
// transmitNow — [M3] re-finalizes with PSK before each TX
// ---------------------------------------------------------------------------
void EmergencyManager::transmitNow() {
    if (!pendingFrameValid) return;

    pendingFrame.attempt   = retryCount;
    pendingFrame.hop_count = 0;
    strncpy(pendingFrame.sender_id, nodeID, sc::DEVICE_ID_LEN - 1);
    pendingFrame.sender_id[sc::DEVICE_ID_LEN - 1] = '\0';

    // [M3] Re-finalize: recompute auth_tag and crc16 (attempt/sender may have changed)
    sc::Protocol::finalizeFrame(pendingFrame, psk, sc::PSK_LEN);
    saveJournal(JOURNAL_PENDING_ACK);

    Serial.printf("TX: EventID %lu Attempt %u auth=0x%08lX\n",
        (unsigned long)pendingFrame.event_id, pendingFrame.attempt,
        (unsigned long)pendingFrame.auth_tag);
    ble.send("SENDING EVENT");

    ledcWriteTone(PIN_BUZZER, 4000);
    ledcWrite(PIN_BUZZER, 128);
    delay(100);
    ledcWrite(PIN_BUZZER, 0);

    LoRa.beginPacket();
    LoRa.write((uint8_t*)&pendingFrame, sizeof(sc::SafeChainFrameV1));
    LoRa.endPacket();
    LoRa.receive();

    setState(EM_STATE_WAITING_ACK);
    scheduleAction(ACK_TIMEOUT_MS);
}

void EmergencyManager::scheduleRetry() {
    if (!pendingFrameValid) return;
    if (retryCount >= MAX_RETRIES) { markFailed(); return; }

    retryCount++;
    uint32_t minD, maxD;
    switch (retryCount) {
        case 1: minD=600;  maxD=900;   break;
        case 2: minD=1800; maxD=2400;  break;
        case 3: minD=4000; maxD=5000;  break;
        default:minD=8000; maxD=10000; break;
    }
    uint32_t backoff = random(minD, maxD + 1);
    Serial.printf("RETRY %u/%u EventID %lu after %lums\n",
        retryCount, MAX_RETRIES,
        (unsigned long)pendingFrame.event_id, (unsigned long)backoff);
    ble.send("RETRYING");
    ledcWriteTone(PIN_BUZZER, 800);
    ledcWrite(PIN_BUZZER, 128);
    delay(50);
    ledcWrite(PIN_BUZZER, 0);

    pendingFrame.attempt       = retryCount;
    pendingFrame.event_time_ms = millis();
    sc::Protocol::finalizeFrame(pendingFrame, psk, sc::PSK_LEN);
    saveJournal(JOURNAL_PENDING_ACK);

    setState(EM_STATE_PENDING_TX);
    scheduleAction(backoff);
}

void EmergencyManager::markFailed() {
    Serial.println(">>> FAILED - No ACK after all retries");
    ble.send("FAILED - Auto-retry in 60s.");
    ledcWriteTone(PIN_BUZZER, 500);
    ledcWrite(PIN_BUZZER, 128);
    delay(1000);
    ledcWrite(PIN_BUZZER, 0);
    saveJournal(JOURNAL_PENDING_ACK);
    setState(EM_STATE_FAILED);
    scheduleAction(FAILED_RETRY_INTERVAL_MS);
}

void EmergencyManager::markConfirmed() {
    Serial.println(">>> ACK RECEIVED - SUCCESS!");
    ble.send("ALERT CONFIRMED - Help is on the way!");
    ledcWriteTone(PIN_BUZZER, 2500);
    ledcWrite(PIN_BUZZER, 128);
    delay(150);
    ledcWrite(PIN_BUZZER, 0);
    saveJournal(JOURNAL_ACKED);
    clearJournal();
    pendingFrameValid    = false;
    currentEventId       = 0;
    currentEmergencyType = EM_NONE;
    retryCount           = 0;
    setState(EM_STATE_CONFIRMED);
    scheduleAction(1000);
}

void EmergencyManager::update() {
    btnFlood.tick();
    btnFire.tick();
    btnCrime.tick();

    switch (state) {
        case EM_STATE_IDLE: break;
        case EM_STATE_PENDING_TX:
            if (isActionDue()) transmitNow();
            break;
        case EM_STATE_WAITING_ACK:
            if (isActionDue()) scheduleRetry();
            break;
        case EM_STATE_CONFIRMED:
            if (isActionDue()) setState(EM_STATE_IDLE);
            break;
        case EM_STATE_FAILED:
            if (isActionDue() && pendingFrameValid) {
                Serial.println(">>> FAILED RECOVERY: re-attempting");
                ble.send("Auto-retrying failed emergency event");
                retryCount = 0;
                setState(EM_STATE_PENDING_TX);
                scheduleAction(0);
            }
            break;
        default: break;
    }
}

// ---------------------------------------------------------------------------
// handleACK — [M3] validateFrame checks HMAC too
// ---------------------------------------------------------------------------
void EmergencyManager::handleACK(const sc::SafeChainFrameV1 &ack) {
    if (state != EM_STATE_WAITING_ACK) return;
    if (!pendingFrameValid) return;
    if (ack.frame_type != sc::FRAME_ACK) return;

    // 👇 FIX: Print exact reasons if an ACK is rejected!
    if (strncmp(ack.origin_id, nodeID, sc::DEVICE_ID_LEN) != 0) {
        Serial.printf(">>> IGNORING ACK: ID mismatch! Expected '%s', got '%s'\n", nodeID, ack.origin_id);
        return;
    }
    if (ack.event_id != pendingFrame.event_id) {
        Serial.printf(">>> IGNORING ACK: Event ID mismatch!\n");
        return;
    }

    markConfirmed();
}

bool EmergencyManager::resumePending() {
    if (!storage || !nodeID) return false;
    NodeEventRecord rec{};
    if (!storage->loadPendingEvent(rec)) return false;
    if (rec.state != JOURNAL_PENDING_ACK) return false;

    currentEventId       = rec.event_id;
    currentEmergencyType = fromProtocolEventType(rec.event_type);
    retryCount           = rec.attempt;
    bool gpsValid   = !(rec.lat_e7 == 0 && rec.lon_e7 == 0);
    bool lowBattery = (rec.battery_pct <= 15);

    sc::Protocol::buildEventFrame(
        pendingFrame, nodeID, nodeID,
        rec.event_id, static_cast<sc::EventType>(rec.event_type),
        rec.attempt, rec.created_ms,
        rec.lat_e7, rec.lon_e7, rec.battery_pct,
        gpsValid, lowBattery,
        psk, sc::PSK_LEN
    );

    pendingFrameValid = true;
    Serial.printf(">>> RESUMING PENDING EVENT: ID=%lu attempt=%u\n",
        (unsigned long)pendingFrame.event_id, pendingFrame.attempt);
    ble.send("Resuming pending emergency event after reboot");
    setState(EM_STATE_PENDING_TX);
    scheduleAction(500);
    return true;
}