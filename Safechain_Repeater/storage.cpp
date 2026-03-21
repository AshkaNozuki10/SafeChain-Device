#include "storage.h"

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