#include "safechain_protocol.h"
#include <string.h>
#include "mbedtls/md.h"  // HMAC-SHA256 — built into ESP32 Arduino core

namespace sc {

// ---------------------------------------------------------------------------
// CRC16-CCITT (unchanged)
// ---------------------------------------------------------------------------
uint16_t Protocol::calcCRC16(const uint8_t* data, size_t len) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (uint8_t j = 0; j < 8; j++) {
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

// ---------------------------------------------------------------------------
// [M3] computeHMAC32 — HMAC-SHA256 over the stable inner payload,
// truncated to 4 bytes (first 4 bytes of the 32-byte output).
//
// Inner payload covers only fields that do NOT change per hop:
//   protocol_version, frame_type, event_type, flags,
//   origin_id, event_id, attempt, max_hops,
//   event_time_ms, lat_e7, lon_e7, battery_pct
// (30 bytes total)
//
// Excluded (change per hop):
//   sender_id, hop_count, last_rssi_dbm, auth_tag, crc16
// ---------------------------------------------------------------------------
uint32_t Protocol::computeHMAC32(const uint8_t* psk, size_t pskLen,
                                   const SafeChainFrameV1& frame) {
    // Build the 30-byte stable inner payload
    uint8_t inner[30];
    size_t  pos = 0;

    inner[pos++] = frame.protocol_version;
    inner[pos++] = frame.frame_type;
    inner[pos++] = frame.event_type;
    inner[pos++] = frame.flags;

    memcpy(inner + pos, frame.origin_id, DEVICE_ID_LEN); pos += DEVICE_ID_LEN; // 6

    // Little-endian copy of multi-byte fields (packed struct, memcpy is safe)
    memcpy(inner + pos, &frame.event_id,      4); pos += 4;
    memcpy(inner + pos, &frame.attempt,       2); pos += 2;
    inner[pos++] = frame.max_hops;
    memcpy(inner + pos, &frame.event_time_ms, 4); pos += 4;
    memcpy(inner + pos, &frame.lat_e7,        4); pos += 4;
    memcpy(inner + pos, &frame.lon_e7,        4); pos += 4;
    inner[pos++] = frame.battery_pct;

    // pos should be exactly 30
    // static_assert commented to allow runtime assert in debug builds
    // assert(pos == 30);

    // HMAC-SHA256 via mbedTLS (always available in ESP32 Arduino core)
    uint8_t hmac_out[32];
    mbedtls_md_hmac(
        mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
        psk, pskLen,
        inner, pos,
        hmac_out
    );

    // Truncate to 4 bytes
    uint32_t tag;
    memcpy(&tag, hmac_out, 4);
    return tag;
}

// ---------------------------------------------------------------------------
// initFrame
// ---------------------------------------------------------------------------
void Protocol::initFrame(SafeChainFrameV1& frame) {
    memset(&frame, 0, sizeof(frame));
    frame.protocol_version = PROTOCOL_VERSION;
    frame.max_hops         = MAX_HOPS_DEFAULT;
}

// ---------------------------------------------------------------------------
// [M3] finalizeFrame — compute auth_tag then crc16
// Order matters: auth_tag must be set before CRC16 is computed so CRC16
// covers auth_tag.
// ---------------------------------------------------------------------------
void Protocol::finalizeFrame(SafeChainFrameV1& frame,
                               const uint8_t* psk, size_t pskLen) {
    // Step 1: compute and set auth_tag
    frame.flags   |= FLAG_AUTH_PRESENT;
    frame.auth_tag = computeHMAC32(psk, pskLen, frame);
   

    // Step 2: compute CRC16 over everything except the last 2 bytes (crc16 field)
    frame.crc16 = 0;
    frame.crc16 = calcCRC16(reinterpret_cast<const uint8_t*>(&frame),
                             sizeof(SafeChainFrameV1) - sizeof(frame.crc16));
}

// ---------------------------------------------------------------------------
// [M3] validateFrame — check crc16, then check auth_tag
// Returns false if either check fails.
// ---------------------------------------------------------------------------
bool Protocol::validateFrame(const SafeChainFrameV1& frame,
                               const uint8_t* psk, size_t pskLen) {
    if (frame.protocol_version != PROTOCOL_VERSION) return false;

    // Step 1: CRC16 wire integrity check
    SafeChainFrameV1 tmp = frame;
    uint16_t rx_crc = tmp.crc16;
    tmp.crc16 = 0;
    uint16_t ex_crc = calcCRC16(reinterpret_cast<const uint8_t*>(&tmp),
                                 sizeof(SafeChainFrameV1) - sizeof(tmp.crc16));
    if (rx_crc != ex_crc) return false;

    // Step 2: HMAC authentication check (only if auth flag is set)
    if (frame.flags & FLAG_AUTH_PRESENT) {
        uint32_t expected_tag = computeHMAC32(psk, pskLen, frame);
        if (frame.auth_tag != expected_tag) {
            Serial.printf(">>> AUTH FAIL: origin=%s event=%lu — invalid HMAC\n",
                frame.origin_id, (unsigned long)frame.event_id);
            return false;
        }
    }

    return true;
}

// ---------------------------------------------------------------------------
// buildEventFrame
// ---------------------------------------------------------------------------
void Protocol::buildEventFrame(SafeChainFrameV1& frame,
                                const char* originId, const char* senderId,
                                uint32_t eventId, EventType eventType,
                                uint16_t attempt, uint32_t eventTimeMs,
                                int32_t latE7, int32_t lonE7,
                                uint8_t batteryPct, bool gpsValid, bool lowBattery,
                                const uint8_t* psk, size_t pskLen,
                                uint8_t maxHops) {
    initFrame(frame);

    frame.frame_type = FRAME_EVENT;
    frame.event_type = eventType;
    frame.flags      = FLAG_REQUIRE_ACK;
    if (gpsValid)   frame.flags |= FLAG_GPS_VALID;
    if (lowBattery) frame.flags |= FLAG_LOW_BATTERY;

    strncpy(frame.origin_id, originId, DEVICE_ID_LEN - 1);
    strncpy(frame.sender_id, senderId, DEVICE_ID_LEN - 1);

    frame.event_id      = eventId;
    frame.attempt       = attempt;
    frame.hop_count     = 0;
    frame.max_hops      = maxHops;
    frame.event_time_ms = eventTimeMs;
    frame.lat_e7        = latE7;
    frame.lon_e7        = lonE7;
    frame.battery_pct   = batteryPct;
    frame.last_rssi_dbm = 0;

    finalizeFrame(frame, psk, pskLen);
}

// ---------------------------------------------------------------------------
// buildAckFrame
// ---------------------------------------------------------------------------
void Protocol::buildAckFrame(SafeChainFrameV1& frame,
                              const char* gatewayOrRepeaterId,
                              const char* originId,
                              uint32_t eventId, EventType eventType,
                              const uint8_t* psk, size_t pskLen,
                              uint8_t maxHops) {
    initFrame(frame);

    frame.frame_type = FRAME_ACK;
    frame.event_type = eventType;
    frame.flags      = 0;

    strncpy(frame.origin_id, originId,              DEVICE_ID_LEN - 1);
    strncpy(frame.sender_id, gatewayOrRepeaterId,   DEVICE_ID_LEN - 1);

    frame.event_id      = eventId;
    frame.attempt       = 0;
    frame.hop_count     = 0;
    frame.max_hops      = maxHops;
    frame.event_time_ms = millis();
    frame.battery_pct   = 100;
    frame.last_rssi_dbm = 0;

    finalizeFrame(frame, psk, pskLen);
}

// ---------------------------------------------------------------------------
// prepareRelay — update mutable hop fields and recompute crc16.
// auth_tag is NOT recomputed (HMAC covers stable inner payload only).
// ---------------------------------------------------------------------------
bool Protocol::prepareRelay(SafeChainFrameV1& frame, const char* newSenderId,
                              const uint8_t* psk, size_t pskLen) {
    if (frame.hop_count >= frame.max_hops) return false;

    frame.hop_count++;
    strncpy(frame.sender_id, newSenderId, DEVICE_ID_LEN - 1);
    frame.sender_id[DEVICE_ID_LEN - 1] = '\0';

    // Recompute CRC16 only — auth_tag remains unchanged (stable fields intact)
    frame.crc16 = 0;
    frame.crc16 = calcCRC16(reinterpret_cast<const uint8_t*>(&frame),
                             sizeof(SafeChainFrameV1) - sizeof(frame.crc16));

    // Suppress unused parameter warning — psk may be used in future
    (void)psk; (void)pskLen;
    return true;
}

// ---------------------------------------------------------------------------
// Utilities (unchanged)
// ---------------------------------------------------------------------------
bool Protocol::makeDedupKey(const SafeChainFrameV1& frame, DedupKey& out) {
    strncpy(out.origin_id, frame.origin_id, DEVICE_ID_LEN);
    out.event_id   = frame.event_id;
    out.frame_type = frame.frame_type;
    return true;
}

bool Protocol::isEmergencyEvent(const SafeChainFrameV1& frame) {
    if (frame.frame_type != FRAME_EVENT) return false;
    return frame.event_type == EVENT_FIRE  ||
           frame.event_type == EVENT_FLOOD ||
           frame.event_type == EVENT_CRIME ||
           frame.event_type == EVENT_SAFE;
}

const char* Protocol::frameTypeName(uint8_t frameType) {
    switch (frameType) {
        case FRAME_EVENT:     return "EVENT";
        case FRAME_ACK:       return "ACK";
        case FRAME_HEARTBEAT: return "HEARTBEAT";
        case FRAME_STATUS:    return "STATUS";
        case FRAME_CONFIG:    return "CONFIG";
        default:              return "UNKNOWN";
    }
}

const char* Protocol::eventTypeName(uint8_t eventType) {
    switch (eventType) {
        case EVENT_NONE:  return "NONE";
        case EVENT_FIRE:  return "FIRE";
        case EVENT_FLOOD: return "FLOOD";
        case EVENT_CRIME: return "CRIME";
        case EVENT_SAFE:  return "SAFE";
        case EVENT_TEST:  return "TEST";
        default:          return "UNKNOWN";
    }
}

} // namespace sc