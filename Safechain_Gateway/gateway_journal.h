#ifndef GATEWAY_JOURNAL_H
#define GATEWAY_JOURNAL_H

#include <Arduino.h>
#include <Preferences.h>
#include "safechain_protocol.h"

// ============================================================
// M2 — Persistent Gateway Journal
//
// Previously the journal was a plain in-memory array that was
// wiped on every reboot or power loss. Any events received but
// not yet confirmed to the host were permanently lost.
//
// This version backs every record write to NVS via Preferences.
// On boot, init() reloads all records and replays any that have
// not yet reached HOST_COMMITTED status.
//
// Storage layout (NVS namespace "gw_journal"):
//   Key "gj_NN"  (NN = 00..29) -> NvsJournalSlot (46 bytes)
//
// Each slot is CRC-protected. A corrupted or missing slot is
// treated as empty. MAX_NVS_SLOTS (30) caps NVS usage at
// ~1380 bytes — well within the default ESP32 partition.
// Records beyond slot 29 are kept in RAM only (same as before).
// ============================================================

enum GatewayEventStatus : uint8_t {
    GW_EVT_EMPTY        = 0,
    GW_EVT_RECEIVED     = 1,
    GW_EVT_HOST_QUEUED  = 2,
    GW_EVT_HOST_COMMITTED = 3
};

struct GatewayJournalRecord {
    bool     used;
    char     origin_id[sc::DEVICE_ID_LEN];
    uint32_t event_id;
    uint8_t  event_type;
    char     first_sender_id[sc::DEVICE_ID_LEN];
    uint32_t first_rx_ms;
    int32_t  lat_e7;
    int32_t  lon_e7;
    uint8_t  battery_pct;
    uint8_t  hop_count;
    int16_t  rssi_dbm;
    uint16_t attempt;
    uint8_t  status;
    uint8_t  protocol_version;
    uint16_t payload_crc16;
};

// NVS-persisted wrapper: magic + record + crc16
struct __attribute__((packed)) NvsJournalSlot {
    uint32_t           magic;    // 0x474A4F55 = "GJOU"
    GatewayJournalRecord record;
    uint16_t           crc16;
};

class GatewayJournal {
private:
    static const uint16_t MAX_RECORDS  = 100; // total in-memory slots
    static const uint16_t MAX_NVS_SLOTS = 30; // slots backed to NVS
    static const uint32_t SLOT_MAGIC   = 0x474A4F55; // "GJOU"

    GatewayJournalRecord records[MAX_RECORDS];
    uint16_t count;

    Preferences prefs;
    bool        nvsOpen;

    // NVS helpers
    void     nvsOpen_();
    void     nvsClose_();
    bool     nvsWriteSlot(uint16_t idx);
    bool     nvsReadSlot(uint16_t idx, GatewayJournalRecord &out);
    void     nvsClearSlot(uint16_t idx);
    uint16_t calcCRC16(const uint8_t* data, size_t len) const;
    void     slotKey(uint16_t idx, char* buf) const; // "gj_00" format

public:
    GatewayJournal();

    // init() loads NVS records into RAM. Returns number of uncommitted
    // records found (caller should call replayUncommitted() after).
    uint16_t init();

    // Returns pointer to records that need replay (status < HOST_COMMITTED).
    // Caller iterates 0..replayCount-1 via getReplayRecord().
    uint16_t          replayCount() const;
    const GatewayJournalRecord* getReplayRecord(uint16_t replayIdx) const;

    bool exists(const char* originId, uint32_t eventId) const;
    int  findIndex(const char* originId, uint32_t eventId) const;

    bool appendIfNew(const sc::SafeChainFrameV1 &frame);
    bool markHostQueued(const char* originId, uint32_t eventId);
    // markHostCommitted() is now fully wired — clears NVS slot after commit
    bool markHostCommitted(const char* originId, uint32_t eventId);

    const GatewayJournalRecord* getRecord(const char* originId, uint32_t eventId) const;

    uint16_t size() const { return count; }
};

#endif