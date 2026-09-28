#include "Protocol.h"

namespace Protocol {

void fillBenchPayload(BenchPacket &pkt, const uint8_t start) {
  for (size_t i = 0; i < sizeof(pkt.payload); i++) {
    pkt.payload[i] = static_cast<uint8_t>(start + i);
  }
}

void encodeServicePacket(const ServicePacket &pkt, uint8_t out[SERVICE_PACKET_SIZE]) {
  out[0] = pkt.type;
  out[1] = pkt.b1;
  out[2] = static_cast<uint8_t>(pkt.value & 0xFF);
  out[3] = static_cast<uint8_t>((pkt.value >> 8) & 0xFF);
}

bool decodeServicePacket(const uint8_t *data, const size_t len, ServicePacket &out) {
  if (len != SERVICE_PACKET_SIZE) return false;
  out.type = data[0];
  out.b1 = data[1];
  out.value = static_cast<uint16_t>(data[2]) | (static_cast<uint16_t>(data[3]) << 8);
  return true;
}

void encodeBenchPacket(const BenchPacket &pkt, uint8_t out[BENCH_PACKET_SIZE]) {
  out[0] = pkt.configIndex;
  out[1] = static_cast<uint8_t>(pkt.seqNum & 0xFF);
  out[2] = static_cast<uint8_t>((pkt.seqNum >> 8) & 0xFF);
  memcpy(out + 3, pkt.payload, sizeof(pkt.payload));
}

bool decodeBenchPacket(const uint8_t *data, const size_t len, BenchPacket &out) {
  if (len != BENCH_PACKET_SIZE) return false;
  out.configIndex = data[0];
  out.seqNum = static_cast<uint16_t>(data[1]) | (static_cast<uint16_t>(data[2]) << 8);
  memcpy(out.payload, data + 3, sizeof(out.payload));
  return true;
}

ServicePacket encodeSlotReport(const SlotReport &r) {
  ServicePacket pkt{};
  pkt.type = static_cast<uint8_t>(MsgType::SlotReport);
  pkt.b1 = static_cast<uint8_t>(r.request);
  const uint8_t lo = static_cast<uint8_t>((r.profile & 0x0F) | ((r.desiredProfile & 0x0F) << 4));
  pkt.value = static_cast<uint16_t>(lo | (static_cast<uint16_t>(r.received) << 8));
  return pkt;
}

SlotReport decodeSlotReport(const ServicePacket &pkt) {
  SlotReport r;
  r.request = static_cast<SlotRequest>(pkt.b1 <= 2 ? pkt.b1 : 0);
  r.profile = pkt.value & 0x0F;
  r.desiredProfile = (pkt.value >> 4) & 0x0F;
  r.received = static_cast<uint8_t>(pkt.value >> 8);
  return r;
}

ServicePacket encodeSlotReply(const SlotReply &r) {
  ServicePacket pkt{};
  pkt.type = static_cast<uint8_t>(MsgType::SlotReply);
  pkt.b1 = static_cast<uint8_t>((static_cast<uint8_t>(r.decision) & 0x03) | (r.lastSkipped ? 0x04 : 0) |
                                (r.nextSkipped ? 0x08 : 0) | ((r.nextProfile & 0x0F) << 4));
  pkt.value = r.msToNext;
  return pkt;
}

SlotReply decodeSlotReply(const ServicePacket &pkt) {
  SlotReply r;
  r.decision = static_cast<SlotDecision>(pkt.b1 & 0x03);
  r.lastSkipped = (pkt.b1 & 0x04) != 0;
  r.nextSkipped = (pkt.b1 & 0x08) != 0;
  r.nextProfile = (pkt.b1 >> 4) & 0x0F;
  r.msToNext = pkt.value;
  return r;
}

} // namespace Protocol
