#ifndef COMMANDDATA_CV710_H
#define COMMANDDATA_CV710_H 1

#include <string>

extern std::string cv710_setup_commands;

// Fast-start variant: same capture, I2C status reads dropped and poll loops
// collapsed (see tools/trim_bootstrap.py). Selected by Stream::setFastBootstrap(true).
extern std::string cv710_setup_commands_fast;

#endif
