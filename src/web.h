#pragma once

// Wi-Fi точка доступу + HTTP-сервер: поточна телеметрія, список CSV, завантаження, видалення.
namespace web {

void begin();
void handle();   // викликати з loop()

}  // namespace web
