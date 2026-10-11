#include "cvx8897_convert.h"

#include <cstring>

namespace cvx8897 {

namespace {

const Table *tableFor(Mode mode) {
    for (std::size_t i = 0; i < kTableCount; ++i) {
        if (kTables[i].mode == mode) return &kTables[i];
    }
    return nullptr;
}

}  // namespace

const Table *findTable(const std::string &name) {
    for (std::size_t i = 0; i < kTableCount; ++i) {
        if (name == kTables[i].name) return &kTables[i];
    }
    return nullptr;
}

const Table *selectHalfRate(uint16_t width, uint16_t height, float fps, bool rgb,
                            bool interlaced) {
    // The vendor tables are progressive-only.
    if (interlaced) return nullptr;

    // 50-family sources (50p, 25p) map to the *25 tables; everything else
    // (60p / 59.94p, 30p, 24p) maps to the *30 tables.
    const bool fifty = fps >= 45.0f && fps < 55.0f;

    if (width == 1920 && height == 1080) {
        return tableFor(fifty ? Mode::M_1920_1080_50P_422_To_1920_1080_25P_422
                              : Mode::M_1920_1080_60P_422_To_1920_1080_30P_422);
    }

    if (width == 1280 && height == 720) {
        if (fifty) return tableFor(Mode::M_1280_720_50P_422_To_1280_720_25P_422);
        return tableFor(rgb ? Mode::M_1280_720_60P_RGB_To_1280_720_30P_RGB
                            : Mode::M_1280_720_60P_422_To_1280_720_30P_422);
    }

    // 1366x768 is a common panel mode; the vendor table is registered as
    // 1360x768 with the same (halved) timings.
    if ((width == 1360 || width == 1366) && height == 768) {
        return tableFor(Mode::M_1360_768_60P_422_To_1360_768_30P_422);
    }

    // The remaining registered modes only have 60->30 tables.
    struct { uint16_t w, h; Mode mode; } static const kOthers[] = {
        {1680, 1050, Mode::M_1680_1050_60P_422_To_1680_1050_30P_422},
        {1440, 900,  Mode::M_1440_900_60P_422_To_1440_900_30P_422},
        {1280, 1024, Mode::M_1280_1024_60P_422_To_1280_1024_30P_422},
        {1280, 800,  Mode::M_1280_800_60P_422_To_1280_800_30P_422},
        {1280, 768,  Mode::M_1280_768_60P_422_To_1280_768_30P_422},
        {1024, 768,  Mode::M_1024_768_60P_422_To_1024_768_30P_422},
        {800, 600,   Mode::M_800_600_60P_422_To_800_600_30P_422},
    };
    for (const auto &e : kOthers) {
        if (e.w == width && e.h == height) return tableFor(e.mode);
    }

    return nullptr;
}

int apply(const Table &table, const std::function<bool(uint8_t, uint8_t)> &write) {
    int failures = 0;
    for (std::size_t i = 0; i < kRegCount; ++i) {
        if (!write(kRegOrder[i], table.values[i])) ++failures;
    }
    return failures;
}

}  // namespace cvx8897
