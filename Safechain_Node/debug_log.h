#ifndef DEBUG_LOG_H
#define DEBUG_LOG_H

#include <Arduino.h>

enum LogLevel : uint8_t {
    LOG_QUIET = 0,
    LOG_INFO  = 1,
    LOG_DEBUG = 2
};

#ifndef LOG_LEVEL_DEFAULT
#define LOG_LEVEL_DEFAULT LOG_INFO
#endif

class DebugLog {
public:
    static void init(uint8_t level = LOG_LEVEL_DEFAULT);
    static void setLevel(uint8_t level);
    static uint8_t getLevel();

    static bool quiet();
    static bool info();
    static bool debug();
};

#define LOGQ(...) do { if (DebugLog::quiet()) { Serial.printf(__VA_ARGS__); } } while (0)
#define LOGI(...) do { if (DebugLog::info())  { Serial.printf(__VA_ARGS__); } } while (0)
#define LOGD(...) do { if (DebugLog::debug()) { Serial.printf(__VA_ARGS__); } } while (0)

#endif