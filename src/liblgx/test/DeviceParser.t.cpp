// CV-23: parser regression tests for the "skewed lines" fix.
//
// A frame whose word count differs from the locked geometry is corrupt by
// construction - a single word lost/duplicated mid-frame shifts every
// following scanline by 1 word (2 px). The parser must DROP such frames
// instead of padding/presenting them, and must automatically re-anchor the
// parser (CV-23b: parser-only; mid-stream FPGA/FX3 writes were field-tested
// and are destructive) when a persistent corrupt streak builds up while the
// receiver is locked - capped at kMaxAutoResyncs per stuck episode.
// when a persistent corrupt streak builds up while the receiver is locked.

#include "catch_amalgamated.hpp"

#include "lgxdevice.h"

#include <cstdint>
#include <cstring>
#include <functional>
#include <vector>

namespace {

constexpr uint32_t kC0 = 0xC0FFFF00u;
constexpr uint32_t kC1 = 0xC1FFFF00u;
constexpr uint32_t k1080P_WORDS = 1920u * 1080u / 2u;

// Genuine C0 metadata word: b1 = 0x01, b3 = (b0+b1+b2-0x40) & 0xFF (PROTOCOL.md).
uint32_t c0Meta(uint8_t seq, uint8_t flags) {
    uint8_t tb0 = seq, tb1 = 0x01, tb2 = flags;
    uint8_t tb3 = static_cast<uint8_t>(tb0 + tb1 + tb2 - 0x40);
    return (static_cast<uint32_t>(tb3) << 24) | (static_cast<uint32_t>(tb2) << 16) |
           (static_cast<uint32_t>(tb1) << 8) | tb0;
}

// Genuine C1 trailer: tb1 = 0x02, seq must match, b4 = (tb0+tb1+tb2+tb3-0x3F) & 0xFF.
void c1Trailer(uint8_t seq, uint8_t flags, uint32_t &t1, uint32_t &t2) {
    uint8_t tb0 = seq, tb1 = 0x02, tb2 = flags, tb3 = 0x00;
    uint8_t chk = static_cast<uint8_t>(tb0 + tb1 + tb2 + tb3 - 0x3F);
    t1 = (static_cast<uint32_t>(tb3) << 24) | (static_cast<uint32_t>(tb2) << 16) |
         (static_cast<uint32_t>(tb1) << 8) | tb0;
    t2 = chk;
}

// One complete synthetic 1080p frame: C0 + meta + N pixel words + C1 + trailer.
std::vector<uint32_t> makeFrame(uint32_t seq, uint32_t pixelWords) {
    std::vector<uint32_t> f;
    f.reserve(pixelWords + 5);
    f.push_back(kC0);
    f.push_back(c0Meta(static_cast<uint8_t>(seq), 0x04));
    for (uint32_t i = 0; i < pixelWords; i++) {
        // Never equals any marker word (0xC0FFFF00 / 0xC1FFFF00 / 0x58FFFF00).
        f.push_back(0x10108010u | (i & 0x0Fu));
    }
    f.push_back(kC1);
    uint32_t t1, t2;
    c1Trailer(static_cast<uint8_t>(seq), 0x04, t1, t2);
    f.push_back(t1);
    f.push_back(t2);
    return f;
}

class MockStream : public lgx2::Stream {
public:
    lgx2::VideoSignalInfo sig{};
    std::function<void(uint8_t *, uint32_t)> *cb = nullptr;

    bool deviceAvailable(lgx2::DeviceType) override { return true; }
    void streamSetupCommands(lgx2::DeviceType) override {}
    void queueFrameRead(std::function<void(uint8_t *, uint32_t)> *onData) override { cb = onData; }
    void update() override {}
    void shutdownStream() override {}
    lgx2::VideoSignalInfo getVideoSignalInfo() const override { return sig; }

    void call(const std::vector<uint32_t> &frame) {
        REQUIRE(cb != nullptr);
        (*cb)(reinterpret_cast<uint8_t *>(const_cast<uint32_t *>(frame.data())),
              static_cast<uint32_t>(frame.size() * 4));
    }
};

class CountingVideoOutput : public lgx2::VideoOutput {
public:
    int frames = 0;
    void initialiseVideo(lgx2::VideoScale scale) override { (void)scale; }
    void videoFrameAvailable(uint32_t *image) override { (void)image; frames++; }
    void videoFrameAvailable(uint32_t *image, uint32_t w, uint32_t h) override {
        (void)w; (void)h;
        videoFrameAvailable(image);
    }
    void display() override {}
    void shutdownVideo() override {}
};

class NullAudioOut : public lgx2::AudioOutput {
public:
    void initialiseAudio() override {}
    void audioFrameAvailable(uint32_t *a, uint32_t len) override { (void)a; (void)len; }
    void render() override {}
    void shutdownAudio() override {}
};

class RunErrorSink : public lgx2::ErrorSink {
public:
    void catchErrors(const std::function<void()> &run) override { run(); }
};

} // namespace

TEST_CASE("CV-23: exact-size locked frame is presented, corrupt frame is dropped", "[parser]") {
    MockStream stream;
    CountingVideoOutput vo;
    NullAudioOut ao;
    RunErrorSink sink;
    lgx2::Device device{&stream, &vo, &ao, nullptr, &sink};
    device.initialise(lgx2::DeviceType::CV710, lgx2::VideoScale::Full);
    REQUIRE(stream.cb != nullptr);

    // Lock + deliver a clean 1080p frame.
    auto exact = makeFrame(1, k1080P_WORDS);
    stream.call(exact);
    REQUIRE(vo.frames == 1);

    // One word short = the row-stride skew trigger: presenting it would shift
    // every following scanline by 1 word. Must be dropped, not padded+presented.
    auto shortFrame = makeFrame(2, k1080P_WORDS - 1);
    stream.call(shortFrame);
    REQUIRE(vo.frames == 1); // not presented

    // The next clean frame re-anchors and presents again.
    auto exact2 = makeFrame(3, k1080P_WORDS);
    stream.call(exact2);
    REQUIRE(vo.frames == 2);
}

TEST_CASE("CV-23b: persistent corrupt stream auto re-anchors the parser, capped per episode", "[parser]") {
    MockStream stream;
    stream.sig.locked = true;
    stream.sig.activeWidth = 1920;
    stream.sig.activeHeight = 1080;
    CountingVideoOutput vo;
    NullAudioOut ao;
    RunErrorSink sink;
    lgx2::Device device{&stream, &vo, &ao, nullptr, &sink};
    device.initialise(lgx2::DeviceType::CV710, lgx2::VideoScale::Full);

    // Phase A: 60 consecutive off-by-one frames while the receiver is locked
    // => the parser auto re-anchors once (never presents a corrupt frame).
    for (uint32_t s = 1; s <= 60; s++) {
        stream.call(makeFrame(s, k1080P_WORDS - 1));
    }
    REQUIRE(device.autoResyncCount() == 1);
    REQUIRE(vo.frames == 0);

    // Phase B: the streak rebuilds (the re-anchor reset it) - one more attempt.
    for (uint32_t s = 61; s <= 120; s++) {
        stream.call(makeFrame(s, k1080P_WORDS - 1));
    }
    REQUIRE(device.autoResyncCount() == 2);

    // Phase C: still no valid frame - the per-episode cap holds; it must not
    // loop forever (this is the loop the old software replug got stuck in).
    for (uint32_t s = 121; s <= 180; s++) {
        stream.call(makeFrame(s, k1080P_WORDS - 1));
    }
    REQUIRE(device.autoResyncCount() == 2);

    // Phase D: a clean frame presents and ends the stuck episode; a fresh
    // 60-frame corrupt run is allowed to re-anchor again.
    stream.call(makeFrame(181, k1080P_WORDS));
    REQUIRE(vo.frames == 1);
    for (uint32_t s = 182; s <= 241; s++) {
        stream.call(makeFrame(s, k1080P_WORDS - 1));
    }
    REQUIRE(device.autoResyncCount() == 3);
}