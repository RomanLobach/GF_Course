#include <Arduino.h>
#include <esp_partition.h>
#include <esp_spi_flash.h>

// Default first partition table offset for the Arduino-ESP32 framework
// (esp-idf keeps the table itself outside of any esp_partition_t entry,
// so reading it requires a raw flash read at this fixed offset).
#define PARTITION_TABLE_OFFSET 0x8000
#define PARTITION_TABLE_DUMP_SIZE 0xC00 // max size reserved for the table (esp-idf default)

static void printMenu() {
    Serial.println();
    Serial.println(F("=== HW4 crash/debug demo ==="));
    Serial.println(F("1 - crash: integer divide by zero"));
    Serial.println(F("2 - crash: invalid memory access (bad pointer)"));
    Serial.println(F("3 - print partition table (esp_partition API)"));
    Serial.println(F("4 - dump raw partition table bytes (low-level flash read)"));
    Serial.println(F("Send a digit over Serial to run the corresponding action."));
}

// volatile prevents the compiler from folding the division at compile time
// and optimizing the crash away.
static void crashDivideByZero() {
    Serial.println(F("[crash] Triggering integer divide by zero..."));
    Serial.flush();
    volatile int numerator = 10;
    volatile int denominator = 0;
    volatile int result = numerator / denominator;
    Serial.printf("unreachable, result=%d\n", result);
}

static void crashBadPointer() {
    Serial.println(F("[crash] Triggering invalid memory access..."));
    Serial.flush();
    volatile int *badPtr = (volatile int *) 0x1;
    *badPtr = 42;
}

static void printPartitionTable() {
    Serial.println(F("[partitions] Iterating partition table via esp_partition API:"));
    Serial.println(F("name             type subtype address    size"));

    esp_partition_iterator_t it = esp_partition_find(
        ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, NULL);

    while (it != NULL) {
        const esp_partition_t *part = esp_partition_get(it);
        Serial.printf("%-16s 0x%02x 0x%02x    0x%08x %u bytes\n",
                      part->label, part->type, part->subtype,
                      part->address, part->size);
        it = esp_partition_next(it);
    }
    esp_partition_iterator_release(it);
}

static void dumpPartitionTableRaw() {
    Serial.printf("[partitions] Reading %d bytes at flash offset 0x%x (spi_flash_read)\n",
                  PARTITION_TABLE_DUMP_SIZE, PARTITION_TABLE_OFFSET);

    uint8_t buffer[PARTITION_TABLE_DUMP_SIZE];
    esp_err_t err = spi_flash_read(PARTITION_TABLE_OFFSET, buffer, sizeof(buffer));
    if (err != ESP_OK) {
        Serial.printf("spi_flash_read failed: %d\n", err);
        return;
    }

    for (size_t offset = 0; offset < sizeof(buffer); offset += 16) {
        // an all-0xFF row means the rest of the reserved region is unwritten
        bool allFF = true;
        for (size_t i = 0; i < 16 && offset + i < sizeof(buffer); i++) {
            if (buffer[offset + i] != 0xFF) {
                allFF = false;
                break;
            }
        }
        if (allFF) continue;

        Serial.printf("%04x: ", (unsigned) offset);
        for (size_t i = 0; i < 16 && offset + i < sizeof(buffer); i++) {
            Serial.printf("%02x ", buffer[offset + i]);
        }
        Serial.println();
    }
}

void setup() {
    Serial.begin(115200);
    delay(1000);
    printMenu();
}

void loop() {
    if (Serial.available()) {
        char cmd = Serial.read();
        switch (cmd) {
            case '1':
                crashDivideByZero();
                break;
            case '2':
                crashBadPointer();
                break;
            case '3':
                printPartitionTable();
                break;
            case '4':
                dumpPartitionTableRaw();
                break;
            case '\n':
            case '\r':
                break;
            default:
                printMenu();
                break;
        }
    }
}
