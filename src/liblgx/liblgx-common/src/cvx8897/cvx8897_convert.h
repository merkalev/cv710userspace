#ifndef LGX2USERSPACE_CVX8897_CONVERT_H
#define LGX2USERSPACE_CVX8897_CONVERT_H

#include <cstdint>
#include <functional>
#include <string>

#include "cvx8897_tables.h"

// CV-28: helpers for the CVX8897 hardware frame-rate converter.
//
// The converter sits between the ADV7604 HDMI receiver and the capture path. The
// official application exposes it as the *Video Input Frame Rate*
// [60p]/[30p]/[50p]/[25p] selector, i.e. it halves the delivered rate in
// hardware. The register tables were recovered from the vendor drivers and
// live in cvx8897_tables.h (see tools/cvx8897/).
namespace cvx8897 {

// Look up a table by its exact mode name (e.g.
// "1920_1080_60P_422_To_1920_1080_30P_422"). Returns nullptr if unknown.
const Table *findTable(const std::string &name);

// Pick the half-rate table (60->30 / 50->25) that matches a detected input.
// `rgb` is true for an RGB source (the converter has a dedicated 720p RGB
// table). Returns nullptr for interlaced sources or geometries the converter
// was not registered for; the caller should leave the converter untouched.
const Table *selectHalfRate(uint16_t width, uint16_t height, float fps, bool rgb,
                            bool interlaced);

// Apply a table by invoking write(reg, val) for every entry, in order. The
// initial table commit (register 0x01) is included. Returns the number of
// failed writes (0 == fully applied).
int apply(const Table &table, const std::function<bool(uint8_t, uint8_t)> &write);

}  // namespace cvx8897

#endif  // LGX2USERSPACE_CVX8897_CONVERT_H
