#pragma once
#include <stdint.h>

namespace ntp_server {

// Start listening on UDP port 123. Call once the network interface is up
// (it is safe to call again after a reconnect).
bool begin();

uint32_t requestsServed();

}  // namespace ntp_server
