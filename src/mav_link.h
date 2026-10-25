#pragma once
#include "telemetry.h"

// MAVLink-з'єднання з польотним контролером.
// Окрема FreeRTOS-задача читає UART, розбирає пакети, шле heartbeat і запити потоків,
// відстежує втрату та відновлення зв'язку. Інші задачі бачать лише знімок стану.
namespace mav_link {

void begin();
Telemetry snapshot();   // потокобезпечна копія останніх даних
LinkState state();

}  // namespace mav_link
