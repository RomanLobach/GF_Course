#include "storage.h"
#include "config.h"
#include <LittleFS.h>
#include <algorithm>

namespace {

File g_file;
String g_name;
uint8_t g_rows = 0;

// "log_0007.csv" -> 7, інакше -1
int parseIndex(const String& n) {
    if (!n.startsWith("log_") || !n.endsWith(".csv")) return -1;
    String digits = n.substring(4, n.length() - 4);
    if (digits.length() == 0 || digits.length() > 6) return -1;
    for (size_t i = 0; i < digits.length(); i++) {
        if (!isDigit(digits[i])) return -1;
    }
    return digits.toInt();
}

}  // namespace

namespace storage {

bool begin() {
    return LittleFS.begin(true);  // true: відформатувати, якщо розділ порожній
}

bool isSafeName(const String& name) {
    if (name.length() > 24 || parseIndex(name) < 0) return false;
    for (size_t i = 0; i < name.length(); i++) {
        char c = name[i];
        if (!(isAlphaNumeric(c) || c == '_' || c == '.')) return false;
    }
    return name.indexOf("..") < 0;
}

size_t totalBytes() { return LittleFS.totalBytes(); }
size_t freeBytes()  { return LittleFS.totalBytes() - LittleFS.usedBytes(); }

std::vector<FileInfo> list() {
    std::vector<FileInfo> out;
    File root = LittleFS.open("/");
    if (!root) return out;
    for (File f = root.openNextFile(); f; f = root.openNextFile()) {
        String n = f.name();
        if (n.startsWith("/")) n = n.substring(1);
        if (parseIndex(n) >= 0) out.push_back({n, (size_t)f.size()});
    }
    std::sort(out.begin(), out.end(), [](const FileInfo& a, const FileInfo& b) {
        return parseIndex(a.name) < parseIndex(b.name);
    });
    return out;
}

bool isOpen() { return (bool)g_file; }
String currentName() { return g_name; }

bool openNewLog() {
    closeLog();

    std::vector<FileInfo> files = list();
    int next = 1;
    for (const FileInfo& f : files) next = std::max(next, parseIndex(f.name) + 1);

    // вільне місце: прибираємо найстаріші файли
    while (freeBytes() < MIN_FREE_BYTES && !files.empty()) {
        Serial.printf("[fs] мало місця, видаляю %s\n", files.front().name.c_str());
        LittleFS.remove("/" + files.front().name);
        files.erase(files.begin());
    }

    char name[24];
    snprintf(name, sizeof name, "log_%04d.csv", next);
    g_file = LittleFS.open(String("/") + name, FILE_WRITE);
    if (!g_file) return false;

    g_file.print(csvHeader());
    g_file.flush();
    g_name = name;
    g_rows = 0;
    return true;
}

void closeLog() {
    if (g_file) {
        g_file.flush();
        g_file.close();
    }
    g_name = "";
}

bool writeRow(const Telemetry& t) {
    if (!g_file) return false;
    char buf[256];
    size_t n = telemetryToCsv(t, millis(), buf, sizeof buf);
    if (g_file.write((const uint8_t*)buf, n) != n) return false;
    if (++g_rows % FLUSH_EVERY_ROWS == 0) g_file.flush();
    return true;
}

void flushNow() {
    if (g_file) g_file.flush();
}

bool remove(const String& name) {
    if (!isSafeName(name)) return false;
    if (g_file && name == g_name) return false;
    return LittleFS.remove("/" + name);
}

}  // namespace storage
