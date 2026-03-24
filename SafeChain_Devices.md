# SafeChain M0–M6: Complete Technical Deep-Dive

## Overview

SafeChain is a LoRa-based emergency alert network running on ESP32 microcontrollers. Three device roles communicate over 433 MHz radio using a custom binary protocol:

- **Node** — a wearable or fixed panic button that originates emergency alerts
- **Repeater** — a relay device that extends RF range by forwarding packets
- **Gateway** — the receiver that journals events and connects to host software

This document explains every algorithm, data structure, and engineering decision from M0 through M6 in enough depth that you can locate, understand, and modify any part of the system.

---

## The Wire Protocol — SafeChainFrameV1

Before explaining the milestones, you need to understand the single binary frame that every device sends and receives.

```
Byte  0     protocol_version  (always 0x01)
Byte  1     frame_type        (EVENT=0x01, ACK=0x02, HEARTBEAT=0x03,
                               STATUS=0x04, CONFIG=0x05)
Byte  2     event_type        (FIRE=0x01, FLOOD=0x02, CRIME=0x03,
                               SAFE=0x04, TEST=0xFF, NONE=0x00)
                               For FRAME_CONFIG: holds ConfigKey
Byte  3     flags             (bitfield: REQUIRE_ACK, LOW_BATTERY,
                               GPS_VALID, HOST_COMMITTED, AUTH_PRESENT)
Bytes 4–9   origin_id[6]      original creator node ID (e.g. "FOB01")
                               For FRAME_CONFIG: target node ID or "ALL"
Bytes 10–15 sender_id[6]      the device that sent this specific hop
Bytes 16–19 event_id          stable uint32 across all retries and relays
                               For FRAME_CONFIG: dedup ID from gateway counter
Bytes 20–21 attempt           retry counter (0 = first TX)
Byte  22    hop_count         incremented by each repeater
Byte  23    max_hops          ceiling (default 10)
Bytes 24–27 event_time_ms     millis() at event creation
                               For FRAME_CONFIG: config_value (SF, dBm, ms)
Bytes 28–31 lat_e7            latitude × 10,000,000 as int32
Bytes 32–35 lon_e7            longitude × 10,000,000 as int32
Byte  36    battery_pct       0–100
Bytes 37–38 last_rssi_dbm     RSSI filled in by receiver, not sender
Bytes 39–42 auth_tag          HMAC-SHA256 truncated to 32 bits [M3]
Bytes 43–44 crc16             CRC-CCITT over bytes 0–42
```

**Total: 45 bytes.** This is sent over LoRa in a single packet. At SF11, 125 kHz bandwidth, the air time is approximately 800 ms. The frame is `__attribute__((packed))` which tells the compiler to use no padding bytes — every field is contiguous in memory, so `LoRa.write((uint8_t*)&frame, sizeof(frame))` sends exactly 45 bytes with no gaps.

The **separation of origin_id and sender_id** is the key design insight that makes multi-hop work. `origin_id` never changes — it is always the node that triggered the alert. `sender_id` changes at each hop to identify who last transmitted. The gateway uses `origin_id` to route the ACK back to the right device. The repeater uses `sender_id` to avoid relaying its own packets back.

---

## M0 — Safety Fixes

M0 is a collection of seven independent bug fixes. None of them add features — they fix ways the existing code could silently fail in production.

### M0-1: millis() Overflow (All Three Devices)

**The problem.** The original code used this pattern everywhere:

```cpp
nextActionAt = millis() + 5000;
// ...later...
if (millis() >= nextActionAt) { ... }
```

`millis()` is a `uint32_t` that counts milliseconds since boot. It overflows back to zero after 49.7 days. If `millis()` is near the overflow boundary, say at `4294964000`, then `millis() + 5000` wraps to `1704`, producing a `nextActionAt` in the past. The condition `millis() >= nextActionAt` is then immediately true, causing the action to fire instantly rather than waiting 5 seconds.

**The fix.** Subtract instead of compare absolute values:

```cpp
// Schedule:
actionScheduledAt = millis();
actionDelayMs     = 5000;

// Check:
bool isActionDue() const {
    return (millis() - actionScheduledAt) >= actionDelayMs;
}
```

This works because unsigned subtraction in C++ wraps correctly. If `millis()` overflows from `4294967295` to `0`, then `0 - 4294965295 = 1000` (correct elapsed time) in unsigned arithmetic. The elapsed time is always correct regardless of overflow.

This pattern was applied in three places: `EmergencyManager` (Node), `RepeaterRouter` (Repeater), and `EventManager` (Gateway).

### M0-2: Non-Blocking Siren (Node)

**The problem.** The original `triggerFlood()` function contained:

```cpp
for(int i=0; i<3; i++) {
    tone(PIN_BUZZER, 2000, 300);
    delay(600);
}
```

`delay()` blocks the entire CPU for 1.8 seconds total. During that time, no LoRa packets are received, no buttons are processed, and the emergency state machine does not advance. The first transmission could not happen until the siren finished.

**The fix.** A non-blocking siren uses a step table and a state machine driven by `millis()`:

```cpp
struct SirenStep { uint16_t freq; uint16_t dur_ms; };

static const SirenStep SIREN_FLOOD[] = {
    {2000,300},{0,300},{2000,300},{0,300},{2000,300},{0,350}
};
```

`sirenPlay()` starts the first step and returns immediately. `sirenUpdate()` is called every `loop()` tick. It checks if the current step has elapsed — if not, it returns immediately. If yes, it advances to the next step and plays that tone. When all steps are done, it clears `activeSirenSteps` and stops. The siren runs concurrently with everything else.

### M0-3: Non-Blocking Gateway ACK (Gateway)

**The problem.** When the gateway received an emergency frame, it immediately called `sendAckV1()`, which contained `delay(random(100, 300))`. This was intended to stagger ACKs to avoid RF collisions, but it blocked the entire gateway loop. During those 100–300 ms the gateway could not receive the next incoming packet.

**The fix.** Split into schedule and fire:

```cpp
// On receive:
void scheduleAckV1(const sc::SafeChainFrameV1 &frame) {
    pendingAckFrame = frame;
    ackScheduledAt  = millis();
    ackDelayMs      = random(100, 300);
    pendingAckValid = true;
}

// In loop(), called first before parsePacket():
void update() {
    if (pendingAckValid && (millis() - ackScheduledAt) >= ackDelayMs) {
        pendingAckValid = false;
        // ... build and transmit ACK ...
    }
}
```

`eventMgr.update()` must be the **first call in `loop()`**, before `LoRa.parsePacket()`. This ensures the ACK fires at the correct time even if a new packet arrives in the same loop iteration.

### M0-4: Resume Before Wakeup (Node)

**The problem.** When the ESP32 wakes from deep sleep, it runs `setup()` from the beginning. If the node had an emergency event in progress when it went to sleep, that event is stored in NVS. But the wakeup handler also checked which button caused the wake and called `triggerFlood()` / `triggerFire()` etc., which created a **new** event and overwrote the NVS record. The original pending event was silently lost.

**The fix.** Call `resumePending()` before checking wakeup pins:

```cpp
bool resumed = emergency.resumePending();

if (!resumed && wakeup_pin_mask != 0) {
    // Only process button wakeup if there was nothing to resume
    if (wakeup_pin_mask & (1ULL << PIN_BTN_FLOOD)) triggerFlood();
    // ...
} else if (resumed) {
    Serial.println(">>> Skipped wakeup — NVS pending event has priority");
}
```

If `resumePending()` returns `true`, it means there was an unacknowledged event that was interrupted. That event takes priority. The button that caused the wakeup is ignored this one time — the user pressed it to escalate, but the system correctly prioritizes completing the in-flight alert first.

### M0-5: Repeater NVS Self-ID (Repeater)

**The problem.** The repeater router used `DEFAULT_NODE_ID` (a compile-time macro, always `"REP01"`) to filter its own packets and stamp relayed packets. If you changed the repeater's node ID via the `uid` command, the NVS stored the new ID, but the router still used `"REP01"`. This meant:

1. The repeater would relay its own packets back (self-loop)
2. Relayed packets carried the wrong sender ID

**The fix.** `RepeaterRouter::init(const char* nodeId)` now takes the NVS-sourced ID as a parameter. The gateway calls this after `storage.init()`:

```cpp
storage.init();
String nodeId = storage.getNodeID();
router.init(nodeId.c_str()); // NVS ID, not DEFAULT_NODE_ID
```

The `ownNodeID` field is then used in both `shouldRelayV1()` (self-filter) and `queueRelayV1()` (sender stamp).

### M0-6: FAILED State Auto-Recovery (Node)

**The problem.** If the gateway was unreachable and all three retries failed, the node entered `EM_STATE_FAILED` and stayed there permanently. The user had to physically reboot to retry. In an actual emergency, the person may not be able to reboot — they just need the device to keep trying.

**The fix.** `markFailed()` schedules a 60-second auto-retry instead of being a terminal state:

```cpp
void EmergencyManager::markFailed() {
    // ...alert user...
    saveJournal(JOURNAL_PENDING_ACK); // keep NVS record alive
    setState(EM_STATE_FAILED);
    scheduleAction(FAILED_RETRY_INTERVAL_MS); // 60,000 ms
}

// In update():
case EM_STATE_FAILED:
    if (isActionDue() && pendingFrameValid) {
        retryCount = 0;
        setState(EM_STATE_PENDING_TX);
        scheduleAction(0); // transmit on next tick
    }
    break;
```

The event stays in NVS throughout. If the gateway comes back online or the node moves into range, it will automatically resume transmitting.

### M0-7: Gateway TX Power (Gateway)

A one-line fix: the original gateway `setup()` never called `LoRa.setTxPower()`. The radio used whatever power level the library defaulted to (typically 17 dBm), but the config specified 20 dBm. Added to both `setup()` and `loraReinit()`.

---

## M1 — Watchdog and LoRa Fault Recovery

M1 makes all three devices self-healing. Without it, a hardware glitch or deadlock would leave the device silently broken until someone physically reset it.

### M1-1: Hardware Watchdog (All Three Devices)

The ESP32 has a built-in hardware timer that reboots the CPU if the software doesn't "feed" it on schedule. The IDF5 API uses a struct configuration:

```cpp
esp_task_wdt_config_t wdt_config = {
    .timeout_ms    = WDT_TIMEOUT_S * 1000,  // 10s Node, 15s Rep/GW
    .idle_core_mask = 0,
    .trigger_panic  = true  // causes a stack trace dump before reboot
};
esp_task_wdt_init(&wdt_config);
esp_task_wdt_add(NULL); // watch the current task (loop task)
```

`esp_task_wdt_reset()` is called as the **very first line of `loop()`**. If the device ever hangs — perhaps inside a blocking LoRa TX or a deadlocked mutex — the watchdog fires after 10–15 seconds and reboots the device. `trigger_panic = true` means it dumps a stack trace to Serial before rebooting, which is invaluable for debugging.

The Node uses 10 seconds because LoRa TX at SF11 takes ~800 ms, leaving comfortable margin. The Repeater and Gateway use 15 seconds because they have no deep sleep and sometimes do longer serial parsing.

### M1-2: Reboot Reason Logging (All Three Devices)

```cpp
esp_reset_reason_t reason = esp_reset_reason();
switch (reason) {
    case ESP_RST_TASK_WDT: rs = "TASK-WATCHDOG"; break;
    case ESP_RST_PANIC:    rs = "panic";         break;
    // ...
}
```

`RTC_DATA_ATTR int bootCount` and `RTC_DATA_ATTR int loraReinitCount` survive deep sleep because they live in the RTC memory domain — a small region of SRAM that stays powered during deep sleep. On every boot, these counters tell you how many times the device has rebooted and how many times the LoRa chip has needed hardware recovery.

### M1-3: LoRa Health Check and Reinit (All Three Devices)

**The problem being solved.** The SX1276/SX1278 LoRa chip can enter an undefined state due to RF interference, power glitches, or the radio being in TX mode when a function expects RX mode. When this happens, the chip stops receiving packets silently — no error is returned.

**Stage 1 — Soft recovery.** `LoRa.receive()` is called every 30 seconds to ensure the chip is in receive mode. This costs nothing.

**Stage 2 — Hardware verification.** The SX127x has a read-only register at address `0x42` (REG_VERSION) that always returns `0x12` on genuine SX1276/SX1278 chips. If it returns anything else, the chip is in a bad state:

```cpp
uint8_t readLoRaRegister(uint8_t reg) {
    digitalWrite(SS_PIN, LOW);
    SPI.transfer(reg & 0x7F); // MSB=0 = read mode for SX127x
    uint8_t val = SPI.transfer(0x00);
    digitalWrite(SS_PIN, HIGH);
    return val;
}
```

Note: `LoRa.readRegister()` exists in the library but is declared `private`. The direct SPI read bypasses this restriction.

**Stage 3 — Hardware reinit.** If the register check fails, `loraReinit()` runs:

```cpp
bool loraReinit() {
    LoRa.end();
    delay(150); // let SPI bus settle

    // Hardware RST pulse — forces chip back to power-on state
    digitalWrite(RST_PIN, HIGH); delay(10);
    digitalWrite(RST_PIN, LOW);  delay(10);
    digitalWrite(RST_PIN, HIGH); delay(20);

    // Three attempts to re-initialize
    for (int attempt = 1; attempt <= 3; attempt++) {
        if (LoRa.begin(LORA_FREQ)) { ok = true; break; }
        delay(200);
    }
    // Restore all radio settings...
}
```

The health check is skipped during `EM_STATE_PENDING_TX` on the Node because it would interrupt an in-progress transmission.

---

## M2 — Gateway Persistent Journal

### The Problem

Before M2, the gateway stored all received events in a plain array in RAM:

```cpp
GatewayJournalRecord records[100];
```

Every power cut, reboot, or crash wiped this array. If the gateway received an emergency alert, printed the CSV line, and then lost power before the host PC read it — the alert was gone. The node would retry, but the gateway would treat the retry as a duplicate and silently drop it.

### The Architecture

M2 backs every record write to NVS using the ESP32 `Preferences` library. The NVS (Non-Volatile Storage) is a key-value store in flash memory that survives power loss.

**Storage layout.** Each journal record is 40 bytes. It is wrapped in an `NvsJournalSlot` struct:

```cpp
struct __attribute__((packed)) NvsJournalSlot {
    uint32_t magic;             // 0x474A4F55 "GJOU" — format version check
    GatewayJournalRecord record; // 40 bytes of data
    uint16_t crc16;             // CRC over magic + record
};
// Total: 4 + 40 + 2 = 46 bytes per slot
```

Slots are stored with keys `gj_00` through `gj_29` in NVS namespace `gw_journal`. Total NVS usage: 46 × 30 = 1,380 bytes — well within the default ESP32 NVS partition (~20 KB).

**Why per-slot keys rather than one big blob.** Writing all 100 records as a single value would require 4,000+ bytes per write. NVS values are limited to about 4,000 bytes, and writing the entire array every time a single record changes is wasteful in both time and flash wear. Per-slot keys mean a single event update writes exactly 46 bytes.

**Write-through on every state change.** The record is written to NVS immediately at three points:

1. `appendIfNew()` — record arrives, status `RECEIVED`
2. `markHostQueued()` — CSV emitted to serial, status `HOST_QUEUED`
3. `markHostCommitted()` — host confirms storage, status `HOST_COMMITTED`, NVS slot cleared

After `markHostCommitted()`, the NVS slot is deleted. The record stays in RAM for the session but will not be replayed on the next boot.

### Boot Replay

On gateway startup, `journal.init()` reads all 30 NVS slots. For each slot:

- If the CRC is invalid: slot is treated as empty (silent corruption recovery)
- If status is `HOST_COMMITTED`: slot is cleared from NVS (already delivered)
- If status is `RECEIVED` or `HOST_QUEUED`: the record is loaded into RAM

After loading, `replayUncommitted()` iterates all uncommitted records and re-emits the `REPLAY_CSV_V1,...` line to the serial port. Host software can parse this prefix and distinguish it from fresh events. A re-ACK is also scheduled so the node stops retrying if it is still alive.

The host calls `commit FOB01 42` (node ID + event ID) over serial when it has durably stored an event. This calls `hostCommit()` which transitions to `HOST_COMMITTED` and clears the NVS slot.

---

## M3 — HMAC Frame Authentication

### The Security Problem

Before M3, any device that knew the LoRa frequency, sync word, and spreading factor could transmit a valid-looking packet. A CRC only checks wire integrity — it does not prove the sender is legitimate. An attacker with a cheap LoRa module could flood the network with fake emergencies or fake ACKs.

### HMAC-SHA256 and Why It Works Here

HMAC (Hash-based Message Authentication Code) is a standard construction that uses a secret key to produce a tag for a message. Only a device that knows the key can produce a valid tag. Verifying the tag proves both authenticity (right sender) and integrity (message not modified).

SHA-256 produces a 32-byte output. SafeChain uses only the first 4 bytes — a 32-bit truncation. This adds only 4 bytes to the frame while providing collision resistance vastly stronger than a simple CRC. This truncation technique is standard in IEEE 802.15.4 (Zigbee), Z-Wave, and other constrained wireless protocols.

**mbedTLS** is the cryptography library built into the ESP32 Arduino core — the same library used for TLS in WiFi and BLE. No additional library is needed. The call is:

```cpp
mbedtls_md_hmac(
    mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
    psk, pskLen,   // 16-byte secret key
    inner, pos,    // 30-byte stable payload
    hmac_out       // 32-byte output — we use only first 4
);
```

### What the HMAC Covers

Not all fields can be authenticated. The Repeater legitimately changes `sender_id`, `hop_count`, and `last_rssi_dbm` at each hop. If the HMAC covered those fields, every relay would invalidate the tag.

The **stable inner payload** (30 bytes) covers only fields that the originating node sets and that nobody downstream should change:

```
protocol_version (1) + frame_type (1) + event_type (1) + flags (1)
+ origin_id (6)
+ event_id (4) + attempt (2) + max_hops (1)
+ event_time_ms (4) + lat_e7 (4) + lon_e7 (4)
+ battery_pct (1)
= 30 bytes
```

The excluded fields (`sender_id`, `hop_count`, `last_rssi_dbm`) are still protected by the CRC16 for wire integrity — they just aren't authenticated.

### Finalization Order

Order matters: the `auth_tag` must be computed and stored in the frame **before** the CRC16 is computed, because the CRC must cover the auth_tag:

```cpp
void Protocol::finalizeFrame(SafeChainFrameV1& frame,
                              const uint8_t* psk, size_t pskLen) {
    // Step 1: compute auth_tag over stable inner payload
    frame.auth_tag = computeHMAC32(psk, pskLen, frame);
    frame.flags   |= FLAG_AUTH_PRESENT;

    // Step 2: compute CRC16 over the entire frame except crc16 itself
    frame.crc16 = 0;
    frame.crc16 = calcCRC16((uint8_t*)&frame,
                             sizeof(SafeChainFrameV1) - sizeof(frame.crc16));
}
```

### Validation Order

Validation reverses this: check CRC first (cheap, fast) to reject corrupt frames without wasting time on HMAC computation. Then check HMAC:

```cpp
bool Protocol::validateFrame(const SafeChainFrameV1& frame,
                              const uint8_t* psk, size_t pskLen) {
    // Step 1: CRC check (wire integrity)
    SafeChainFrameV1 tmp = frame;
    uint16_t rx_crc = tmp.crc16;
    tmp.crc16 = 0;
    uint16_t ex_crc = calcCRC16((uint8_t*)&tmp, sizeof(tmp) - sizeof(tmp.crc16));
    if (rx_crc != ex_crc) return false;

    // Step 2: HMAC check (authentication) — only if auth flag is set
    if (frame.flags & FLAG_AUTH_PRESENT) {
        uint32_t expected = computeHMAC32(psk, pskLen, frame);
        if (frame.auth_tag != expected) return false;
    }
    return true;
}
```

### PSK Storage and Provisioning

The PSK is stored in NVS under key `"psk"` in namespace `"safechain"`. The default development key is `"SafeChain_Dev01"` (15 bytes, padded to 16). The `hasPSK()` method returns `false` when using the default, letting the Serial monitor warn the operator.

Provisioning is done via Serial command:
```
setpsk 0102030405060708090a0b0c0d0e0f10
```

This takes 32 hex characters (16 bytes), stores them in NVS, and calls `reloadPSK()` to update the in-RAM cache immediately. The Repeater's `router.reloadPSK()` also updates the router's PSK copy so `prepareRelay()` uses the new key for CRC16 recomputation.

### Relay Without Recomputing auth_tag

The Repeater calls `prepareRelay()` which only updates `sender_id` and `hop_count`, then recomputes CRC16:

```cpp
bool Protocol::prepareRelay(SafeChainFrameV1& frame, const char* newSenderId,
                             const uint8_t* psk, size_t pskLen) {
    if (frame.hop_count >= frame.max_hops) return false;
    frame.hop_count++;
    strncpy(frame.sender_id, newSenderId, DEVICE_ID_LEN - 1);
    // CRC16 only — auth_tag unchanged (stable inner payload intact)
    frame.crc16 = 0;
    frame.crc16 = calcCRC16((uint8_t*)&frame, sizeof(frame) - sizeof(frame.crc16));
    return true;
}
```

The `auth_tag` stays exactly as the originating node set it. When the Gateway receives the relayed frame, it recomputes the HMAC over the same 30-byte inner payload and gets the same result — because none of those 30 bytes changed during relay.

---

## M4 — Legacy Packet Path Removal

Before M4, all three devices contained dual receive paths: one for `SafeChainFrameV1` (the new format) and one for `SafeChainPacket` (the original format). This dual-path had three costs:

1. **Binary size**: ~3–5 KB per device for unused code
2. **Complexity**: every frame-handling function had to consider two formats
3. **Security gap**: legacy packets had no HMAC authentication

M4 removes `SafeChainPacket`, `PacketBuilder`, `packet.h`, `packet.cpp`, and the `Router` class from the Node (which handled legacy relay). The Node's `sendTestPacket()` was upgraded to send `SafeChainFrameV1` with `event_type = EVENT_TEST`, which is authenticated and relayed correctly by the M4 Repeater.

The loop RX path simplified from:
```cpp
if (size == sizeof(SafeChainFrameV1)) { ... }
else if (size == sizeof(SafeChainPacket)) { ... }
else { unknown size }
```
to:
```cpp
if (size == sizeof(SafeChainFrameV1)) { ... }
else { unknown size }
```

---

## M5 — Heartbeat Frames

### Purpose

Emergency events prove a node is alive — but only when something goes wrong. Between emergencies, the gateway has no way to know if a node's battery died, it went out of range, or it was physically destroyed. M5 adds periodic liveness beacons.

### Node Side

A `sendHeartbeat()` function builds a `FRAME_HEARTBEAT` frame and transmits it. Key decisions:

**No ACK requested.** The `FLAG_REQUIRE_ACK` bit is not set. Heartbeats are fire-and-forget. Requiring an ACK would double the airtime cost and create retry loops for a non-critical message. If a heartbeat is lost, the next one arrives in 5 minutes.

**Idle guard.** The heartbeat timer only fires when `emergency.getState() == EM_STATE_IDLE`. If the node is mid-emergency (PENDING_TX, WAITING_ACK, etc.), transmission is already happening and proving liveness. Sending a heartbeat during emergency TX could collide with the emergency frame or ACK.

**event_id = 0.** Heartbeat frames use `event_id = 0` — a reserved value meaning "not an emergency event." The gateway does not journal heartbeats.

```cpp
if (emergency.getState() == EM_STATE_IDLE &&
    (millis() - lastHeartbeatTime) >= heartbeatIntervalMs) {
    lastHeartbeatTime = millis();
    sendHeartbeat();
}
```

### Repeater Side

One line added to `shouldRelayV1()`:

```cpp
if (!(frame.frame_type == sc::FRAME_EVENT  ||
      frame.frame_type == sc::FRAME_ACK    ||
      frame.frame_type == sc::FRAME_HEARTBEAT)) {
    packetsDropped++;
    return false;
}
```

Without this change, heartbeats from nodes behind a repeater would be dropped and the gateway would never see them.

### Gateway Side — NodeRegistry

The `NodeRegistry` class maintains a fixed-size table of up to 20 known nodes:

```cpp
struct NodeRecord {
    bool     used;
    char     origin_id[6];
    unsigned long last_seen_ms;
    uint8_t  last_battery;
    int16_t  last_rssi;
    NodeStatus status;        // UNKNOWN, ONLINE, OFFLINE
    uint32_t frames_received;
    uint32_t heartbeats_received;
};
```

`nodeReg.update()` is called on **every received V1 frame**, not just heartbeats. An emergency event also counts as proof-of-life — a node mid-alert will never be flagged offline.

`checkOffline()` runs every 60 seconds (M0-1 overflow-safe timer). Any node silent for longer than `NODE_OFFLINE_THRESHOLD_MS` (18 minutes = 3 × 5-minute interval + 3-minute margin) transitions to `NODE_OFFLINE` and a warning is printed:

```
>>> NODE OFFLINE: FOB01 — silent for >1080s | last batt=45% RSSI=-87
```

The 3-minute margin accounts for RF propagation variance, GPS acquisition delays, and the fact that the node may be in a sleep cycle when the check runs.

When the node transmits again, it transitions back to `NODE_ONLINE` immediately.

### Serial Output

The gateway produces two machine-readable lines for every heartbeat:

```
[HEARTBEAT] node=FOB01  batt= 87%  RSSI= -72  GPS=valid  loc=14.59951,121.00000
HB_V1,FOB01,14.599512,121.000000,87,-72
```

The `HB_V1,` prefix follows the same CSV convention as `CSV_V1,` for emergency events, making it easy for host software to parse both with the same split-on-comma logic.

The `nodes` command prints the full registry table:
```
FOB01   ONLINE   batt= 87%  RSSI= -72  age=142s  frames=15  hb=3
REP01   ONLINE   batt=N/A   RSSI= -65  age=48s   frames=42  hb=0
```

---

## M6 — Over-the-Air Configuration

### Design Constraint: No Frame Size Change

Adding a new frame type for configuration could require new fields. Instead, M6 repurposes existing `SafeChainFrameV1` fields that have no natural meaning for a config message:

| Field | Normal meaning | FRAME_CONFIG meaning |
|---|---|---|
| `origin_id` | Who created the event | Target node ID or "ALL" |
| `event_type` | FIRE / FLOOD / CRIME | ConfigKey (SF, TXPOWER, HB_INTERVAL, REBOOT) |
| `event_time_ms` | Event timestamp | Config value (SF 7-12, dBm 2-20, ms) |
| `event_id` | Event counter | Config dedup ID (from gateway counter) |

This fits entirely in the existing 45-byte frame with no struct changes. The HMAC and CRC work identically. The Repeater forwards config frames to nodes without any code change (FRAME_CONFIG was already defined in the protocol enum from the original design).

### ConfigKey Enum

```cpp
enum ConfigKey : uint8_t {
    CONFIG_SF           = 0x01,  // Spreading factor (7–12)
    CONFIG_TX_POWER     = 0x02,  // TX power in dBm (2–20)
    CONFIG_HB_INTERVAL  = 0x03,  // Heartbeat interval in ms
    CONFIG_REBOOT       = 0x04   // Soft reboot (value ignored)
};

static const char CONFIG_TARGET_ALL[DEVICE_ID_LEN] = "ALL";
```

### Gateway — Building and Sending Config Frames

The gateway serial command parser:

```
config FOB01 sf 9
config ALL txpower 17
config FOB01 heartbeat 120000
config FOB01 reboot
```

Each command builds a `SafeChainFrameV1` with `frame_type = FRAME_CONFIG`, sets the appropriate fields, calls `finalizeFrame(cfgFrame, gwPSK, sc::PSK_LEN)` to compute HMAC and CRC, then transmits it. A monotonically incrementing `configEventCounter` (starting at 1000, above the range of normal emergency event IDs) provides the `event_id` for dedup.

### Node — Receiving and Applying Config

`handleConfigFrame()` is called from the RX loop when `frame.frame_type == sc::FRAME_CONFIG`:

**Step 1 — Target check.** Is `origin_id` equal to `nodeID` or `"ALL"`? If neither, return without processing — the config was for a different node.

**Step 2 — Dedup check.** `storage.getLastConfigId()` returns the `event_id` of the last applied config. If the incoming `event_id` matches, it is a relay duplicate and is ignored.

**Step 3 — Range validation.** Each config key has valid ranges enforced before application:
- SF: 7–12 (LoRa specification limits)
- TX power: 2–20 dBm (SX1276 hardware limits)
- HB interval: 30,000–1,800,000 ms (30 seconds to 30 minutes)

Out-of-range values are rejected with a serial and BLE message. This prevents the gateway operator from accidentally bricking a node's radio.

**Step 4 — Live application.** Changes are applied to the running radio immediately:
```cpp
case sc::CONFIG_SF:
    storage.setSpreadingFactor(sf);   // NVS persist
    LoRa.setSpreadingFactor(sf);      // Live apply
    break;
```

**Step 5 — NVS persist.** The new value is written to NVS so it survives the next reboot.

**Step 6 — Config ID persist.** `storage.setLastConfigId(frame.event_id)` prevents relay bounces from re-applying the same config.

### New NVS Keys on Node

Three new keys were added to the `safechain` namespace:

| Key | Type | Default | Meaning |
|---|---|---|---|
| `tx_pwr` | uint8 | `LORA_TXPOWER` | TX power in dBm |
| `hb_ms` | uint32 | `HEARTBEAT_INTERVAL_MS` | Heartbeat interval |
| `cfg_id` | uint32 | 0 | Last applied config event_id |

These load at boot in `setup()` — the node always starts with whatever configuration was last pushed to it, not the compile-time defaults.

---

## The Complete State Machine (Node)

The emergency state machine is the central logic of the Node. Every possible situation is handled by explicit state transitions, never by ad-hoc `if` chains scattered through `loop()`.

```
                    trigger()
  EM_STATE_IDLE ──────────────► EM_STATE_PENDING_TX
       ▲                              │
       │                     isActionDue() → transmitNow()
       │                              │
       │                              ▼
       │                    EM_STATE_WAITING_ACK
       │                              │
       │              isActionDue() → scheduleRetry()
       │                              │
       │              retryCount < MAX_RETRIES → EM_STATE_PENDING_TX
       │              retryCount >= MAX_RETRIES → EM_STATE_FAILED
       │                              │
       │  (60s)                       ▼
       │  ◄─────────────────── EM_STATE_FAILED
       │                              │
       │         handleACK()          │
       │  ◄── EM_STATE_CONFIRMED ◄────┘
       │         (1s settle)
       │
  back to IDLE
```

Every state transition is driven by `update()` which is called every `loop()` tick. There are no blocking calls inside the state machine. The entire emergency lifecycle — from button press to confirmed ACK — runs concurrently with LoRa RX, GPS updates, BLE communication, and LED management.

---

## CRC-CCITT Algorithm

Both the frame CRC and the NVS record CRC use CRC-16/CCITT (polynomial 0x1021, initial value 0xFFFF). This is the same algorithm used in X.25, HDLC, and many industrial protocols:

```cpp
uint16_t Protocol::calcCRC16(const uint8_t* data, size_t len) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (uint8_t j = 0; j < 8; j++) {
            crc = (crc & 0x8000)
                ? (uint16_t)((crc << 1) ^ 0x1021)
                : (uint16_t)(crc << 1);
        }
    }
    return crc;
}
```

To compute the CRC, set the `crc16` field to 0 first, compute over all bytes except the last 2 (the CRC field itself), then store the result. To verify, set `crc16` to 0, recompute, and compare.

---

## Where to Make Changes

| What you want to change | File | What to modify |
|---|---|---|
| Emergency retry timing | `config.h` (Node) | `ACK_TIMEOUT_MS`, `RETRY_INTERVAL_MS`, `FAILED_RETRY_INTERVAL_MS` |
| Heartbeat frequency | `config.h` (Node) | `HEARTBEAT_INTERVAL_MS` |
| Offline detection threshold | `config.h` (GW) | `NODE_OFFLINE_THRESHOLD_MS` |
| LoRa frequency or SF | `config.h` (all) | `LORA_FREQ`, `LORA_SF` |
| Add a new emergency type | `safechain_protocol.h` | Add to `EventType` enum |
| Add a new config parameter | `safechain_protocol.h` | Add to `ConfigKey` enum; `handleConfigFrame()` in Node.ino; `config` command in GW.ino |
| Change PSK provisioning | Serial command | `setpsk <32 hex chars>` on each device |
| Change node ID | Serial command | `uid <newid>` on Node, then reboot |
| Add a new button action | `Safechain_Node.ino` | Wire button callback + `sirenPlay()` call |
| Add new NVS-persisted setting | `storage.h`, `storage.cpp` | New getter/setter with unique NVS key |
| Change relay dedup cache size | `config.h` (Repeater) | `DUPLICATE_CACHE_SIZE` |
| Change relay TTL | `config.h` (Repeater) | `RELAY_TTL_MS` |
| Handle a new frame type at gateway | `event_manager.cpp` | Add case in `processFrameV1()` |