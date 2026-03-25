// ============================================================
// Safechain_Repeater.ino  —  M0 + M1 + M3(auth)
// ============================================================
#include <SPI.h>
#include <LoRa.h>
#include <Adafruit_NeoPixel.h>
#include <esp_task_wdt.h>

#include "config.h"
#include "router.h"
#include "storage.h"
#include "ble_terminal.h"
#include "safechain_protocol.h"
#include "debug_log.h"

// === OBJECTS ===
RepeaterRouter    router;
Adafruit_NeoPixel strip(1, PIN_RGB, NEO_GRB + NEO_KHZ800);
Storage           storage;
BLETerminal       ble;

// === STATE ===
RTC_DATA_ATTR int bootCount       = 0;
RTC_DATA_ATTR int loraReinitCount = 0;
uint32_t          loraFaultCount  = 0;
unsigned long     lastStatsTime   = 0;
unsigned long     lastLoraHealthCheck = 0;
const unsigned long STATS_INTERVAL = 60000;

// [M3] Repeater PSK — used for validateFrame on inbound AND prepareRelay
uint8_t repPSK[sc::PSK_LEN];

// === HELPERS ===
void setRGB(int r, int g, int b) { strip.setPixelColor(0, strip.Color(r,g,b)); strip.show(); }
void flashRGB(int r, int g, int b, int t) {
    for (int i=0;i<t;i++) { setRGB(r,g,b); delay(100); setRGB(0,0,0); delay(100); }
}

void loadRepPSK() {
    storage.getPSK(repPSK, sc::PSK_LEN);
    router.reloadPSK(repPSK, sc::PSK_LEN);
    Serial.printf(">>> PSK loaded (%s)\n", storage.hasPSK() ? "provisioned" : "dev-default");
}

void onBLECommand(String cmd) {
    cmd.trim(); cmd.toLowerCase();
    if      (cmd == "stats") router.printStats();
    else if (cmd == "ping")  ble.send("PONG: Repeater alive");
    else if (cmd == "info") {
        ble.send("=== REPEATER INFO ===");
        ble.send("LoRa faults: " + String(loraFaultCount));
        ble.send("LoRa reinits: " + String(loraReinitCount));
        ble.send("LoRa Status: OK");
    }
    else if (cmd == "reboot") {
        ble.send("Rebooting Repeater...");
        delay(500);
        ESP.restart();
    }
    else ble.send("Commands: stats, ping, info, reboot");
}

// ============================================================
// Direct SPI register read (private in arduino-LoRa)
// ============================================================
uint8_t readLoRaRegister(uint8_t reg) {
    digitalWrite(SS_PIN, LOW);
    SPI.transfer(reg & 0x7F);
    uint8_t val = SPI.transfer(0x00);
    digitalWrite(SS_PIN, HIGH);
    return val;
}

// ============================================================
// LoRa reinit [M1-3/4]
// ============================================================
bool loraReinit() {
    Serial.printf(">>> LoRa reinit (fault #%lu)...\n", (unsigned long)(loraFaultCount+1));
    LoRa.end(); delay(100);
    pinMode(RST_PIN, OUTPUT);
    digitalWrite(RST_PIN, HIGH); delay(10);
    digitalWrite(RST_PIN, LOW);  delay(10);
    digitalWrite(RST_PIN, HIGH); delay(20);
    bool ok = false;
    for (int i=1;i<=3;i++) { if (LoRa.begin(LORA_FREQ)) { ok=true; break; } delay(200); }
    if (!ok) { loraFaultCount++; loraReinitCount++; return false; }
    LoRa.setSpreadingFactor(LORA_SF);
    LoRa.setSignalBandwidth(LORA_BW); LoRa.setCodingRate4(LORA_CR);
    LoRa.setPreambleLength(LORA_PREAMBLE); LoRa.enableCrc();
    LoRa.setSyncWord(LORA_SYNCWORD); LoRa.setTxPower(LORA_TXPOWER); LoRa.setGain(0);
    LoRa.receive();
    loraReinitCount++;
    Serial.printf(">>> LoRa reinit OK (#%d)\n", loraReinitCount);
    return true;
}

void setup() {
    Serial.begin(115200);
    delay(2000);
    for (int i=0; i<50 && !Serial; i++) delay(100);

    bootCount++;
    Serial.printf("\n\n=== SAFECHAIN REPEATER v2.2 (Boot #%d) ===\n", bootCount);
    DebugLog::init(LOG_LEVEL_DEFAULT);

    // [M1-2] Reboot reason
    esp_reset_reason_t reason = esp_reset_reason();
    const char* rs = "unknown";
    switch (reason) {
        case ESP_RST_POWERON:  rs="power-on";           break;
        case ESP_RST_BROWNOUT: rs="brownout";           break;
        case ESP_RST_TASK_WDT: rs="TASK-WATCHDOG";      break;
        case ESP_RST_INT_WDT:  rs="INTERRUPT-WATCHDOG"; break;
        case ESP_RST_PANIC:    rs="panic";              break;
        case ESP_RST_SW:       rs="software";           break;
        default: break;
    }
    Serial.printf(">>> Reset reason: %s\n", rs);
    if (reason == ESP_RST_TASK_WDT || reason == ESP_RST_INT_WDT)
        Serial.println("!!! WATCHDOG RESET DETECTED !!!");

    // [M1-1] Watchdog
    esp_task_wdt_config_t wdt_config = {
        .timeout_ms=WDT_TIMEOUT_S*1000, .idle_core_mask=0, .trigger_panic=true
    };
    esp_err_t wdtErr = esp_task_wdt_init(&wdt_config);
    if (wdtErr == ESP_ERR_INVALID_STATE) esp_task_wdt_reconfigure(&wdt_config);
    esp_err_t addErr = esp_task_wdt_add(NULL);
    if (addErr != ESP_OK && addErr != ESP_ERR_INVALID_STATE)
        Serial.printf(">>> WDT add failed: %d\n", (int)addErr);
    esp_task_wdt_reset();

    storage.init();
    String nodeId = storage.getNodeID();
    loadRepPSK(); // [M3]

    Serial.printf(">>> Node ID: %s | PSK: %s\n",
        nodeId.c_str(), storage.hasPSK() ? "provisioned" : "dev-default");
    Serial.printf("REPEATER FW=v2.2 | V1_SIZE=%u\n", (unsigned)sizeof(sc::SafeChainFrameV1));

    ble.init(nodeId.c_str());
    ble.setCommandCallback(onBLECommand);
    esp_task_wdt_reset();

    strip.begin(); strip.setBrightness(50); flashRGB(50, 0, 50, 2);

    SPI.begin(SCK_PIN, MISO_PIN, MOSI_PIN, SS_PIN);
    LoRa.setPins(SS_PIN, RST_PIN, DIO0_PIN);
    if (!LoRa.begin(LORA_FREQ)) {
        Serial.println("LoRa Init Failed!");
        while (1) { flashRGB(255,0,0,1); delay(1000); esp_task_wdt_reset(); }
    }
    LoRa.setSpreadingFactor(LORA_SF);
    LoRa.setSignalBandwidth(LORA_BW); LoRa.setCodingRate4(LORA_CR);
    LoRa.setPreambleLength(LORA_PREAMBLE); LoRa.enableCrc();
    LoRa.setSyncWord(LORA_SYNCWORD); LoRa.setTxPower(LORA_TXPOWER); LoRa.setGain(0);
    randomSeed(esp_random());

   router.init(nodeId.c_str());
    setRGB(0,0,0);
    LoRa.receive();
    lastLoraHealthCheck = millis();
    esp_task_wdt_reset();

    Serial.println("LoRa Ready");
    Serial.println("=================================");
    Serial.println("   REPEATER ACTIVE");
    Serial.println("=================================");
}

void loop() {
    esp_task_wdt_reset();
    router.update();

   int packetSize = LoRa.parsePacket();
    if (packetSize == sizeof(sc::SafeChainFrameV1)) {
        sc::SafeChainFrameV1 rxFrame;
        LoRa.readBytes((uint8_t*)&rxFrame, sizeof(sc::SafeChainFrameV1));

        // 👇 FIX 1: Pass the Repeater's PSK into the validation function!
        if (sc::Protocol::validateFrame(rxFrame, repPSK, sc::PSK_LEN)) {
            rxFrame.last_rssi_dbm = LoRa.packetRssi();
            
            // Format a clean string to send
            char logBuf[128];
            snprintf(logBuf, sizeof(logBuf), "[RX] Origin: %s | Event: %lu | Hops: %u | RSSI: %d dBm",
                rxFrame.origin_id,
                (unsigned long)rxFrame.event_id,
                rxFrame.hop_count,
                rxFrame.last_rssi_dbm);
                
            Serial.println(logBuf);
            
            // 👇 FIX 2: Use ble.send() instead of ble.println()
            if (ble.isConnected()) {
                ble.send(logBuf); 
            }
                
            flashRGB(50, 0, 50, 1);
            if (router.shouldRelayV1(rxFrame)) {
                flashRGB(255, 0, 255, 1);
                Serial.println(">>> Relaying packet...");
                
                if (ble.isConnected()) {
                    ble.send(">>> Relaying packet..."); 
                }
                
                router.queueRelayV1(rxFrame);
            } else {
                Serial.println(">>> Ignored (Duplicate/Max Hops)");
                
                if (ble.isConnected()) {
                    ble.send(">>> Ignored (Duplicate/Max Hops)"); 
                }
            }
        } else {
            Serial.println(">>> CRC/AUTH FAIL - Dropped");
        }

    } else if (packetSize > 0) {
        // QUICKLY clear the ghost traffic buffer without hanging the CPU
        int bytesRead = 0;
        while (LoRa.available() && bytesRead < 256) { 
            LoRa.read(); 
            bytesRead++;
        }
        Serial.printf(">>> WARNING: Dropped Ghost Traffic (size: %d)\n", packetSize);
        // Force radio back to listening mode
        LoRa.receive(); 
    }

    // [M1-3] LoRa health check
    if ((millis() - lastLoraHealthCheck) >= LORA_HEALTH_CHECK_INTERVAL_MS) {
        lastLoraHealthCheck = millis();
        LoRa.receive();
        uint8_t v = readLoRaRegister(0x42);
        if (v != 0x12) { Serial.printf(">>> LoRa health FAIL: 0x%02X\n", v); loraFaultCount++; loraReinit(); }
        else Serial.printf(">>> LoRa health OK | faults=%lu\n", (unsigned long)loraFaultCount);
    }

    if ((millis() - lastStatsTime) > STATS_INTERVAL) {
        lastStatsTime = millis();
        router.printStats();
        Serial.printf("LoRa faults: %lu | reinits: %d\n", (unsigned long)loraFaultCount, loraReinitCount);
    }

    if (Serial.available()) {
        String cmd = Serial.readStringUntil('\n');
        cmd.trim(); cmd.toLowerCase();
        int sp = cmd.indexOf(' ');
        String verb = (sp == -1) ? cmd : cmd.substring(0, sp);
        String arg  = (sp == -1) ? "" : cmd.substring(sp + 1);

        if      (verb == "stats") { router.printStats(); }
        // [M3] setpsk <32 hex chars>
        else if (verb == "setpsk") {
            if (arg.length() == 32) {
                uint8_t newKey[sc::PSK_LEN]; bool ok = true;
                for (int i = 0; i < sc::PSK_LEN; i++) {
                    long v = strtol(arg.substring(i*2, i*2+2).c_str(), nullptr, 16);
                    if (v<0||v>255) { ok=false; break; }
                    newKey[i] = (uint8_t)v;
                }
                if (ok) { storage.setPSK(newKey, sc::PSK_LEN); loadRepPSK(); Serial.println("PSK provisioned."); }
                else      Serial.println("Invalid hex.");
            } else Serial.println("Usage: setpsk <32 hex chars>");
        }
        else if (verb == "showpsk") {
            Serial.printf("PSK: %s | first4=%02X%02X%02X%02X\n",
                storage.hasPSK() ? "provisioned" : "dev-default",
                repPSK[0], repPSK[1], repPSK[2], repPSK[3]);
        }
        else if (verb == "help") {
            Serial.println("stats | setpsk <32hex> | showpsk | help");
        }
    }

    delay(10);
}