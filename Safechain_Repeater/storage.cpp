#include "storage.h"
#include <string.h>

static constexpr uint32_t NODE_EVENT_MAGIC = 0x53434556; // "SCEV"

static uint16_t recordCRC16(const uint8_t* data, size_t len) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (uint8_t j = 0; j < 8; j++) {
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

Storage::Storage() {}

void Storage::init() {
    prefs.begin("safechain", false);
}

String Storage::getNodeID() {
    return prefs.getString("uid", DEFAULT_NODE_ID);
}

void Storage::setNodeID(const String &id) {
    prefs.putString("uid", id.c_str());
}

bool Storage::getRelayEnabled() {
    return prefs.getBool("relay", false);
}

void Storage::setRelayEnabled(bool enabled) {
    prefs.putBool("relay", enabled);
}

uint8_t Storage::getSpreadingFactor() {
    return prefs.getUChar("sf", LORA_SF);
}

void Storage::setSpreadingFactor(uint8_t sf) {
    prefs.putUChar("sf", sf);
}

uint32_t Storage::getEventCounter() {
    return prefs.getUInt("evt_ctr", 0);
}

uint32_t Storage::nextEventCounter() {
    uint32_t next = prefs.getUInt("evt_ctr", 0) + 1;
    prefs.putUInt("evt_ctr", next);
    return next;
}

void Storage::setEventCounter(uint32_t value) {
    prefs.putUInt("evt_ctr", value);
}

bool Storage::savePendingEvent(const NodeEventRecord &record) {
    NodeEventRecord tmp = record;
    tmp.magic = NODE_EVENT_MAGIC;
    tmp.crc16 = 0;
    tmp.crc16 = recordCRC16(reinterpret_cast<const uint8_t*>(&tmp),
                             sizeof(NodeEventRecord) - sizeof(tmp.crc16));
    return prefs.putBytes("pend_evt", &tmp, sizeof(NodeEventRecord))
           == sizeof(NodeEventRecord);
}

bool Storage::loadPendingEvent(NodeEventRecord &record) {
    if (!prefs.isKey("pend_evt")) return false;

    NodeEventRecord tmp{};
    if (prefs.getBytes("pend_evt", &tmp, sizeof(NodeEventRecord))
        != sizeof(NodeEventRecord)) return false;
    if (tmp.magic != NODE_EVENT_MAGIC) return false;

    uint16_t rx = tmp.crc16;
    tmp.crc16 = 0;
    if (rx != recordCRC16(reinterpret_cast<const uint8_t*>(&tmp),
                           sizeof(NodeEventRecord) - sizeof(tmp.crc16)))
        return false;

    tmp.crc16 = rx;
    record = tmp;
    return true;
}

void Storage::clearPendingEvent() {
    prefs.remove("pend_evt");
}

bool Storage::hasPendingEvent() {
    return prefs.isKey("pend_evt");
}

// ---------------------------------------------------------------------------
// [M3] PSK storage
// ---------------------------------------------------------------------------
void Storage::getPSK(uint8_t* out, size_t len) {
    if (len < sc::PSK_LEN) return;

    if (!prefs.isKey("psk")) {
        // No custom key provisioned — use default dev key
        memcpy(out, sc::PSK_DEFAULT, sc::PSK_LEN);
        return;
    }

    size_t read = prefs.getBytes("psk", out, len);
    if (read != sc::PSK_LEN) {
        // Corrupt NVS key — fall back to default
        memcpy(out, sc::PSK_DEFAULT, sc::PSK_LEN);
    }
}

void Storage::setPSK(const uint8_t* key, size_t len) {
    if (len != sc::PSK_LEN) {
        Serial.printf(">>> PSK must be exactly %u bytes\n", sc::PSK_LEN);
        return;
    }
    prefs.putBytes("psk", key, len);
}

bool Storage::hasPSK() {
    return prefs.isKey("psk");
}