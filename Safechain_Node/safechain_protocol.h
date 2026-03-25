#ifndef SAFECHAIN_PROTOCOL_H
#define SAFECHAIN_PROTOCOL_H

#include <Arduino.h>

// ============================================================
// M3 — Security Baseline
//
// SafeChainFrameV1 now carries a 4-byte auth_tag (HMAC-SHA256
// truncated to 32 bits) before the existing crc16 field.
// Frame size grows from 41 → 45 bytes.
//
// HMAC covers only the fields that are STABLE across hops:
//   protocol_version, frame_type, event_type, flags,
//   origin_id, event_id, attempt, max_hops,
//   event_time_ms, lat_e7, lon_e7, battery_pct
//
// Mutable-per-hop fields (sender_id, hop_count, last_rssi_dbm)
// are NOT authenticated — they are protected only by crc16.
//
// This means the repeater can relay without recomputing auth_tag.
// Only the originating node sets auth_tag; all receivers verify it.
//
// Key: 16-byte PSK stored in NVS. Default dev key is
// "SafeChain_Dev01" (all devices use this until provisioned).
// Provision via Serial: setpsk <32 hex chars>
// ============================================================

namespace sc {

static constexpr uint8_t  PROTOCOL_VERSION = 1;
static constexpr uint8_t  DEVICE_ID_LEN    = 6;
static constexpr uint8_t  MAX_HOPS_DEFAULT = 20; 
static constexpr uint8_t  PSK_LEN          = 16;  // [M3]

// Default development PSK — change in production via setpsk command
static const char PSK_DEFAULT[PSK_LEN + 1] = "SafeChain_Dev01"; // 15 chars + null

// Transport-level frame meaning
enum FrameType : uint8_t {
    FRAME_EVENT     = 0x01,
    FRAME_ACK       = 0x02,
    FRAME_HEARTBEAT = 0x03,
    FRAME_STATUS    = 0x04,
    FRAME_CONFIG    = 0x05
};

// Emergency/business meaning
enum EventType : uint8_t {
    EVENT_NONE  = 0x00,
    EVENT_FIRE  = 0x01,
    EVENT_FLOOD = 0x02,
    EVENT_CRIME = 0x03,
    EVENT_SAFE  = 0x04,
    EVENT_TEST  = 0xFF
};

enum FrameFlags : uint8_t {
    FLAG_REQUIRE_ACK    = 1 << 0,
    FLAG_LOW_BATTERY    = 1 << 1,
    FLAG_GPS_VALID      = 1 << 2,
    FLAG_HOST_COMMITTED = 1 << 3,
    FLAG_AUTH_PRESENT   = 1 << 4   // [M3] set when auth_tag is valid
};

// [M6] Configuration keys — carried in event_type field of FRAME_CONFIG
// config_value is carried in event_time_ms (uint32).
// origin_id = target node ID, or "ALL" for broadcast.
enum ConfigKey : uint8_t {
    CONFIG_SF           = 0x01,  // Spreading factor (7-12)
    CONFIG_TX_POWER     = 0x02,  // TX power in dBm (2-20)
    CONFIG_HB_INTERVAL  = 0x03,  // Heartbeat interval in ms
    CONFIG_REBOOT       = 0x04   // Trigger soft reboot (value ignored)
};

// Broadcast target — origin_id value meaning "apply to all nodes"
static const char CONFIG_TARGET_ALL[DEVICE_ID_LEN] = "ALL";

struct __attribute__((packed)) SafeChainFrameV1 {
    uint8_t  protocol_version;
    uint8_t  frame_type;
    uint8_t  event_type;
    uint8_t  flags;

    char     origin_id[DEVICE_ID_LEN];
    char     sender_id[DEVICE_ID_LEN];

    uint32_t event_id;
    uint16_t attempt;
    uint8_t  hop_count;
    uint8_t  max_hops;

    uint32_t event_time_ms;
    int32_t  lat_e7;
    int32_t  lon_e7;

    uint8_t  battery_pct;
    int16_t  last_rssi_dbm;

    // [M3] Authentication tag — HMAC-SHA256 truncated to 32 bits.
    // Covers stable inner payload (see computeHMAC32).
    // Must be computed before crc16.
    uint32_t auth_tag;

    // Wire integrity over entire frame above (including auth_tag)
    uint16_t crc16;
};

struct DedupKey {
    char     origin_id[DEVICE_ID_LEN];
    uint32_t event_id;
    uint8_t  frame_type;
};

class Protocol {
public:
    static uint16_t calcCRC16(const uint8_t* data, size_t len);

    // [M3] Compute 4-byte truncated HMAC-SHA256 over the stable inner payload
    static uint32_t computeHMAC32(const uint8_t* psk, size_t pskLen,
                                   const SafeChainFrameV1& frame);

    static void initFrame(SafeChainFrameV1& frame);

    // [M3] finalizeFrame now requires PSK to compute auth_tag
    static void finalizeFrame(SafeChainFrameV1& frame,
                               const uint8_t* psk, size_t pskLen);

    // [M3] validateFrame checks both crc16 and auth_tag
    static bool validateFrame(const SafeChainFrameV1& frame,
                               const uint8_t* psk, size_t pskLen);

    static void buildEventFrame(SafeChainFrameV1& frame,
                                const char* originId,
                                const char* senderId,
                                uint32_t eventId,
                                EventType eventType,
                                uint16_t attempt,
                                uint32_t eventTimeMs,
                                int32_t latE7,
                                int32_t lonE7,
                                uint8_t batteryPct,
                                bool gpsValid,
                                bool lowBattery,
                                const uint8_t* psk, size_t pskLen,
                                uint8_t maxHops = MAX_HOPS_DEFAULT);

    static void buildAckFrame(SafeChainFrameV1& frame,
                              const char* gatewayOrRepeaterId,
                              const char* originId,
                              uint32_t eventId,
                              EventType eventType,
                              const uint8_t* psk, size_t pskLen,
                              uint8_t maxHops = MAX_HOPS_DEFAULT);

    // prepareRelay only updates sender_id/hop_count (not auth_tag — HMAC stable)
    // but must recompute crc16 since sender_id and hop_count changed
    static bool prepareRelay(SafeChainFrameV1& frame, const char* newSenderId,
                              const uint8_t* psk, size_t pskLen);

    static bool makeDedupKey(const SafeChainFrameV1& frame, DedupKey& out);

    static bool isEmergencyEvent(const SafeChainFrameV1& frame);

    static const char* frameTypeName(uint8_t frameType);
    static const char* eventTypeName(uint8_t eventType);
};

} // namespace sc

#endif