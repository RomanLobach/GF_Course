#include <Arduino.h>
#include <esp_task_wdt.h>

// Частина 2: навмисний deadlock. Дві задачі захоплюють два mutex-и у зворотному
// порядку (circular wait). Обидві зареєстровані в Task Watchdog Timer, тому коли
// вони назавжди зависають у xSemaphoreTake(), watchdog перестає отримувати
// esp_task_wdt_reset() і зрештою спрацьовує паніка/перезавантаження.

static const uint8_t WDT_TIMEOUT_S = 5;

static SemaphoreHandle_t mutexA;
static SemaphoreHandle_t mutexB;

static void taskA(void *pvParameters) {
    esp_task_wdt_add(nullptr);

    xSemaphoreTake(mutexA, portMAX_DELAY);
    Serial.println("taskA: locked mutexA, waiting for mutexB...");
    vTaskDelay(pdMS_TO_TICKS(100)); // дає час taskB захопити mutexB

    xSemaphoreTake(mutexB, portMAX_DELAY); // назавжди блокується: mutexB тримає taskB
    Serial.println("taskA: locked mutexB (недосяжно)");

    xSemaphoreGive(mutexB);
    xSemaphoreGive(mutexA);
    vTaskDelete(nullptr);
}

static void taskB(void *pvParameters) {
    esp_task_wdt_add(nullptr);

    xSemaphoreTake(mutexB, portMAX_DELAY);
    Serial.println("taskB: locked mutexB, waiting for mutexA...");
    vTaskDelay(pdMS_TO_TICKS(100)); // дає час taskA захопити mutexA

    xSemaphoreTake(mutexA, portMAX_DELAY); // назавжди блокується: mutexA тримає taskA
    Serial.println("taskB: locked mutexA (недосяжно)");

    xSemaphoreGive(mutexA);
    xSemaphoreGive(mutexB);
    vTaskDelete(nullptr);
}

void setup() {
    Serial.begin(115200);
    delay(1000);

    esp_task_wdt_init(WDT_TIMEOUT_S, true);

    mutexA = xSemaphoreCreateMutex();
    mutexB = xSemaphoreCreateMutex();

    xTaskCreatePinnedToCore(taskA, "taskA", 4096, nullptr, 1, nullptr, 0);
    xTaskCreatePinnedToCore(taskB, "taskB", 4096, nullptr, 1, nullptr, 1);
}

void loop() {
    vTaskDelete(nullptr);
}
