// Thin non-blocking wrapper around RadioLib's SX1276 driver.
//
// - The only interrupt in this whole firmware lives here (DIO0, TX/RX-done); its ISR body
//   does nothing but set a volatile flag.
// - A full chip reset (RadioLib begin()) happens only at boot and when switching modem
//   (LoRa <-> FSK); LoRa -> LoRa switches only rewrite frequency/BW/SF/CR.
// - startTransmit() refuses to start while a transmit is still in flight or while paused,
//   so no caller can ever clobber a packet already on air.
// - Every completed transmit's measured duration is recorded in the per-sub-band
//   AirtimeBudget.
#pragma once

#include <Arduino.h>
#include <SPI.h>
#include <RadioLib.h>
#include "Config.h"
#include "Protocol.h"
#include "AirtimeBudget.h"

class RadioManager {
public:
  RadioManager();

  bool begin();
  // SX127x version register (0x12 for the SX1276), for the self-test.
  int16_t chipVersion() { return radio_.getChipVersion(); }

  // Service channel: heartbeat + all control traffic (4-byte packets).
  void useServiceChannel();
  // One of the 6 measurement profiles (1-based, 64-byte bench packets).
  void useBenchConfig(uint8_t profile1to6);

  // Wi-Fi offload: radio asleep, TX/RX refused until resume() (back on the service channel).
  void pause();
  void resume();
  bool isPaused() const { return paused_; }

  bool startTransmit(const uint8_t *data, size_t len);
  // Non-blocking: returns true exactly once, the loop iteration the TX actually finished.
  bool isTransmitDone();
  uint32_t lastTxDurationUs() const { return lastTxDurationUs_; }

  void startReceive();
  // Non-blocking: returns true if a packet was received this call; fills buf/outLen/rssi/snr.
  bool poll(uint8_t *buf, size_t maxLen, size_t &outLen, float &rssi, float &snr);

  bool isTransmitting() const { return opMode_ == OpMode::Transmitting; }

  const AirtimeBudget &budget() const { return budget_; }

private:
  enum class OpMode : uint8_t { Idle, Transmitting, Receiving };
  enum class Channel : uint8_t { None, Service, Bench };

  SPIClass spi_;
  Module module_;
  SX1276 radio_;
  AirtimeBudget budget_;

  OpMode opMode_ = OpMode::Idle;
  Channel channel_ = Channel::None;
  uint8_t benchProfile_ = 0;
  Config::Modulation modulation_ = Config::Modulation::LoRaMod;
  Config::SubBand band_ = Config::SubBand::G3;
  bool paused_ = false;
  uint32_t txStartUs_ = 0;
  uint32_t lastTxDurationUs_ = 0;

  // Fixed on-air length of whatever this channel carries (4 service / 64 bench). SF6 is
  // implicit-header only, so the receiver must be armed with this exact length, and FSK runs
  // in fixed-length mode (the 64-byte bench packet fills the SX1276 FIFO exactly).
  uint8_t expectedRxLen_ = Protocol::SERVICE_PACKET_SIZE;

  void configureLoRa(float freqMhz, float bwKhz, uint8_t sf, uint8_t cr);
  void configureFsk(const Config::BenchConfig &cfg);

  static volatile bool s_operationFlag;
  static void IRAM_ATTR onRadioInterrupt();
};
