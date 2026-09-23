#pragma once
#include "nmea.h"

// Tiny HTTP status page: "/" (HTML) and "/status.json".
namespace status_page {
void begin(const NmeaParser &nmea);
void handle();
}  // namespace status_page
