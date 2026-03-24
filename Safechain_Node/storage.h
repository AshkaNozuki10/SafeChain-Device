#ifndef STORAGE_H
#define STORAGE_H

#include <Arduino.h>
#include <Preferences.h>
#include "config.h"
#include "safechain_protocol.h"

struct NodeEventRecord {
    uint32_t magic;
    uint32_t event_id;
    uint8_t  event_type;
    uint16_t attempt;
    uint8_t  state;       // 1=pending_ack, 2=acked, 3=cleared
    uint8_t  battery_pct;
    int32_t  lat_e7;
    int32_t  lon_e7;
    uint32_t created_ms;
    uint32_t updated_ms;
    uint16_t crc16;
};

class Storage {
private:
    Preferences prefs;

public:
    Storage();
    void init();

    String  getNodeID();
    void    setNodeID(const String &id);

    bool    getRelayEnabled();
    void    setRelayEnabled(bool enabled);

    uint8_t getSpreadingFactor();
    void    setSpreadingFactor(uint8_t sf);

    uint32_t getEventCounter();
    uint32_t nextEventCounter();
    void     setEventCounter(uint32_t value);

    bool savePendingEvent(const NodeEventRecord &record);
    bool loadPendingEvent(NodeEventRecord &record);
    void clearPendingEvent();
    bool hasPendingEvent();

    // [M6] TX power — persisted so it survives reboot after OTA config
    uint8_t getTxPower();
    void    setTxPower(uint8_t dbm);

    // [M6] Heartbeat interval — persisted so it survives reboot after OTA config
    uint32_t getHBInterval();
    void     setHBInterval(uint32_t ms);

    // [M6] Config dedup — last applied config event_id, prevents replay
    uint32_t getLastConfigId();
    void     setLastConfigId(uint32_t id);

    // [M3] PSK storage — 16-byte pre-shared key for HMAC authentication
    // Default: sc::PSK_DEFAULT ("SafeChain_Dev01")
    // Provision via: setpsk <32 hex chars>
    void getPSK(uint8_t* out, size_t len);
    void setPSK(const uint8_t* key, size_t len);
    bool hasPSK(); // returns true if a custom key has been provisioned
};

#endif