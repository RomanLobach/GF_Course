// On-air packet formats.
#pragma once

#include <Arduino.h>

namespace Protocol {

// Device state machine. Shared by SessionState, Display and FlashLog so
// all three agree on one vocabulary. Numeric values are persisted in LogRecord::deviceStatus,
// so they stay compatible with logs written by older firmware: 5 was the old MeasureDone state
// (kept reserved), 6+ were old never-logged states.
enum class DeviceState : uint8_t {
  Idle = 0,
  Searching = 1, // SRCH, sub-step "шукаю"
  SeenYou = 2,   // SRCH, sub-step "бачу тебе"
  Synced = 3,
  Measuring = 4,
  // 5 reserved (old MeasureDone)
  Testing = 6,
};

// Byte 0 of the 4-byte service packet.
enum class MsgType : uint8_t {
  Heartbeat    = 0x01, // b1 = HbState of the sender, value = sender's session id
  MeasStartReq = 0x10, // value = 0
  MeasStartAck = 0x11, // value = ms until the common start
  TestStartReq = 0x12, // b1 = start profile
  TestStartAck = 0x13, // b1 = start profile, value = ms until the common start
  StartNak     = 0x14, // b1 = NakReason, value = 1 (MEAS) / 2 (TEST)
  SlotReport   = 0x20, // Rover -> Base, see encodeSlotReport()
  SlotReply    = 0x21, // Base -> Rover, see encodeSlotReply()
  AbortReq     = 0x30, // value = session id
  AbortAck     = 0x31, // value = session id
};

// State carried in every heartbeat.
enum class HbState : uint8_t { Searching = 1, SeenYou = 2, Synced = 3 };

enum class NakReason : uint8_t { Busy = 1, Budget = 2, LogFull = 3 };

// Operator request piggybacked on SLOT_REPORT (Rover) / applied in SLOT_REPLY (Base).
enum class SlotRequest : uint8_t { None = 0, Stop = 1, Abort = 2 };

// Decision carried in SLOT_REPLY, bits 0-1 of b1.
enum class SlotDecision : uint8_t { Next = 0, Done = 1, Stop = 2, Abort = 3 };

struct ServicePacket {
  uint8_t type;   // MsgType
  uint8_t b1;     // meaning depends on type
  uint16_t value; // little-endian on air, meaning depends on type
};

struct BenchPacket {
  uint8_t configIndex;  // profile 1..6
  uint16_t seqNum;      // 0..N-1 within the burst
  uint8_t payload[61];  // deterministic cyclically-incrementing byte pattern
};

// seqNum of the TEST end-of-burst marker the Base sends instead of its next packet when it
// wants to switch profile / stop.
constexpr uint16_t BURST_END_SEQ = 0xFFFF;

constexpr size_t SERVICE_PACKET_SIZE = 4;
constexpr size_t BENCH_PACKET_SIZE = 64;
// On-air sizes are part of the protocol - both roles must agree on them.
static_assert(SERVICE_PACKET_SIZE == 4, "service packet must be exactly 4 bytes on the air");
static_assert(1 + 2 + sizeof(BenchPacket::payload) == BENCH_PACKET_SIZE,
              "bench packet must be exactly 64 bytes on the air");

// Fills payload[i] = (start + i) & 0xFF - the deterministic pattern.
void fillBenchPayload(BenchPacket &pkt, uint8_t start = 0);

// Encode/decode helpers own the on-air byte layout - never memcpy these structs.
void encodeServicePacket(const ServicePacket &pkt, uint8_t out[SERVICE_PACKET_SIZE]);
bool decodeServicePacket(const uint8_t *data, size_t len, ServicePacket &out);

void encodeBenchPacket(const BenchPacket &pkt, uint8_t out[BENCH_PACKET_SIZE]);
bool decodeBenchPacket(const uint8_t *data, size_t len, BenchPacket &out);

// SLOT_REPORT (Rover -> Base): b1 = SlotRequest; value low byte = current profile (bits 0-3)
// | desired profile (bits 4-7, 0 = no change requested); value high byte = packets received.
struct SlotReport {
  uint8_t profile = 0;
  uint8_t desiredProfile = 0;
  uint8_t received = 0;
  SlotRequest request = SlotRequest::None;
};
ServicePacket encodeSlotReport(const SlotReport &r);
SlotReport decodeSlotReport(const ServicePacket &pkt);

// SLOT_REPLY (Base -> Rover): b1 bits 0-1 = SlotDecision, bit 2 = the burst just finished was
// skipped (not transmitted - budget pause), bit 3 = the next burst will be skipped, bits 4-7 =
// profile of the next burst; value = ms until the next burst starts (airtime-compensated).
struct SlotReply {
  SlotDecision decision = SlotDecision::Next;
  bool lastSkipped = false;
  bool nextSkipped = false;
  uint8_t nextProfile = 0;
  uint16_t msToNext = 0;
};
ServicePacket encodeSlotReply(const SlotReply &r);
SlotReply decodeSlotReply(const ServicePacket &pkt);

} // namespace Protocol
