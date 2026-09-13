#include <Arduino.h>
#include <RadioLib.h>

// Бонус: LED (GPIO15) блимає рівно кожні 100 мс на одній задачі/ядрі, поки на
// іншому ядрі окрема задача асинхронно передає LoRa-пакети (SF11, 32 байти).
// Передача не блокує виклик - вона стартує через startTransmit() і завершується
// в перериванні DIO0, тому LED-задача ніколи не чекає на радіомодуль.

static const uint8_t LED_PIN = 15;
static const uint32_t LED_INTERVAL_MS = 100;

// Пінаут LoRa (SX1276) для плати ttgo-lora32-v1 задається платформою в
// pins_arduino.h: LORA_CS=18, LORA_IRQ(DIO0)=26, LORA_RST=14.
static const float LORA_FREQ_MHZ = 868.0;
static const uint8_t LORA_SF = 11;
static const uint8_t PACKET_SIZE = 32;

static SX1276 radio = new Module(LORA_CS, LORA_IRQ, LORA_RST, RADIOLIB_NC);

static volatile bool transmittedFlag = false;

static void IRAM_ATTR onTransmitDone() {
    transmittedFlag = true;
}

static void ledTask(void *pvParameters) {
    pinMode(LED_PIN, OUTPUT);
    bool ledState = false;

    for (;;) {
        ledState = !ledState;
        digitalWrite(LED_PIN, ledState);
        vTaskDelay(pdMS_TO_TICKS(LED_INTERVAL_MS));
    }
}

static void loraTask(void *pvParameters) {
    int state = radio.begin(LORA_FREQ_MHZ);
    if (state != RADIOLIB_ERR_NONE) {
        Serial.printf("radio.begin failed, code %d\n", state);
        vTaskDelete(nullptr);
    }

    radio.setSpreadingFactor(LORA_SF);
    radio.setDio0Action(onTransmitDone, RISING);

    uint8_t payload[PACKET_SIZE];
    uint32_t packetCounter = 0;
    bool transmitting = false;

    for (;;) {
        if (!transmitting) {
            memset(payload, 0, sizeof(payload));
            memcpy(payload, &packetCounter, sizeof(packetCounter));
            packetCounter++;

            transmittedFlag = false;
            radio.startTransmit(payload, PACKET_SIZE);
            transmitting = true;
        }

        if (transmittedFlag) {
            transmitting = false;
            Serial.println("LoRa: packet sent");
            vTaskDelay(pdMS_TO_TICKS(1000)); // пауза між пакетами
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void setup() {
    Serial.begin(115200);
    delay(1000);

    xTaskCreatePinnedToCore(ledTask, "ledTask", 2048, nullptr, 1, nullptr, 1);
    xTaskCreatePinnedToCore(loraTask, "loraTask", 4096, nullptr, 1, nullptr, 0);
}

void loop() {
    vTaskDelete(nullptr);
}
