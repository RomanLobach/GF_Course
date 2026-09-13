#include <Arduino.h>

// Частина 1: кнопка (GPIO39) на одному ядрі керує інтервалом блимання LED (GPIO15)
// на іншому ядрі. Нове значення інтервалу передається виключно через Queue.

static const uint8_t BUTTON_PIN = 39;
static const uint8_t LED_PIN = 15;

static const uint32_t BLINK_INTERVALS_MS[] = {250, 500, 1000, 2000};
static const uint8_t BLINK_INTERVALS_COUNT = sizeof(BLINK_INTERVALS_MS) / sizeof(BLINK_INTERVALS_MS[0]);

static const uint32_t DEBOUNCE_MS = 30;
static const uint32_t DOUBLE_CLICK_WINDOW_MS = 300;

static QueueHandle_t intervalQueue;

static void buttonTask(void *pvParameters) {
    bool lastStableState = digitalRead(BUTTON_PIN);
    bool lastRawState = lastStableState;
    uint32_t lastDebounceTime = 0;

    bool pendingClick = false;
    uint32_t pendingClickTime = 0;

    int8_t intervalIndex = 0;

    for (;;) {
        bool rawState = digitalRead(BUTTON_PIN);
        if (rawState != lastRawState) {
            lastDebounceTime = millis();
            lastRawState = rawState;
        }

        if (millis() - lastDebounceTime > DEBOUNCE_MS && rawState != lastStableState) {
            lastStableState = rawState;
            bool pressed = (lastStableState == LOW); // кнопка підтягнута до GND

            if (pressed) {
                uint32_t now = millis();
                if (pendingClick && (now - pendingClickTime) <= DOUBLE_CLICK_WINDOW_MS) {
                    // подвійне натискання: крок назад
                    intervalIndex = (intervalIndex - 1 + BLINK_INTERVALS_COUNT) % BLINK_INTERVALS_COUNT;
                    pendingClick = false;

                    uint32_t interval = BLINK_INTERVALS_MS[intervalIndex];
                    xQueueOverwrite(intervalQueue, &interval);
                    Serial.printf("double click -> interval %lu ms\n", interval);
                } else {
                    pendingClick = true;
                    pendingClickTime = now;
                }
            }
        }

        // якщо вікно подвійного кліку минуло без другого натискання - це одинарний клік
        if (pendingClick && (millis() - pendingClickTime) > DOUBLE_CLICK_WINDOW_MS) {
            intervalIndex = (intervalIndex + 1) % BLINK_INTERVALS_COUNT;
            pendingClick = false;

            uint32_t interval = BLINK_INTERVALS_MS[intervalIndex];
            xQueueOverwrite(intervalQueue, &interval);
            Serial.printf("single click -> interval %lu ms\n", interval);
        }

        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

static void ledTask(void *pvParameters) {
    uint32_t interval = BLINK_INTERVALS_MS[0];
    bool ledState = false;

    for (;;) {
        uint32_t receivedInterval;
        if (xQueuePeek(intervalQueue, &receivedInterval, 0) == pdTRUE) {
            interval = receivedInterval;
        }

        ledState = !ledState;
        digitalWrite(LED_PIN, ledState);
        vTaskDelay(pdMS_TO_TICKS(interval));
    }
}

void setup() {
    Serial.begin(115200);
    pinMode(BUTTON_PIN, INPUT);
    pinMode(LED_PIN, OUTPUT);

    intervalQueue = xQueueCreate(1, sizeof(uint32_t));

    xTaskCreatePinnedToCore(buttonTask, "buttonTask", 4096, nullptr, 1, nullptr, 0);
    xTaskCreatePinnedToCore(ledTask, "ledTask", 2048, nullptr, 1, nullptr, 1);
}

void loop() {
    vTaskDelete(nullptr);
}
