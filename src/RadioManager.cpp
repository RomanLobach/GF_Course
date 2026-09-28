#include "RadioManager.h"

volatile bool RadioManager::s_operationFlag = false;

void IRAM_ATTR RadioManager::onRadioInterrupt() {
  // Deliberately trivial - per project convention, ISRs only ever set a flag.
  s_operationFlag = true;
}

RadioManager::RadioManager()
  : spi_(HSPI),
    module_(Config::PIN_LORA_CS, Config::PIN_LORA_DIO0, Config::PIN_LORA_RST, RADIOLIB_NC, spi_),
    radio_(&module_) {}

bool RadioManager::begin() {
  spi_.begin(Config::PIN_LORA_SCK, Config::PIN_LORA_MISO, Config::PIN_LORA_MOSI, Config::PIN_LORA_CS);
  const int state = radio_.begin(Config::SERVICE_FREQ_MHZ, Config::SERVICE_BW_KHZ, Config::SERVICE_SF,
                                 Config::SERVICE_CR, RADIOLIB_SX127X_SYNC_WORD, Config::RADIO_TX_POWER_DBM);
  radio_.setCRC(true);
  radio_.setDio0Action(onRadioInterrupt, RISING);
  modulation_ = Config::Modulation::LoRaMod;
  channel_ = Channel::Service;
  band_ = Config::SubBand::G3;
  expectedRxLen_ = Protocol::SERVICE_PACKET_SIZE;
  startReceive();
  return state == RADIOLIB_ERR_NONE;
}

void RadioManager::configureLoRa(const float freqMhz, const float bwKhz, const uint8_t sf, const uint8_t cr) {
  if (modulation_ != Config::Modulation::LoRaMod) {
    // Modem change needs RadioLib's full LoRa init (includes a chip reset, ~10 ms).
    radio_.begin(freqMhz, bwKhz, sf, cr, RADIOLIB_SX127X_SYNC_WORD, Config::RADIO_TX_POWER_DBM);
    radio_.setCRC(true);
    modulation_ = Config::Modulation::LoRaMod;
    return;
  }
  // LoRa -> LoRa: parameter writes only, no reset. setSpreadingFactor() also switches
  // implicit/explicit header mode for SF6 and re-derives low-data-rate optimisation.
  radio_.standby();
  radio_.setFrequency(freqMhz);
  radio_.setBandwidth(bwKhz);
  radio_.setSpreadingFactor(sf);
  radio_.setCodingRate(cr);
}

void RadioManager::configureFsk(const Config::BenchConfig &cfg) {
  radio_.beginFSK(cfg.freqMhz, cfg.bitrate / 1000.0f, cfg.fdevHz / 1000.0f, cfg.rxBwKhz,
                  Config::RADIO_TX_POWER_DBM);
  // beginFSK() leaves RadioLib in variable-length mode, which prefixes a length byte in the
  // FIFO - a 64-byte bench packet would then overflow the 64-byte SX127x FIFO by one byte.
  radio_.fixedPacketLengthMode(Protocol::BENCH_PACKET_SIZE);
  radio_.setCRC(true);
  modulation_ = Config::Modulation::FSK;
}

void RadioManager::useServiceChannel() {
  opMode_ = OpMode::Idle;
  s_operationFlag = false;
  if (channel_ != Channel::Service) {
    configureLoRa(Config::SERVICE_FREQ_MHZ, Config::SERVICE_BW_KHZ, Config::SERVICE_SF, Config::SERVICE_CR);
    channel_ = Channel::Service;
  }
  band_ = Config::SubBand::G3;
  expectedRxLen_ = Protocol::SERVICE_PACKET_SIZE;
}

void RadioManager::useBenchConfig(const uint8_t profile1to6) {
  opMode_ = OpMode::Idle;
  s_operationFlag = false;
  const Config::BenchConfig &cfg = Config::BENCH_CONFIGS[profile1to6 - 1];
  if (channel_ != Channel::Bench || benchProfile_ != profile1to6) {
    if (cfg.modulation == Config::Modulation::LoRaMod) {
      configureLoRa(cfg.freqMhz, cfg.bwKhz, cfg.sf, /*cr=*/5);
    } else {
      configureFsk(cfg);
    }
    channel_ = Channel::Bench;
    benchProfile_ = profile1to6;
  }
  band_ = cfg.band;
  expectedRxLen_ = Protocol::BENCH_PACKET_SIZE;
}

void RadioManager::pause() {
  opMode_ = OpMode::Idle;
  paused_ = true;
  radio_.sleep();
}

void RadioManager::resume() {
  paused_ = false;
  // Waking from sleep keeps the registers, but force a clean service-channel config anyway.
  channel_ = Channel::None;
  useServiceChannel();
  startReceive();
}

bool RadioManager::startTransmit(const uint8_t *data, const size_t len) {
  if (paused_ || opMode_ == OpMode::Transmitting) return false;
  s_operationFlag = false;
  opMode_ = OpMode::Transmitting;
  txStartUs_ = micros();
  const int state = radio_.startTransmit(const_cast<uint8_t *>(data), len);
  if (state != RADIOLIB_ERR_NONE) {
    opMode_ = OpMode::Idle;
    return false;
  }
  return true;
}

bool RadioManager::isTransmitDone() {
  if (opMode_ != OpMode::Transmitting) return false;
  if (!s_operationFlag) return false;
  s_operationFlag = false;
  lastTxDurationUs_ = micros() - txStartUs_;
  budget_.record(band_, (lastTxDurationUs_ + 999) / 1000);
  radio_.finishTransmit();
  opMode_ = OpMode::Idle;
  return true;
}

void RadioManager::startReceive() {
  if (paused_) return;
  s_operationFlag = false;
  opMode_ = OpMode::Receiving;
  // The explicit length matters at SF6 (implicit header): RadioLib's zero-arg overload would
  // arm the modem for 0-byte packets and silently drop everything.
  radio_.startReceive(expectedRxLen_);
}

bool RadioManager::poll(uint8_t *buf, const size_t maxLen, size_t &outLen, float &rssi, float &snr) {
  if (opMode_ != OpMode::Receiving) return false;
  if (!s_operationFlag) return false;
  s_operationFlag = false;

  const size_t len = radio_.getPacketLength();
  bool ok = false;
  if (len > 0 && len <= maxLen) {
    if (radio_.readData(buf, len) == RADIOLIB_ERR_NONE) {
      outLen = len;
      rssi = radio_.getRSSI();
      snr = radio_.getSNR();
      ok = true;
    }
  }
  // Re-arm through our own wrapper so the SF6 payload length is supplied every time.
  startReceive();
  return ok;
}
