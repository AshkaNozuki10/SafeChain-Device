#ifndef PACKET_H
#define PACKET_H

#include <Arduino.h>
#include "config.h"

struct __attribute__((packed)) SafeChainPacket {
    char     srcID[6];      // "FOB01" + null terminator
    uint8_t  msgType;       // MsgType enum
    uint16_t seqNum;        // Sequence counter
    float    latitude;      // Raw float (4 bytes)
    float    longitude;     // Raw float (4 bytes)
    uint8_t  hopCount;      // Current hop
    uint8_t  maxHop;        // Limit (always 3)
    uint8_t  battery;       // 0-100%
    int16_t  rssi;  
    uint16_t crc;           // CRC-16-CCITT
};

class PacketBuilder {
public:
    static uint16_t seqCounter;


    // Build new packet
    static void build(SafeChainPacket &pkt, 
                      const char* srcID,
                      MsgType type,
                      EmergencyType emType,
                      float lat, float lon,
                      uint8_t batt);
    
    // Build ACK response
    static void buildACK(SafeChainPacket &pkt,
                         const char* srcID,
                         uint16_t ackSeqNum);
    
    // CRC calculation
    static uint16_t calcCRC16(const uint8_t *data, uint16_t length);
    
    // Validation
    static bool validate(const SafeChainPacket &pkt);
    
    // Relay preparation
    static bool prepareRelay(SafeChainPacket &pkt);
    
    // Debug print
    static void print(const SafeChainPacket &pkt);
};

#endif