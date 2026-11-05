#pragma once

#include <cstdint>

namespace SimulatorLifecycle {

enum class WakeReason { None, PowerButton };

void initProcessArgs(char** argv);
#ifdef _WIN32
// Windows has no exec(): see SimulatorLifecycle.cpp. Call first thing in main().
// In the first process this runs the simulator as a child and never returns.
void superviseIfNeeded();
#endif
WakeReason consumeWakeReason();
void setSilentRebootToken(uint32_t magic, uint32_t target, uint32_t payload);
void restoreSilentRebootToken(uint32_t& magic, uint32_t& target, uint32_t& payload);
[[noreturn]] void reboot();
[[noreturn]] void rebootAsPowerWake();

}  // namespace SimulatorLifecycle
