#pragma once
#include <Arduino.h>
#include <vector>
#include "telemetry.h"

// CSV-логи на LittleFS. Новий файл log_NNNN.csv відкривається при кожному
// (пере)підключенні до FC; номер береться з файлової системи, тому переживає перезапуск ESP.
namespace storage {

struct FileInfo {
    String name;
    size_t size;
};

bool begin();
bool openNewLog();
void closeLog();
bool isOpen();
String currentName();
bool writeRow(const Telemetry& t);
void flushNow();

std::vector<FileInfo> list();       // лише log_*.csv, від старіших до новіших
bool remove(const String& name);    // поточний файл видалити не можна
bool isSafeName(const String& name);
size_t freeBytes();
size_t totalBytes();

}  // namespace storage
