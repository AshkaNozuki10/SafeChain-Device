#include "packet.h"

uint16_t PacketBuilder::calcCRC16(const uint8_t *data, uint16_t length) {
    uint16_t crc = 0xFFFF;
    for (uint16_t i = 0; i < length; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (uint8_t j = 0; j < 8; j++) {
            if (crc & 0x8000) crc = (crc << 1) ^ 0x1021;
            else crc <<= 1;
        }
    }
    return crc;
}

bool PacketBuilder::validate(const SafeChainPacket &pkt) {
    uint16_t expected = calcCRC16((uint8_t*)&pkt, sizeof(SafeChainPacket) - 2);
    return (pkt.crc == expected);
}

void PacketBuilder::buildACK(SafeChainPacket &pkt, const char* srcID, uint16_t ackSeqNum) {
    memset(&pkt, 0, sizeof(SafeChainPacket));
    strncpy(pkt.srcID, srcID, 5);
    pkt.msgType = MSG_ACK;
    pkt.seqNum = ackSeqNum;
    pkt.hopCount = 0;
    pkt.maxHop = 10;
    pkt.battery = 100;
    pkt.rssi = 0;
    pkt.crc = calcCRC16((uint8_t*)&pkt, sizeof(SafeChainPacket) - 2);
}

void PacketBuilder::print(const SafeChainPacket &pkt) {
    Serial.printf("[PKT] ID:%s Type:%02X Seq:%u Hop:%u/%u Lat:%.6f Lon:%.6f Batt:%u%% RSSI:%d\n",
        pkt.srcID, pkt.msgType, pkt.seqNum, pkt.hopCount, pkt.maxHop,
        pkt.latitude, pkt.longitude, pkt.battery, pkt.rssi);
}