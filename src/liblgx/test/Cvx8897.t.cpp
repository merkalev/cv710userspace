// CV-28: CVX8897 frame-rate converter selection/apply tests.
//
// The register tables themselves come from the vendor binaries (see
// tools/cvx8897/); these tests pin the *selection* rules and the apply loop,
// which are the parts this project controls.

#include "catch_amalgamated.hpp"

#include "cvx8897/cvx8897_convert.h"

#include <string>
#include <vector>

using namespace cvx8897;

namespace {

std::string nameOf(const Table *t) {
    return t == nullptr ? std::string("<null>") : std::string(t->name);
}

}  // namespace

TEST_CASE("cvx8897: half-rate selection covers the registered modes", "[cvx8897]") {
    // 1080p60 and 1080p59.94 -> 30p
    CHECK(nameOf(selectHalfRate(1920, 1080, 60.0f, false, false)) ==
          "1920_1080_60P_422_To_1920_1080_30P_422");
    CHECK(nameOf(selectHalfRate(1920, 1080, 59.94f, false, false)) ==
          "1920_1080_60P_422_To_1920_1080_30P_422");

    // 1080p50 -> 25p
    CHECK(nameOf(selectHalfRate(1920, 1080, 50.0f, false, false)) ==
          "1920_1080_50P_422_To_1920_1080_25P_422");

    // 720p60: RGB sources take the dedicated RGB table, YCbCr the 422 table.
    CHECK(nameOf(selectHalfRate(1280, 720, 60.0f, false, false)) ==
          "1280_720_60P_422_To_1280_720_30P_422");
    CHECK(nameOf(selectHalfRate(1280, 720, 60.0f, true, false)) ==
          "1280_720_60P_RGB_To_1280_720_30P_RGB");
    CHECK(nameOf(selectHalfRate(1280, 720, 50.0f, false, false)) ==
          "1280_720_50P_422_To_1280_720_25P_422");

    // 1366x768 is served by the 1360x768 table.
    CHECK(nameOf(selectHalfRate(1366, 768, 60.0f, false, false)) ==
          "1360_768_60P_422_To_1360_768_30P_422");

    // The 4:3 / 16:10 modes only have 60->30 tables.
    CHECK(nameOf(selectHalfRate(800, 600, 60.0f, false, false)) ==
          "800_600_60P_422_To_800_600_30P_422");
    CHECK(nameOf(selectHalfRate(1280, 1024, 60.0f, false, false)) ==
          "1280_1024_60P_422_To_1280_1024_30P_422");
}

TEST_CASE("cvx8897: unsupported and interlaced inputs are left alone", "[cvx8897]") {
    // Interlaced sources have no converter table.
    CHECK(selectHalfRate(1920, 1080, 60.0f, false, true) == nullptr);
    // Unregistered geometry.
    CHECK(selectHalfRate(1600, 900, 60.0f, false, false) == nullptr);
    CHECK(selectHalfRate(3840, 2160, 60.0f, false, false) == nullptr);
}

TEST_CASE("cvx8897: findTable resolves exact names and rejects junk", "[cvx8897]") {
    CHECK(findTable("1920_1080_60P_422_To_1920_1080_30P_422") != nullptr);
    CHECK(findTable("800_600_60P_422_To_800_600_30P_422") != nullptr);
    CHECK(findTable("not_a_mode") == nullptr);
    CHECK(findTable("") == nullptr);
}

TEST_CASE("cvx8897: apply writes every register in order and counts failures", "[cvx8897]") {
    const Table *t = findTable("800_600_60P_422_To_800_600_30P_422");
    REQUIRE(t != nullptr);

    std::vector<std::pair<uint8_t, uint8_t>> seen;
    int failures = apply(*t, [&](uint8_t reg, uint8_t val) {
        seen.emplace_back(reg, val);
        return true;
    });

    CHECK(failures == 0);
    REQUIRE(seen.size() == kRegCount);
    for (std::size_t i = 0; i < kRegCount; ++i) {
        CHECK(seen[i].first == kRegOrder[i]);
        CHECK(seen[i].second == t->values[i]);
    }

    // Failures are counted, not short-circuited.
    int failCount = apply(*t, [](uint8_t, uint8_t) { return false; });
    CHECK(failCount == static_cast<int>(kRegCount));
}
