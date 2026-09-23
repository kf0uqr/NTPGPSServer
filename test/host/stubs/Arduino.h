#pragma once
// Just enough of the Arduino/ESP32 API to build timekeeper.cpp on a PC.
#include <stdint.h>
#include <stdlib.h>
#include <time.h>
#define IRAM_ATTR
#define INPUT 0
#define RISING 1
#define FALLING 2
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(m) (void)(m)
#define portEXIT_CRITICAL(m) (void)(m)
#define portENTER_CRITICAL_ISR(m) (void)(m)
#define portEXIT_CRITICAL_ISR(m) (void)(m)
inline void pinMode(int, int) {}
inline int digitalPinToInterrupt(int p) { return p; }
void attachInterrupt(int pin, void (*isr)(), int mode);  // provided by the test
