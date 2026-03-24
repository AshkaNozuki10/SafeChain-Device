#include "debug_log.h"

static uint8_t g_logLevel = LOG_LEVEL_DEFAULT;

void DebugLog::init(uint8_t level) {
    g_logLevel = level;
}

void DebugLog::setLevel(uint8_t level) {
    g_logLevel = level;
}

uint8_t DebugLog::getLevel() {
    return g_logLevel;
}

bool DebugLog::quiet() {
    return g_logLevel >= LOG_QUIET;
}

bool DebugLog::info() {
    return g_logLevel >= LOG_INFO;
}

bool DebugLog::debug() {
    return g_logLevel >= LOG_DEBUG;
}