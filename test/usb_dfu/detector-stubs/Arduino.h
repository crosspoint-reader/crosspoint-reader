#pragma once
#include <cstdint>
#define IRAM_ATTR
#define INPUT 1
#define FALLING 2
extern uint32_t testRegisters[2];
extern unsigned testWrites, testPinCalls;
extern void (*testInterrupt)();
#define BIT(n) (1u << (n))
#define REG_READ(reg) testRegisters[reg]
#define REG_WRITE(reg, value) (testWrites++, testRegisters[reg] = (value))
inline void pinMode(int, int) { testPinCalls++; }
inline void attachInterrupt(int, void (*cb)(), int) { testInterrupt = cb; }
inline void detachInterrupt(int) { testInterrupt = nullptr; }
