// ============================================================
// Safechain_Gateway.ino  —  M0 + M1 hardened
// ============================================================
#include <SPI.h>
#include <LoRa.h>
#include <esp_task_wdt.h>   // [M1-1]
#include <Preferences.h>  // [M3] for PSK NVS

#include "config.h"
#include "event_manager.h"
#include "safechain_protocol.h"
#include "debug_log.h"

// === OBJECTS ===
EventManager eventMgr;

// [M3] Gateway PSK — provisioned via: setpsk <32 hex chars>
uint8_t gwPSK[sc::PSK_LEN];
Preferences gwPrefs; // lightweight NVS access for PSK (no Storage class on GW)

void loadGWPSK() {
    gwPrefs.begin("safechain", false);
    if (gwPrefs.isKey("psk")) {
        gwPrefs.getBytes("psk", gwPSK, sc::PSK_LEN);
        Serial.println(">>> PSK loaded (provisioned)");
    } else {
        memcpy(gwPSK, sc::PSK_DEFAULT, sc::PSK_LEN);
        Serial.println(">>> PSK loaded (dev-default)");
    }
    gwPrefs.end();
    eventMgr.reloadPSK(gwPSK, sc::PSK_LEN);
}

// === STATE ===
RTC_DATA_ATTR int bootCount       = 0; // [M1-2]
RTC_DATA_ATTR int loraReinitCount = 0; // [M1-4]
uint32_t      loraFaultCount      = 0;
unsigned long lastStatsTime       = 0;
unsigned long lastLoraHealthCheck = 0;
uint32_t      configEventCounter  = 1000; // [M6] config event IDs start at 1000
const unsigned long STATS_INTERVAL = 300000; // 5 min

// === FORWARD DECLARATIONS ===
bool loraReinit();
void checkLoraHealth();

// ==========================================================================
// [FIX] Direct SPI register read — bypasses LoRa.readRegister() which is
// private in sandeepmistry/arduino-LoRa. Reads SX127x registers directly.
// REG_VERSION (0x42) returns 0x12 on SX1276/SX1278.
// ==========================================================================
uint8_t readLoRaRegister(uint8_t reg) {
    digitalWrite(SS_PIN, LOW);
    SPI.transfer(reg & 0x7F); // MSB=0 = read mode for SX127x
    uint8_t val = SPI.transfer(0x00);
    digitalWrite(SS_PIN, HIGH);
    return val;
}

// ==========================================================================
// [M1-3/4] LoRa reinit — 3-attempt + RST_PIN pulse
// ==========================================================================
bool loraReinit() {
    Serial.printf(">>> LoRa reinit (fault #%lu)...\n", (unsigned long)(loraFaultCount + 1));
    LoRa.end();
    delay(100);

    pinMode(RST_PIN, OUTPUT);
    digitalWrite(RST_PIN, HIGH);
    delay(10);
    digitalWrite(RST_PIN, LOW);
    delay(10);
    digitalWrite(RST_PIN, HIGH);
    delay(20);

    bool ok = false;
    for (int attempt = 1; attempt <= 3; attempt++) {
        if (LoRa.begin(LORA_FREQ)) {
            ok = true;
            break;
        }
        delay(200);
    }

    if (!ok) {
        loraFaultCount++;
        loraReinitCount++;
        Serial.printf(">>> LoRa reinit FAILED (faults=%lu reinits=%d)\n",
            (unsigned long)loraFaultCount, loraReinitCount);
        return false;
    }

    LoRa.setSpreadingFactor(LORA_SF);
    LoRa.setSignalBandwidth(LORA_BW);
    LoRa.setCodingRate4(LORA_CR);
    LoRa.setPreambleLength(LORA_PREAMBLE);
    LoRa.enableCrc();
    LoRa.setSyncWord(LORA_SYNCWORD);
    LoRa.setTxPower(LORA_TXPOWER); // [M0-7]
    LoRa.setGain(0);
    LoRa.receive();

    loraReinitCount++;
    Serial.printf(">>> LoRa reinit OK (reinit #%d)\n", loraReinitCount);
    return true;
}

// ==========================================================================
// [M1-3] Health check — wired to loraReinit() via direct SPI register read
// ==========================================================================
void checkLoraHealth() {
    if ((millis() - lastLoraHealthCheck) < LORA_HEALTH_CHECK_INTERVAL_MS) return;
    lastLoraHealthCheck = millis();

    // Stage 1: keep radio armed
    LoRa.receive();

    // Stage 2: direct SPI read — LoRa.readRegister() is private
    uint8_t version = readLoRaRegister(0x42);
    if (version != 0x12) {
        Serial.printf(">>> LoRa health FAIL: REG_VERSION=0x%02X, reinit triggered\n", version);
        loraFaultCount++;
        loraReinit();
    } else {
        Serial.printf(">>> LoRa health OK | faults=%lu reinits=%d\n",
            (unsigned long)loraFaultCount, loraReinitCount);
    }
}

void setup() {
    Serial.begin(115200);
    delay(2000);
    for (int i = 0; i < 50 && !Serial; i++) delay(100);

    bootCount++;
    Serial.printf("\n\n=== SAFECHAIN GATEWAY v2.2 (Boot #%d) ===\n", bootCount);

    DebugLog::init(LOG_LEVEL_DEFAULT);
    LOGI(">>> Log level: %u\n", DebugLog::getLevel());

    // [M1-2] Reboot reason
    esp_reset_reason_t reason = esp_reset_reason();
    const char* rs = "unknown";
    switch (reason) {
        case ESP_RST_POWERON:   rs = "power-on";           break;
        case ESP_RST_BROWNOUT:  rs = "brownout";           break;
        case ESP_RST_TASK_WDT:  rs = "TASK-WATCHDOG";      break;
        case ESP_RST_INT_WDT:   rs = "INTERRUPT-WATCHDOG"; break;
        case ESP_RST_PANIC:     rs = "panic";              break;
        case ESP_RST_SW:        rs = "software";           break;
        default: break;
    }
    Serial.printf(">>> Reset reason: %s\n", rs);
    if (reason == ESP_RST_TASK_WDT || reason == ESP_RST_INT_WDT)
        Serial.println("!!! WATCHDOG RESET DETECTED !!!");

    // [M1-1] Watchdog
    esp_task_wdt_config_t wdt_config = {
        .timeout_ms    = WDT_TIMEOUT_S * 1000,
        .idle_core_mask = 0,
        .trigger_panic  = true
    };
    esp_err_t wdtErr = esp_task_wdt_init(&wdt_config);
    if (wdtErr == ESP_ERR_INVALID_STATE) {
        esp_task_wdt_reconfigure(&wdt_config);
    }
    // [ACTION-3] error-check wdt_add, matching Node
    esp_err_t addErr = esp_task_wdt_add(NULL);
    if (addErr != ESP_OK && addErr != ESP_ERR_INVALID_STATE) {
        Serial.printf(">>> WDT add failed: %d\n", (int)addErr);
    }
    esp_task_wdt_reset();

    Serial.printf("GATEWAY FW=%s | ID=%s | V1_SIZE=%u\n",
        "v1-current", DEFAULT_NODE_ID, (unsigned)sizeof(sc::SafeChainFrameV1));

    Serial.println(">>> Init LoRa...");
    SPI.begin(SCK_PIN, MISO_PIN, MOSI_PIN, SS_PIN);

    // Hard reset radio on boot before first LoRa.begin()
    pinMode(RST_PIN, OUTPUT);
    digitalWrite(RST_PIN, HIGH);
    delay(10);
    digitalWrite(RST_PIN, LOW);
    delay(10);
    digitalWrite(RST_PIN, HIGH);
    delay(20);

    LoRa.setPins(SS_PIN, RST_PIN, DIO0_PIN);

    bool loraOk = false;
    for (int attempt = 1; attempt <= 3; attempt++) {
        Serial.printf(">>> LoRa begin attempt %d\n", attempt);
        if (LoRa.begin(LORA_FREQ)) {
            loraOk = true;
            break;
        }
        delay(300);
    }

    if (!loraOk) {
        Serial.println("LoRa Init Failed!");
        while (1) { delay(1000); esp_task_wdt_reset(); }
    }

    LoRa.setSpreadingFactor(LORA_SF);
    LoRa.setSignalBandwidth(LORA_BW);
    LoRa.setCodingRate4(LORA_CR);
    LoRa.setPreambleLength(LORA_PREAMBLE);
    LoRa.enableCrc();
    LoRa.setSyncWord(LORA_SYNCWORD);
    LoRa.setTxPower(LORA_TXPOWER); // [M0-7] explicit TX power — was missing
    LoRa.setGain(0);
    randomSeed(esp_random());
    LoRa.receive();

    eventMgr.init();
    loadGWPSK(); // [M3]
    lastLoraHealthCheck = millis();
    esp_task_wdt_reset();

    Serial.println("LoRa Ready (SF11, Max Gain)");
    Serial.println("\n=================================");
    Serial.println("   GATEWAY LISTENING");
    Serial.println("=================================");
    Serial.println("Commands: stats | help");
}

void loop() {
    esp_task_wdt_reset(); // [M1-1]

    // [M0-3] Drive the non-blocking ACK scheduler — must be first in loop
    eventMgr.update();

    int packetSize = LoRa.parsePacket();
    if (packetSize) {
        if (packetSize == sizeof(sc::SafeChainFrameV1)) {
            sc::SafeChainFrameV1 rxFrame;
            LoRa.readBytes((uint8_t*)&rxFrame, sizeof(sc::SafeChainFrameV1));
            if (sc::Protocol::validateFrame(rxFrame, gwPSK, sc::PSK_LEN)) { // [M3]
                rxFrame.last_rssi_dbm = LoRa.packetRssi();
                Serial.printf("\n[GATEWAY RX V1] Frame=%s Event=%s EventID=%lu Hop=%u RSSI=%d\n",
                    sc::Protocol::frameTypeName(rxFrame.frame_type),
                    sc::Protocol::eventTypeName(rxFrame.event_type),
                    (unsigned long)rxFrame.event_id,
                    rxFrame.hop_count,
                    rxFrame.last_rssi_dbm);
                eventMgr.processFrameV1(rxFrame);
            } else {
                Serial.println(">>> CRC FAIL - Corrupted V1 frame");
            }

        } else {
            while (LoRa.available()) LoRa.read();
            Serial.printf("\n>>> WARNING: Unknown size: %d bytes\n", packetSize);
        }
    }

    // [M1-3] LoRa health check — wired to loraReinit() via readLoRaRegister()
    checkLoraHealth();

    // Periodic stats
    if ((millis() - lastStatsTime) > STATS_INTERVAL) {
        lastStatsTime = millis();
        eventMgr.printStats();
        Serial.printf("LoRa faults: %lu | reinits: %d | boots: %d\n",
            (unsigned long)loraFaultCount, loraReinitCount, bootCount);
    }

    // Serial commands
    if (Serial.available()) {
        String cmd = Serial.readStringUntil('\n');
        cmd.trim(); cmd.toLowerCase();
        if      (cmd == "stats") { eventMgr.printStats(); }
        else if (cmd.startsWith("commit ")) {
            // [M2] "commit <originId> <eventId>" — host confirms durable storage
            // Example: commit FOB01 42
            String rest = cmd.substring(7);
            int sp = rest.indexOf(' ');
            if (sp > 0) {
                String originId = rest.substring(0, sp);
                uint32_t eventId = (uint32_t)rest.substring(sp + 1).toInt();
                eventMgr.hostCommit(originId.c_str(), eventId);
            } else {
                Serial.println("Usage: commit <originId> <eventId>");
            }
        }
        else if (cmd.startsWith("setpsk ")) {
            // [M3] setpsk <32 hex chars>
            String arg = cmd.substring(7);
            if (arg.length() == 32) {
                uint8_t newKey[sc::PSK_LEN]; bool ok = true;
                for (int i=0;i<sc::PSK_LEN;i++) {
                    long v = strtol(arg.substring(i*2,i*2+2).c_str(),nullptr,16);
                    if (v<0||v>255) { ok=false; break; }
                    newKey[i]=(uint8_t)v;
                }
                if (ok) {
                    gwPrefs.begin("safechain",false);
                    gwPrefs.putBytes("psk", newKey, sc::PSK_LEN);
                    gwPrefs.end();
                    loadGWPSK();
                    Serial.println("PSK provisioned.");
                } else Serial.println("Invalid hex.");
            } else Serial.println("Usage: setpsk <32 hex chars>");
        }
        else if (cmd == "showpsk") {
            Serial.printf("PSK: %s | first4=%02X%02X%02X%02X\n",
                gwPrefs.isKey("psk") ? "provisioned" : "dev-default",
                gwPSK[0], gwPSK[1], gwPSK[2], gwPSK[3]);
        }
        else if (cmd == "nodes") {
            // [M5] Print node registry table
            eventMgr.printNodes();
        }
        else if (cmd.startsWith("config ")) {
            // [M6] config <nodeId|ALL> <sf|txpower|heartbeat|reboot> [value]
            // Examples:
            //   config FOB01 sf 9
            //   config ALL txpower 17
            //   config FOB01 heartbeat 120000
            //   config FOB01 reboot
            String args = cmd.substring(7);
            args.trim();
            int sp1 = args.indexOf(' ');
            if (sp1 < 0) {
                Serial.println("Usage: config <nodeId|ALL> <sf|txpower|heartbeat|reboot> [value]");
            } else {
                String targetId = args.substring(0, sp1);
                String rest     = args.substring(sp1 + 1);
                rest.trim();
                int    sp2    = rest.indexOf(' ');
                String param  = (sp2 < 0) ? rest : rest.substring(0, sp2);
                String valStr = (sp2 < 0) ? "" : rest.substring(sp2 + 1);
                valStr.trim();

                sc::ConfigKey cfgKey   = (sc::ConfigKey)0;
                uint32_t      cfgVal   = 0;
                bool          validCmd = true;

                if      (param == "sf")        { cfgKey = sc::CONFIG_SF;          cfgVal = (uint32_t)valStr.toInt(); }
                else if (param == "txpower")   { cfgKey = sc::CONFIG_TX_POWER;    cfgVal = (uint32_t)valStr.toInt(); }
                else if (param == "heartbeat") { cfgKey = sc::CONFIG_HB_INTERVAL; cfgVal = (uint32_t)valStr.toInt(); }
                else if (param == "reboot")    { cfgKey = sc::CONFIG_REBOOT;      cfgVal = 0; }
                else { Serial.printf("Unknown param: %s\n", param.c_str()); validCmd = false; }

                if (validCmd) {
                    sc::SafeChainFrameV1 cfgFrame;
                    sc::Protocol::initFrame(cfgFrame);
                    cfgFrame.frame_type    = sc::FRAME_CONFIG;
                    cfgFrame.event_type    = (uint8_t)cfgKey;
                    cfgFrame.flags         = 0;
                    memset(cfgFrame.origin_id, 0, sc::DEVICE_ID_LEN);
                    strncpy(cfgFrame.origin_id, targetId.c_str(), sc::DEVICE_ID_LEN - 1);
                    strncpy(cfgFrame.sender_id, DEFAULT_NODE_ID, sc::DEVICE_ID_LEN - 1);
                    cfgFrame.event_id      = configEventCounter++;
                    cfgFrame.attempt       = 0;
                    cfgFrame.hop_count     = 0;
                    cfgFrame.max_hops      = sc::MAX_HOPS_DEFAULT;
                    cfgFrame.event_time_ms = cfgVal;
                    cfgFrame.lat_e7        = 0;
                    cfgFrame.lon_e7        = 0;
                    cfgFrame.battery_pct   = 0;
                    cfgFrame.last_rssi_dbm = 0;
                    sc::Protocol::finalizeFrame(cfgFrame, gwPSK, sc::PSK_LEN);

                    Serial.printf(">>> SENDING CONFIG: target=%s param=%s value=%lu id=%lu\n",
                        targetId.c_str(), param.c_str(),
                        (unsigned long)cfgVal, (unsigned long)cfgFrame.event_id);

                    LoRa.beginPacket();
                    LoRa.write((uint8_t*)&cfgFrame, sizeof(sc::SafeChainFrameV1));
                    LoRa.endPacket();
                    LoRa.receive();
                    Serial.println(">>> CONFIG FRAME SENT");
                }
            }
        }
        else if (cmd == "help")  {
            Serial.println("\n--- COMMANDS ---");
            Serial.println("stats               - Show statistics");
            Serial.println("nodes               - Show node registry");
            Serial.println("commit <id> <n>     - Mark event host-committed");
            Serial.println("config <id> sf <n>  - Set spreading factor (7-12)");
            Serial.println("config <id> txpower <n> - Set TX power (2-20dBm)");
            Serial.println("config <id> heartbeat <ms> - Set HB interval");
            Serial.println("config <id> reboot  - Reboot a node");
            Serial.println("  (use ALL as <id> for broadcast)");
            Serial.println("setpsk <32hex>      - Provision 16-byte PSK");
            Serial.println("showpsk             - Show PSK status");
            Serial.println("help                - This menu");
        }
    }

    delay(10);
}