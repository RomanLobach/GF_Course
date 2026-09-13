#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>
#include <LoRa.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#if !defined(DEVICE_ROLE_TRANSMITTER) && !defined(DEVICE_ROLE_RECEIVER)
#error "Build with -D DEVICE_ROLE_TRANSMITTER or -D DEVICE_ROLE_RECEIVER (see platformio.ini [env:transmitter] / [env:receiver])"
#endif

#if defined(DEVICE_ROLE_TRANSMITTER)
#include <TinyGPSPlus.h>
#endif

// ---------------------------------------------------------------------------
// LilyGo TTGO T3 LoRa32 v1.6.1 (ESP32 + SX1276 + SSD1306 OLED)
// ---------------------------------------------------------------------------

#define BUTTON_PIN 39 // input-only pin, no internal pull -> needs external pull-up

#define LORA_SCK  5
#define LORA_MISO 19
#define LORA_MOSI 27
#define LORA_SS   18
#define LORA_RST  23
#define LORA_DIO0 26
#define LORA_BAND 868E6

// NOTE: this board's chip is ESP32-PICO-D4, whose embedded flash uses
// GPIO16/17 internally. Driving them as GPIO hangs the chip (confirmed:
// TG1WDT_SYS_RESET reboot loop), so the OLED reset pin is intentionally left
// unmanaged (-1) below - its RST is tied to the board's own reset net.
#define OLED_SDA  21
#define OLED_SCL  22
#define OLED_ADDR 0x3C
#define OLED_WIDTH  128
#define OLED_HEIGHT 64

#define GPS_RX_PIN 15 // ESP32 RX <- GPS TX
#define GPS_TX_PIN 14 // ESP32 TX -> GPS RX
#define GPS_BAUD 115200 // SEQURE M10-25Q default UART baud

#define DEBOUNCE_MS 30
#define DOUBLE_CLICK_WINDOW_MS 350
#define GPS_BROADCAST_INTERVAL_MS 1000

Adafruit_SSD1306 display(OLED_WIDTH, OLED_HEIGHT, &Wire, -1);

struct RadioMessage {
    char text[48];
    uint32_t createdAtMs; // timestamp used for latency logging on the Radio Task
};

static QueueHandle_t radioQueue;

// ---------------------------------------------------------------------------
// Shared OLED helpers (used by both roles).
// ---------------------------------------------------------------------------
static void initDisplay() {
    Wire.begin(OLED_SDA, OLED_SCL);
    if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
        Serial.println("[Display] SSD1306 init failed!");
        return;
    }
    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    display.display();
}

static void showDateTime(const char *title, const char *dateStr, const char *timeStr, const char *footer) {
    display.clearDisplay();

    display.setTextSize(1);
    display.setCursor(0, 0);
    display.println(title);
    display.drawLine(0, 10, OLED_WIDTH - 1, 10, SSD1306_WHITE);

    display.setTextSize(2);
    display.setCursor(0, 18);
    display.println(dateStr);
    display.setCursor(0, 40);
    display.println(timeStr);

    display.setTextSize(1);
    display.setCursor(0, 57);
    display.println(footer);

    display.display();
}

#if defined(DEVICE_ROLE_TRANSMITTER)
// =============================================================================
// TRANSMITTER: Button Task + GPS Task + Radio Task, wired through queues.
//
//   Button   -> buttonQueue -> loop() -> radioQueue -> Radio Task -> LoRa
//   GPS      -------------------------> radioQueue -> Radio Task -> LoRa
// =============================================================================

enum ButtonClickType : uint8_t { CLICK_SINGLE = 0, CLICK_DOUBLE = 1 };

struct ButtonEvent {
    ButtonClickType type;
    uint32_t pressTimeMs; // millis() at the moment the click sequence started
};

static QueueHandle_t buttonQueue;

TinyGPSPlus gps;

// ---------------- Button Task ----------------
static void buttonTask(void *pvParameters) {
    pinMode(BUTTON_PIN, INPUT);

    bool lastStable = HIGH; // HIGH = released (button pulls the pin LOW)
    bool waitingSecondClick = false;
    uint32_t firstPressTimeMs = 0;

    for (;;) {
        bool raw = digitalRead(BUTTON_PIN);

        if (raw != lastStable) {
            vTaskDelay(pdMS_TO_TICKS(DEBOUNCE_MS));
            raw = digitalRead(BUTTON_PIN);

            if (raw != lastStable) {
                lastStable = raw;

                if (lastStable == LOW) { // fresh press detected
                    uint32_t now = millis();

                    if (waitingSecondClick && (now - firstPressTimeMs) <= DOUBLE_CLICK_WINDOW_MS) {
                        ButtonEvent evt = {CLICK_DOUBLE, firstPressTimeMs};
                        xQueueSend(buttonQueue, &evt, 0);
                        waitingSecondClick = false;
                    } else {
                        firstPressTimeMs = now;
                        waitingSecondClick = true;
                    }
                }
            }
        }

        if (waitingSecondClick && (millis() - firstPressTimeMs) > DOUBLE_CLICK_WINDOW_MS) {
            ButtonEvent evt = {CLICK_SINGLE, firstPressTimeMs};
            xQueueSend(buttonQueue, &evt, 0);
            waitingSecondClick = false;
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// ---------------- Radio Task ----------------
static void radioTask(void *pvParameters) {
    SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_SS);
    LoRa.setPins(LORA_SS, LORA_RST, LORA_DIO0);

    if (!LoRa.begin(LORA_BAND)) {
        Serial.println("[Radio] LoRa init failed!");
        vTaskDelete(NULL);
        return;
    }
    LoRa.enableCrc(); // reject corrupted packets instead of showing garbage
    Serial.println("[Radio] LoRa initialized (TX), waiting for messages...");

    RadioMessage msg;
    for (;;) {
        if (xQueueReceive(radioQueue, &msg, portMAX_DELAY) == pdTRUE) {
            LoRa.beginPacket();
            LoRa.print(msg.text);
            LoRa.endPacket();

            uint32_t latencyMs = millis() - msg.createdAtMs;
            Serial.printf("[Radio] sent \"%s\" | queue->sent latency: %lu ms\n", msg.text, latencyMs);
        }
    }
}

// ---------------- GPS Task ----------------
// Reads NMEA from the GPS module, refreshes the OLED clock and, once a valid
// date/time is available, periodically queues a "DT <date> <time>" message
// for the Radio Task.
static void gpsTask(void *pvParameters) {
    Serial2.begin(GPS_BAUD, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);

    char dateStr[16] = "--/--/----";
    char timeStr[16] = "--:--:--";
    uint32_t lastBroadcastMs = 0;
    bool everHadFix = false;

    for (;;) {
        while (Serial2.available()) {
            gps.encode(Serial2.read());
        }

        // gps.date/time.isValid() only checks the NMEA checksum, not whether
        // the module actually has a signal - without tracked satellites the
        // reported date/time is leftover/default data, not real GPS time.
        bool hasSignal = gps.satellites.isValid() && gps.satellites.value() > 0;
        if (hasSignal && gps.date.isValid() && gps.time.isValid() && gps.date.year() >= 2020) {
            snprintf(dateStr, sizeof(dateStr), "%02d/%02d/%04d", gps.date.day(), gps.date.month(), gps.date.year());
            snprintf(timeStr, sizeof(timeStr), "%02d:%02d:%02d", gps.time.hour(), gps.time.minute(), gps.time.second());
            everHadFix = true;
        }

        char footer[32];
        snprintf(footer, sizeof(footer), "sats:%d %s", gps.satellites.value(), everHadFix ? "UTC" : "no fix");
        showDateTime("TX - GPS date/time", dateStr, timeStr, footer);

        uint32_t now = millis();
        if (everHadFix && (now - lastBroadcastMs) >= GPS_BROADCAST_INTERVAL_MS) {
            lastBroadcastMs = now;

            RadioMessage msg;
            snprintf(msg.text, sizeof(msg.text), "DT %s %s", dateStr, timeStr);
            msg.createdAtMs = now;
            xQueueSend(radioQueue, &msg, 0);

            Serial.printf("[GPS] date/time queued for TX: %s %s (sats: %d)\n", dateStr, timeStr, gps.satellites.value());
        }

        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial.println("[Main] Role: TRANSMITTER");

    initDisplay();

    buttonQueue = xQueueCreate(4, sizeof(ButtonEvent));
    radioQueue = xQueueCreate(8, sizeof(RadioMessage));

    // Button task is the most latency-sensitive, GPS/Radio can trail behind.
    xTaskCreatePinnedToCore(buttonTask, "ButtonTask", 2048, NULL, 3, NULL, 1);
    xTaskCreatePinnedToCore(radioTask, "RadioTask", 4096, NULL, 2, NULL, 1);
    xTaskCreatePinnedToCore(gpsTask, "GpsTask", 4096, NULL, 1, NULL, 1);

    Serial.println("[Main] Ready. Press the button on GPIO39 (single / double click).");
}

// Main loop: receives the classified click from buttonQueue and decides which
// packet to hand over to the Radio Task via radioQueue.
void loop() {
    ButtonEvent evt;

    if (xQueueReceive(buttonQueue, &evt, pdMS_TO_TICKS(50)) == pdTRUE) {
        RadioMessage msg;
        msg.createdAtMs = evt.pressTimeMs;

        if (evt.type == CLICK_SINGLE) {
            strcpy(msg.text, "BUTTON SINGLE");
            Serial.println("[Main] single click -> BUTTON SINGLE");
        } else {
            strcpy(msg.text, "BUTTON DOUBLE");
            Serial.println("[Main] double click -> BUTTON DOUBLE");
        }

        xQueueSend(radioQueue, &msg, 0);
    }
}

#else
// =============================================================================
// RECEIVER: Radio Rx Task listens for LoRa packets and forwards what to show
// to the Display Task through displayQueue.
//
//   LoRa -> Radio Rx Task -> displayQueue -> Display Task -> OLED
// =============================================================================

struct DisplayMessage {
    char title[32];
    char dateStr[16];
    char timeStr[16];
    char footer[32];
};

static QueueHandle_t displayQueue;

// ---------------- Radio Rx Task ----------------
static void radioRxTask(void *pvParameters) {
    SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_SS);
    LoRa.setPins(LORA_SS, LORA_RST, LORA_DIO0);

    if (!LoRa.begin(LORA_BAND)) {
        Serial.println("[Radio] LoRa init failed!");
        vTaskDelete(NULL);
        return;
    }
    LoRa.enableCrc(); // reject corrupted packets instead of showing garbage
    Serial.println("[Radio] LoRa initialized (RX), listening...");

    char buf[64];
    for (;;) {
        int packetSize = LoRa.parsePacket();

        if (packetSize > 0) {
            int len = LoRa.readBytes(buf, min(packetSize, (int)sizeof(buf) - 1));
            buf[len] = '\0';
            int rssi = LoRa.packetRssi();

            Serial.printf("[Radio] received %d bytes, RSSI %d dBm: \"%s\"\n", len, rssi, buf);

            DisplayMessage disp = {};
            if (strncmp(buf, "DT ", 3) == 0) {
                char d[16] = "";
                char t[16] = "";
                sscanf(buf + 3, "%15s %15s", d, t);

                strncpy(disp.title, "RX - date/time", sizeof(disp.title) - 1);
                strncpy(disp.dateStr, d, sizeof(disp.dateStr) - 1);
                strncpy(disp.timeStr, t, sizeof(disp.timeStr) - 1);
            } else {
                strncpy(disp.title, "RX - button event", sizeof(disp.title) - 1);
                strncpy(disp.dateStr, buf, sizeof(disp.dateStr) - 1);
                disp.timeStr[0] = '\0';
            }
            snprintf(disp.footer, sizeof(disp.footer), "RSSI %d dBm", rssi);

            xQueueSend(displayQueue, &disp, 0);
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

// ---------------- Display Task ----------------
static void displayTask(void *pvParameters) {
    initDisplay();
    showDateTime("RX - waiting...", "--/--/----", "--:--:--", "no data yet");

    DisplayMessage disp;
    for (;;) {
        if (xQueueReceive(displayQueue, &disp, portMAX_DELAY) == pdTRUE) {
            showDateTime(disp.title, disp.dateStr, disp.timeStr, disp.footer);
        }
    }
}

void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial.println("[Main] Role: RECEIVER");

    displayQueue = xQueueCreate(4, sizeof(DisplayMessage));

    xTaskCreatePinnedToCore(radioRxTask, "RadioRxTask", 4096, NULL, 2, NULL, 1);
    xTaskCreatePinnedToCore(displayTask, "DisplayTask", 4096, NULL, 1, NULL, 1);

    Serial.println("[Main] Ready, waiting for LoRa packets...");
}

void loop() {
    vTaskDelay(pdMS_TO_TICKS(1000));
}

#endif
