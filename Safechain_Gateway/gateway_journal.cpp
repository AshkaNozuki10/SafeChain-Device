#include "gateway_journal.h"
#include <string.h>

// ============================================================
// GatewayJournal — NVS-backed persistent implementation
// ============================================================

GatewayJournal::GatewayJournal() : count(0), nvsOpen(false) {
    memset(records, 0, sizeof(records));
}

// ---------------------------------------------------------------------------
// NVS helpers
// ---------------------------------------------------------------------------
void GatewayJournal::nvsOpen_() {
    if (!nvsOpen) {
        prefs.begin("gw_journal", false);
        nvsOpen = true;
    }
}

void GatewayJournal::nvsClose_() {
    if (nvsOpen) {
        prefs.end();
        nvsOpen = false;
    }
}

void GatewayJournal::slotKey(uint16_t idx, char* buf) const {
    // Produces "gj_00" through "gj_29" — all within 15-char NVS key limit
    snprintf(buf, 8, "gj_%02u", (unsigned)idx);
}

uint16_t GatewayJournal::calcCRC16(const uint8_t* data, size_t len) const {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (uint8_t j = 0; j < 8; j++) {
            crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : crc << 1;
        }
    }
    return crc;
}

bool GatewayJournal::nvsWriteSlot(uint16_t idx) {
    if (idx >= MAX_NVS_SLOTS) return true; // RAM-only slot — not an error

    NvsJournalSlot slot{};
    slot.magic  = SLOT_MAGIC;
    slot.record = records[idx];
    slot.crc16  = 0;
    slot.crc16  = calcCRC16(reinterpret_cast<const uint8_t*>(&slot),
                             sizeof(NvsJournalSlot) - sizeof(slot.crc16));

    char key[8];
    slotKey(idx, key);

    nvsOpen_();
    size_t written = prefs.putBytes(key, &slot, sizeof(NvsJournalSlot));
    return written == sizeof(NvsJournalSlot);
}

bool GatewayJournal::nvsReadSlot(uint16_t idx, GatewayJournalRecord &out) {
    char key[8];
    slotKey(idx, key);

    nvsOpen_();
    if (!prefs.isKey(key)) return false;

    NvsJournalSlot slot{};
    size_t read = prefs.getBytes(key, &slot, sizeof(NvsJournalSlot));
    if (read != sizeof(NvsJournalSlot)) return false;
    if (slot.magic != SLOT_MAGIC) return false;

    uint16_t received = slot.crc16;
    slot.crc16 = 0;
    uint16_t expected = calcCRC16(reinterpret_cast<const uint8_t*>(&slot),
                                  sizeof(NvsJournalSlot) - sizeof(slot.crc16));
    if (received != expected) {
        Serial.printf(">>> Journal slot %u CRC mismatch — discarded\n", idx);
        return false;
    }

    slot.crc16 = received;
    out = slot.record;
    return true;
}

void GatewayJournal::nvsClearSlot(uint16_t idx) {
    if (idx >= MAX_NVS_SLOTS) return;
    char key[8];
    slotKey(idx, key);
    nvsOpen_();
    prefs.remove(key);
}

// ---------------------------------------------------------------------------
// init() — load NVS records into RAM, return count of uncommitted records
// ---------------------------------------------------------------------------
uint16_t GatewayJournal::init() {
    count = 0;
    memset(records, 0, sizeof(records));

    nvsOpen_();
    uint16_t loaded    = 0;
    uint16_t uncommitted = 0;

    for (uint16_t i = 0; i < MAX_NVS_SLOTS; i++) {
        GatewayJournalRecord rec{};
        if (!nvsReadSlot(i, rec)) continue;
        if (!rec.used) continue;

        // Sanity: origin_id must be non-empty
        if (rec.origin_id[0] == '\0') {
            nvsClearSlot(i);
            continue;
        }

        records[i] = rec;
        count++;
        loaded++;

        if (rec.status != GW_EVT_HOST_COMMITTED) {
            uncommitted++;
        } else {
            // Committed records can be cleared from NVS to free space
            nvsClearSlot(i);
            records[i].used = false;
            count--;
        }
    }

    if (loaded > 0) {
        Serial.printf(">>> Journal loaded: %u records (%u uncommitted) from NVS\n",
            loaded, uncommitted);
    }

    return uncommitted;
}

// ---------------------------------------------------------------------------
// replayCount / getReplayRecord — let EventManager iterate uncommitted records
// ---------------------------------------------------------------------------
uint16_t GatewayJournal::replayCount() const {
    uint16_t n = 0;
    for (uint16_t i = 0; i < MAX_RECORDS; i++) {
        if (records[i].used && records[i].status != GW_EVT_HOST_COMMITTED)
            n++;
    }
    return n;
}

const GatewayJournalRecord* GatewayJournal::getReplayRecord(uint16_t replayIdx) const {
    uint16_t n = 0;
    for (uint16_t i = 0; i < MAX_RECORDS; i++) {
        if (records[i].used && records[i].status != GW_EVT_HOST_COMMITTED) {
            if (n == replayIdx) return &records[i];
            n++;
        }
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// findIndex / exists
// ---------------------------------------------------------------------------
int GatewayJournal::findIndex(const char* originId, uint32_t eventId) const {
    for (uint16_t i = 0; i < MAX_RECORDS; i++) {
        if (!records[i].used) continue;
        if (strncmp(records[i].origin_id, originId, sc::DEVICE_ID_LEN) == 0 &&
            records[i].event_id == eventId)
            return i;
    }
    return -1;
}

bool GatewayJournal::exists(const char* originId, uint32_t eventId) const {
    return findIndex(originId, eventId) >= 0;
}

// ---------------------------------------------------------------------------
// appendIfNew — write to RAM and immediately persist to NVS
// ---------------------------------------------------------------------------
bool GatewayJournal::appendIfNew(const sc::SafeChainFrameV1 &frame) {
    if (exists(frame.origin_id, frame.event_id)) return false;

    for (uint16_t i = 0; i < MAX_RECORDS; i++) {
        if (records[i].used) continue;

        records[i].used = true;
        strncpy(records[i].origin_id, frame.origin_id, sc::DEVICE_ID_LEN);
        records[i].origin_id[sc::DEVICE_ID_LEN - 1] = '\0';

        strncpy(records[i].first_sender_id, frame.sender_id, sc::DEVICE_ID_LEN);
        records[i].first_sender_id[sc::DEVICE_ID_LEN - 1] = '\0';

        records[i].event_id        = frame.event_id;
        records[i].event_type      = frame.event_type;
        records[i].first_rx_ms     = millis();
        records[i].lat_e7          = frame.lat_e7;
        records[i].lon_e7          = frame.lon_e7;
        records[i].battery_pct     = frame.battery_pct;
        records[i].hop_count       = frame.hop_count;
        records[i].rssi_dbm        = frame.last_rssi_dbm;
        records[i].attempt         = frame.attempt;
        records[i].status          = GW_EVT_RECEIVED;
        records[i].protocol_version= frame.protocol_version;
        records[i].payload_crc16   = frame.crc16;

        count++;

        // Persist immediately — power loss after this point is safe
        if (!nvsWriteSlot(i)) {
            Serial.printf(">>> WARNING: journal NVS write failed for slot %u\n", i);
        }

        return true;
    }

    Serial.println(">>> WARNING: journal full — event not recorded");
    return false;
}

// ---------------------------------------------------------------------------
// markHostQueued — update status and persist
// ---------------------------------------------------------------------------
bool GatewayJournal::markHostQueued(const char* originId, uint32_t eventId) {
    int idx = findIndex(originId, eventId);
    if (idx < 0) return false;

    records[idx].status = GW_EVT_HOST_QUEUED;
    nvsWriteSlot(idx);
    return true;
}

// ---------------------------------------------------------------------------
// markHostCommitted — update status, persist, then clear NVS slot
// Previously this function was never called. Now it is the terminal state:
// once committed, the record is cleared from NVS (no longer needs replay).
// ---------------------------------------------------------------------------
bool GatewayJournal::markHostCommitted(const char* originId, uint32_t eventId) {
    int idx = findIndex(originId, eventId);
    if (idx < 0) return false;

    records[idx].status = GW_EVT_HOST_COMMITTED;

    // Clear from NVS — committed records do not need replay on reboot
    nvsClearSlot(idx);

    Serial.printf(">>> Journal: event %lu from %s committed and cleared from NVS\n",
        (unsigned long)eventId, originId);
    return true;
}

// ---------------------------------------------------------------------------
// getRecord
// ---------------------------------------------------------------------------
const GatewayJournalRecord* GatewayJournal::getRecord(const char* originId, uint32_t eventId) const {
    int idx = findIndex(originId, eventId);
    if (idx < 0) return nullptr;
    return &records[idx];
}