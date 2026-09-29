// On-air time estimates (Semtech AN1200.13 LoRa model + a simple FSK bit count), used to
// schedule burst windows and estimate duty-cycle cost.
//
// These are *estimates* for planning only; the duty-cycle accounting itself (AirtimeBudget)
// uses the TX duration actually measured by RadioManager.
#pragma once

#include <Arduino.h>
#include <cmath>
#include "Config.h"
#include "Protocol.h"

namespace Airtime {

inline double loraMs(const uint8_t sf, const float bwKhz, const size_t payloadBytes, const uint8_t cr = 5) {
  const double bwHz = bwKhz * 1000.0;
  const double tSym = (1u << sf) / bwHz * 1000.0; // ms
  const int de = tSym >= 16.0 ? 1 : 0;           // low data rate optimisation (RadioLib auto)
  const int h = sf == 6 ? 1 : 0;                 // SF6 is implicit-header only
  constexpr int crc = 1;

  const double numerator = 8.0 * payloadBytes - 4.0 * sf + 28 + 16 * crc - 20 * h;
  const double denominator = 4.0 * (sf - 2 * de);
  const double payloadSymbNb = 8 + std::fmax(std::ceil(numerator / denominator) * cr, 0.0);
  return (8 + 4.25) * tSym + payloadSymbNb * tSym;
}

inline double fskMs(const uint32_t bitrate, const size_t payloadBytes) {
  constexpr double kPreambleSyncCrcBits = 16.0 + 32.0 + 16.0; // preamble + sync word + CRC, approximate
  return (payloadBytes * 8.0 + kPreambleSyncCrcBits) / bitrate * 1000.0;
}

inline double packetMs(const Config::BenchConfig &cfg, const size_t payloadBytes) {
  if (cfg.modulation == Config::Modulation::LoRaMod) {
    return loraMs(cfg.sf, cfg.bwKhz, payloadBytes);
  }
  return fskMs(cfg.bitrate, payloadBytes);
}

// ~207 ms at SF10/BW125 - used to compensate "ms until X" fields for the packet's own flight time.
inline uint32_t serviceMs() {
  return static_cast<uint32_t>(std::ceil(
      loraMs(Config::SERVICE_SF, Config::SERVICE_BW_KHZ, Protocol::SERVICE_PACKET_SIZE, Config::SERVICE_CR)));
}

inline uint32_t benchPacketMs(const uint8_t profile1to6) {
  return static_cast<uint32_t>(
      std::ceil(packetMs(Config::BENCH_CONFIGS[profile1to6 - 1], Protocol::BENCH_PACKET_SIZE)));
}

// Spacing between consecutive packet starts in a burst. TEST adds a listen gap after every
// packet and uses a fixed schedule both sides can predict.
inline uint32_t packetPeriodMs(const uint8_t profile1to6, const bool withTestGap) {
  return benchPacketMs(profile1to6) + Config::BURST_GUARD_MS + (withTestGap ? Config::TEST_GAP_MS : 0);
}

// Burst window W_p: lead + N x period + margin. Both sides compute it
// identically, so it needs no on-air negotiation.
inline uint32_t burstWindowMs(const uint8_t profile1to6, const uint8_t packets, const bool withTestGap = false) {
  return Config::BURST_LEAD_MS + packets * packetPeriodMs(profile1to6, withTestGap) + Config::BURST_MARGIN_MS;
}

// Airtime one burst costs the transmitting side (Base), for budget checks.
inline uint32_t burstCostMs(const uint8_t profile1to6, const uint8_t packets) {
  return packets * benchPacketMs(profile1to6);
}

} // namespace Airtime
