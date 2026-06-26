#pragma once

#ifdef SIMULATOR

namespace SimulatorBenchmark {

void initializeFromEnv();
bool isEnabled();
bool startIfConfigured();
void tick();

}  // namespace SimulatorBenchmark

#endif
