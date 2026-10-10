#include "UsbStream.h"

#include <libusb-1.0/libusb.h>
#include <exception>
#include <system_error>
#include <sstream>
#include <algorithm>
#include <cmath>
#include <cstring>

#include "bootstrap/commanddata_cv710.h"

namespace {
    // Compare the fields that drive mode detection / HUD / audio so a flaky I2C
    // read cannot be mistaken for a real signal change.
    bool sameSignalInfo(const lgx2::VideoSignalInfo &a, const lgx2::VideoSignalInfo &b) {
        return a.valid == b.valid && a.locked == b.locked && a.interlaced == b.interlaced &&
               a.activeWidth == b.activeWidth && a.activeHeight == b.activeHeight &&
               a.vidStd == b.vidStd && a.audioSampleRate == b.audioSampleRate &&
               a.audioLocked == b.audioLocked && a.aviColorspace == b.aviColorspace &&
               std::fabs(a.measuredFps - b.measuredFps) < 0.5f;
    }
}

static void usbTransferComplete(struct libusb_transfer *transfer) {
    auto *stream = static_cast<libusb::UsbStream *>(transfer->user_data);
    stream->decrementActiveTransfers();

    if (stream->isShuttingDown()) {
        return;
    }

    if (transfer->status == LIBUSB_TRANSFER_COMPLETED) {
        stream->onFrameData(transfer);
    } else if (transfer->status == LIBUSB_TRANSFER_NO_DEVICE) {
        fprintf(stderr, "USB capture device was disconnected\n");
        stream->signalError("USB capture device was disconnected");
        return;
    } else if (transfer->status == LIBUSB_TRANSFER_CANCELLED) {
        return;
    } else if (transfer->status == LIBUSB_TRANSFER_STALL) {
        // CV-12: never run synchronous clear_halt() on the libusb event thread.
        // Park the transfer; update() recovers the endpoint on the main thread
        // and resubmits only once recovery has succeeded.
        fprintf(stderr, "[USB] Endpoint stalled; deferring halt recovery to the main loop\n");
        stream->onTransferStalled(transfer);
        return;
    } else {
        // Transient error during capture (e.g. HDMI sync loss, mode change, timeout, overflow)
        int errorCount = stream->noteTransferError();
        if (errorCount <= 10 || (errorCount % 100) == 0) {
            fprintf(stderr, "[USB] Transfer non-fatal warning: status %d (count: %d)\n", transfer->status, errorCount);
        }
    }

    stream->submitTransfer(transfer);
}


namespace libusb {
    static const int LGX_DATA_FRAME_LEN = 0x1FC000;
    static constexpr int PIPELINE_DEPTH = 7;

    static const char *usbSpeedName(int speed) {
        switch (speed) {
            case LIBUSB_SPEED_LOW:      return "low-speed";
            case LIBUSB_SPEED_FULL:     return "full-speed";
            case LIBUSB_SPEED_HIGH:     return "high-speed";
            case LIBUSB_SPEED_SUPER:    return "SuperSpeed";
            case LIBUSB_SPEED_SUPER_PLUS: return "SuperSpeed+";
            default:                    return "unknown-speed";
        }
    }

    UsbStream::UsbStream() : _dev{nullptr}, _onFrameDataCallback{} {
        if (libusb_init(nullptr) != 0) {
            // CV-06: a failed libusb_init must not be silently ignored; the
            // device simply stays unavailable and deviceAvailable() reports it.
            fprintf(stderr, "libusb_init failed - no CV710 device will be available\n");
        } else {
            _libusbInited = true;

            libusb_device **list = nullptr;
            ssize_t count = libusb_get_device_list(nullptr, &list);

            for (ssize_t idx = 0; idx < count; ++idx) {
                libusb_device *device = list[idx];
                libusb_device_descriptor desc{};

                libusb_get_device_descriptor(device, &desc);
                if (desc.idVendor == 0x07ca && desc.idProduct == 0x0710) {
                    // CV-11: bcdUSB is the USB version the device advertises, not
                    // the speed actually negotiated on this port/cable. Require a
                    // genuine SuperSpeed link, otherwise 1080p60 raw throughput is
                    // impossible regardless of what the descriptor claims.
                    int speed = libusb_get_device_speed(device);
                    if (speed >= LIBUSB_SPEED_SUPER) {
                        printf("AVerMedia CV710 (ExtremeCap U3) detected on %s link\n", usbSpeedName(speed));
                        _availableDevices.push_back(lgx2::DeviceType::CV710);
                    } else {
                        fprintf(stderr,
                                "CV710 detected but negotiated a %s link (bcdUSB %04x). "
                                "1080p60 requires USB 3.0 SuperSpeed: use a USB3 port and cable.\n",
                                usbSpeedName(speed), desc.bcdUSB);
                    }
                }
            }

            libusb_free_device_list(list, (int) count);
        }
        _frameBuffer = new uint8_t[LGX_DATA_FRAME_LEN * PIPELINE_DEPTH];

        if (const char *rec = getenv("LGX_RECORD")) {
            const char *mb = getenv("LGX_RECORD_MB");
            _recordPath = rec;
            _recordCap = (size_t)(mb ? atoi(mb) : 256) << 20;
            _record.reserve(_recordCap);
            printf("Recording raw EP83 stream to %s (%zu MB)\n", rec, _recordCap >> 20);
        }
    }

    UsbStream::~UsbStream() {
        // CV-06: shutdownStream() is idempotent and also covers the case where
        // setup failed before the read thread existed - without this, a setup
        // exception would leak the opened device handle and claimed interface.
        shutdownStream();
        delete[] _frameBuffer;
        _frameBuffer = nullptr;
        if (_libusbInited) {
            libusb_exit(nullptr);
            _libusbInited = false;
        }
    }

    void UsbStream::closeDevice() {
        if (_dev == nullptr) return;
        if (_interfaceClaimed) {
            libusb_release_interface(_dev, 0);
            _interfaceClaimed = false;
        }
        libusb_close(_dev);
        _dev = nullptr;
    }

    bool UsbStream::deviceAvailable(lgx2::DeviceType deviceType) {
        return std::find(_availableDevices.begin(), _availableDevices.end(), deviceType) != _availableDevices.end();
    }

    void UsbStream::streamSetupCommands(lgx2::DeviceType) {
        _dev = libusb_open_device_with_vid_pid(nullptr, 0x07ca, 0x0710);
        if (_dev == nullptr) {
            throw std::runtime_error(
                    "Failed to open CV710 - is it connected? Run lsusb to check and ensure you have installed the udev rules (and restarted udev if necessary!)");
        }
        std::string targetCommands =
                _fastBootstrap ? cv710_setup_commands_fast : cv710_setup_commands;

        // CV-06: every failure below must release the claimed interface and the
        // opened handle; previously an exception leaked both until process exit.
        try {
            {
                int cur = 0;
                if (libusb_get_configuration(_dev, &cur) == LIBUSB_SUCCESS && cur == 1) {
                    printf("Configuration already 1, skipping set_configuration\n");
                } else if (libusb_set_configuration(_dev, 1) != LIBUSB_SUCCESS) {
                    throw std::runtime_error("Failed to set configuration\n");
                }
            }
    
            if (libusb_claim_interface(_dev, 0) != LIBUSB_SUCCESS) {
                throw std::runtime_error(
                        "Could not claim interface for the device - is something else using the device?\n");
            }
            _interfaceClaimed = true;
    
            // Always run the bootstrap. It re-uploads the 256-byte EDID and
            // drives the receiver's hot-plug (HPD) handshake; skipping it on a
            // "warm" start left the HDMI source with a stale/limited mode list
            // (and, after reconnects, the audio clock/config could drift) until
            // the cable was physically replugged. The elapsed time is logged so
            // the cost stays visible.
            //
            // CV-15/fast-start: --fast-start replays the same write sequence
            // minus I2C status-read round-trips and collapsed poll loops, so the
            // end state is identical but ~8x faster. The full trace remains the
            // fallback via --full-bootstrap.
            {
                auto tBoot0 = std::chrono::steady_clock::now();
                if (_fastBootstrap) {
                    printf("Bootstrapping device (fast EDID/HPD initialisation, %zu commands)...\n",
                           std::count(targetCommands.begin(), targetCommands.end(), ' ') + 1);
                } else {
                    printf("Bootstrapping device (full EDID/HPD initialisation)...\n");
                }
                fflush(stdout);
                setFpgaIdle();
                sendResetStreamDma();
                // CV-15: no proactive libusb_clear_halt() here. This device takes
                // ~5 s to answer a CLEAR_FEATURE(ENDPOINT_HALT) control request
                // (measured: two calls cost 10.4 s of an 11 s bootstrap), and it
                // is pure overhead when the endpoint is not halted - the normal
                // case. A genuine halt surfaces as LIBUSB_TRANSFER_STALL during
                // streaming and is recovered on the main thread by
                // serviceStalledTransfers() (CV-12).

                int actualLength;
                uint8_t transferBuffer[512]{0};

                auto commands = std::istringstream{targetCommands};
                std::string command;
                int cmdIdx = 0;
                while (commands >> command) {
                    cmdIdx++;
                    if (command[0] == '>') {
                        int commandLength = (int) (command.length() - 1) / 2;
                        for (int i = 0; i < commandLength; i++) {
                            transferBuffer[i] = std::stoi(command.substr(1 + i * 2, 2), nullptr, 16);
                        }
                        int brc = libusb_bulk_transfer(_dev, LIBUSB_ENDPOINT_OUT | 0x01, transferBuffer, static_cast<int>(commandLength),
                                             &actualLength, 2000);
                        if (brc != 0 || actualLength != commandLength) {
                            printf("bootstrap WRITE fail at cmd %d (%s): rc=%d actual=%d\n", cmdIdx, command.c_str(), brc, actualLength); fflush(stdout);
                            throw std::runtime_error("bootstrap write timeout/under-run (see cmd index above)");
                        }
                    } else {
                        int bytesToRead = std::stoi(command.substr(1));
                        int rrc = libusb_bulk_transfer(_dev, LIBUSB_ENDPOINT_IN | 0x01, transferBuffer, bytesToRead, &actualLength, 2000);
                        if (rrc != 0 || actualLength != bytesToRead) {
                            printf("bootstrap READ fail at cmd %d (%s): rc=%d actual=%d\n", cmdIdx, command.c_str(), rrc, actualLength); fflush(stdout);
                            throw std::runtime_error("bootstrap read timeout/under-run (see cmd index above)");
                        }
                    }
                }
    
                // Ensure FPGA is idle until queueFrameRead begins streaming
                setFpgaIdle();
                sendResetStreamDma();
                auto bootMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - tBoot0).count();
                printf("Bootstrapping complete (%d commands, %lld ms)\n", cmdIdx,
                       static_cast<long long>(bootMs)); fflush(stdout);
            }
    
            if (_inputSource == lgx2::VideoInputSource::Component) {
                setVideoInput(lgx2::VideoInputSource::Component);
            }
        } catch (...) {
            // CV-06: release the claimed interface and close the handle on any
            // setup failure instead of leaking them until process exit.
            closeDevice();
            throw;
        }
    }

    bool UsbStream::sendI2cWrite(uint8_t slave7Bit, uint8_t reg, uint8_t val) {
        if (!_dev) return false;
        uint8_t cmd[5];
        cmd[0] = 0x01; // I2C write command
        cmd[1] = slave7Bit;
        cmd[2] = 0x01; // 1-byte register address length
        cmd[3] = reg;
        cmd[4] = val;
        int actual = 0;
        // CV-03: serialise every control-ep transaction. The request/response
        // pair must be atomic or a concurrent thread could steal the response.
        std::lock_guard<std::mutex> lock(_controlMutex);
        int rc = libusb_bulk_transfer(_dev, LIBUSB_ENDPOINT_OUT | 0x01, cmd, sizeof(cmd), &actual, 1000);
        if (rc != 0 || actual != sizeof(cmd)) return false;
        return true;
    }

    void UsbStream::setVideoInput(lgx2::VideoInputSource source) {
        if (source == lgx2::VideoInputSource::Component) {
            printf("[Device] Component (YPbPr) input is unsupported (requires proprietary breakout cable and analog calibration). Maintaining HDMI.\n");
            _inputSource = lgx2::VideoInputSource::HDMI;
        } else {
            _inputSource = source;
        }
        if (!_dev) return;

        printf("[Device] Video input set to HDMI\n");
        sendI2cWrite(0x20, 0x00, 0x08);
        sendI2cWrite(0x20, 0x01, 0x06);
        sendI2cWrite(0x20, 0x02, 0xFC);
        sendI2cWrite(0x20, 0x05, 0x2C);
        fflush(stdout);
    }

    int UsbStream::sendI2cRead(uint8_t slave7Bit, uint8_t reg, uint8_t *data, uint8_t len) {
        if (!_dev) return -1;
        uint8_t cmd[5];
        cmd[0] = 0x02; // I2C read command
        cmd[1] = slave7Bit;
        cmd[2] = 0x01; // 1-byte register address length
        cmd[3] = len;  // bytes to read
        cmd[4] = reg;
        int actual = 0;
        // CV-03: keep command and response atomic w.r.t. other control threads
        std::lock_guard<std::mutex> lock(_controlMutex);
        int rc = libusb_bulk_transfer(_dev, LIBUSB_ENDPOINT_OUT | 0x01, cmd, sizeof(cmd), &actual, 500);
        if (rc != 0 || actual != sizeof(cmd)) return -1;
        rc = libusb_bulk_transfer(_dev, LIBUSB_ENDPOINT_IN | 0x01, data, len, &actual, 500);
        if (rc != 0) return -1;
        return actual;
    }

    void UsbStream::queryVideoSignalStatus() {
        if (!_dev) return;

        // ------------------------------------------------------------------
        // ADV7604 register map (verified against drivers/media/i2c/adv7604.c):
        //   * IO   map @ 0x20 : video standard, STDI interlace flag, TMDS lock,
        //                       cable detect
        //   * HDMI map @ 0x34 : receiver geometry, timings, colorspace, audio
        // Many of the previous reads used the wrong register or the wrong bit
        // mask, which produced a width of (real + 4096), a bogus "50 Hz" on
        // 60 Hz sources (HDMI 0x05 bit 4 is VSYNC polarity, not a 50 Hz flag),
        // an interlaced flag taken from the wrong map, and an unstable "lock"
        // bit taken from the audio-PLL status.
        // ------------------------------------------------------------------

        auto rd = [this](uint8_t addr, uint8_t reg, uint8_t *buf, uint8_t len) {
            return sendI2cRead(addr, reg, buf, len) == static_cast<int>(len) ? 0 : 1;
        };

        uint8_t widthBytes[2] = {0, 0};
        uint8_t heightBytes[2] = {0, 0};
        uint8_t ioLock = 0;
        uint8_t ioFlags = 0;

        // Core reads: without geometry and lock the snapshot is meaningless, so
        // keep the last known good values instead of publishing garbage (CV-13).
        int coreFailures = 0;
        coreFailures += rd(0x20, 0x6A, &ioLock, 1);   // IO 0x6a: TMDS lock (mask 0xe0)
        coreFailures += rd(0x20, 0x12, &ioFlags, 1);  // IO 0x12: STDI, interlaced = bit 4
        coreFailures += rd(0x34, 0x07, widthBytes, 2);
        coreFailures += rd(0x34, 0x09, heightBytes, 2);

        if (coreFailures > 0) {
            _statusQueryFailures++;
            if (_statusQueryFailures <= 5 || (_statusQueryFailures % 60) == 0) {
                printf("[ADV7604] Status query failed (%d core I2C reads, %u consecutive) - keeping last known good values\n",
                       coreFailures, _statusQueryFailures);
                fflush(stdout);
            }
            return;
        }
        _statusQueryFailures = 0;

        // 12-bit H_ACTIVE / V_ACTIVE (Linux masks: linewidth/field0_height = 0x0fff).
        uint16_t activeWidth = ((widthBytes[0] & 0x0F) << 8) | widthBytes[1];
        uint16_t activeHeight = ((heightBytes[0] & 0x0F) << 8) | heightBytes[1];
        bool interlaced = (ioFlags & 0x10) != 0;
        // Linux: no_signal() includes no_lock_tmds() ((io 0x6a & 0xe0) == 0xe0)
        // and no_signal_tmds() (port A: io 0x6a bit 0x10 set).
        bool tmdsLocked = ((ioLock & 0xE0) == 0xE0) && ((ioLock & 0x10) != 0);

        // Optional reads: a failure degrades a single field rather than
        // discarding the whole snapshot.
        uint8_t hdmiMode = 0;
        uint8_t csByte = 0;
        uint8_t audioStatus = 0;
        uint8_t audFreqByte = 0;
        uint8_t cableByte = 0;
        uint8_t vidStd = 0;
        uint8_t ioPresence = 0;
        uint8_t pixelClkMhz = 0;
        uint8_t pixelClkFrac = 0;
        uint8_t deepColor = 0;
        uint8_t field1Bytes[2] = {0, 0};
        uint8_t porches[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        bool haveTimings = true;
        haveTimings &= rd(0x34, 0x20, porches + 0, 2) == 0;
        haveTimings &= rd(0x34, 0x22, porches + 2, 2) == 0;
        haveTimings &= rd(0x34, 0x24, porches + 4, 2) == 0;
        haveTimings &= rd(0x34, 0x2A, porches + 6, 2) == 0;
        haveTimings &= rd(0x34, 0x2E, porches + 8, 2) == 0;
        haveTimings &= rd(0x34, 0x32, porches + 10, 2) == 0;
        (void)rd(0x34, 0x05, &hdmiMode, 1);
        (void)rd(0x34, 0x06, &pixelClkMhz, 1);
        (void)rd(0x34, 0x3B, &pixelClkFrac, 1);
        (void)rd(0x34, 0x0B, &deepColor, 1);
        (void)rd(0x34, 0x0B, field1Bytes, 2);
        (void)rd(0x34, 0x53, &csByte, 1);
        (void)rd(0x34, 0x04, &audioStatus, 1);
        (void)rd(0x34, 0x18, &audFreqByte, 1);
        (void)rd(0x20, 0x6F, &cableByte, 1);
        (void)rd(0x20, 0x01, &vidStd, 1);
        (void)rd(0x20, 0x60, &ioPresence, 1); // IO 0x60: infoframe-present flags

        uint16_t field1Height = ((field1Bytes[0] & 0x0F) << 8) | field1Bytes[1];
        bool cableDetected = (cableByte & 0x01) != 0 || tmdsLocked;
        bool geometryValid = tmdsLocked && activeWidth >= 320 && activeWidth <= 4095 &&
                             activeHeight >= 120 && activeHeight <= 2048;

        // Audio: PLL lock is HDMI 0x04 bit 0; HDMI 0x18 bit 0 is the "audio
        // sample packet detected" flag (verified against Linux adv7604.c), so a
        // locked-and-detected stream needs BOTH bits. Previously only the PLL
        // bit was checked, and the low nibble of 0x18 was misread as an IEC
        // 60958 sample-rate code - a field that does not exist - which latched
        // random 32/44.1/48 kHz values and pitched the audio ("depressing"
        // audio after repeated reconnects).
        bool audioLocked = (audioStatus & 0x01) != 0 && (audFreqByte & 0x01) != 0;

        // Sample rate comes from the received Audio InfoFrame on the INFOFRAME
        // page (slave 0x3e), never from 0x18. Layout per CEA-861 / Linux
        // hdmi_audio_infoframe_unpack: payload byte 1 bits 4:2 = sample rate.
        // If the infoframe is absent or unreadable we keep the last known-good
        // rate rather than inventing one.
        uint32_t audRate = _lastSignalInfo.audioSampleRate;
        switch (audRate) {
            case 32000: case 44100: case 48000: case 88200:
            case 96000: case 176400: case 192000: break;
            default: audRate = 48000; break; // sanitise a previously latched bad value
        }
        if (audioLocked && (ioPresence & 0x02) != 0) {
            uint8_t aifHead[3] = {0, 0, 0};
            if (rd(0x3e, 0xE3, aifHead, 3) == 0 &&
                aifHead[0] == 0x84 && aifHead[1] == 0x01 && aifHead[2] == 0x0A) {
                uint8_t aifPayload[2] = {0, 0};
                if (rd(0x3e, 0x1C, aifPayload, 2) == 0) {
                    switch ((aifPayload[1] >> 2) & 0x07) {
                        case 1: audRate = 32000; break;
                        case 2: audRate = 44100; break;
                        case 3: audRate = 48000; break;
                        case 4: audRate = 88200; break;
                        case 5: audRate = 96000; break;
                        case 6: audRate = 176400; break;
                        case 7: audRate = 192000; break;
                        default: break; // 0 = "refer to stream header", keep current
                    }
                }
            }
        }

        // Colorspace: HDMI map 0x53 low nibble (Linux hdmi_color_space_txt).
        // Map to the app's 0=RGB, 1=YCbCr 4:2:2, 2=YCbCr (full range) scheme.
        uint8_t aviCs;
        switch (csByte & 0x0F) {
            case 0x0: case 0x1: aviCs = 0; break;      // RGB (limited/full)
            case 0x2: case 0x3: aviCs = 1; break;      // YCbCr 601/709 limited
            case 0x6: case 0x7: aviCs = 2; break;      // YCbCr 601/709 full range
            default:            aviCs = 1; break;      // xvYCC/sYCC/opRGB -> YCbCr
        }

        // Refresh rate from the measured pixel clock and total timings, snapped
        // to the nearest CEA/standard rate. This correctly distinguishes e.g.
        // 720p50 from 720p60 (same pixel clock, different H total).
        float fps = _lastSignalInfo.measuredFps;
        if (haveTimings && activeWidth > 0 && activeHeight > 0) {
            auto rd16 = [&](int off, unsigned mask) -> unsigned {
                return ((static_cast<unsigned>(porches[off]) << 8) | porches[off + 1]) & mask;
            };
            unsigned hFront = rd16(0, 0x3FF);
            unsigned hSync  = rd16(2, 0x3FF);
            unsigned hBack  = rd16(4, 0x3FF);
            unsigned vFront = rd16(6, 0x1FFF);
            unsigned vSync  = rd16(8, 0x1FFF);
            unsigned vBack  = rd16(10, 0x1FFF);

            double clkMhz = pixelClkMhz + ((pixelClkFrac & 0x30) >> 4) * 0.25;
            if ((hdmiMode & 0x80) != 0 && clkMhz > 0.0) {
                int bitsPerChannel = ((deepColor & 0x60) >> 4) + 8;
                int pixelRepeat = (hdmiMode & 0x0F) + 1;
                clkMhz = clkMhz * 8.0 / bitsPerChannel / pixelRepeat;
            }
            // Vertical porch registers are in half-lines (Linux divides by 2).
            double htotal = static_cast<double>(activeWidth) + hFront + hSync + hBack;
            double vtotal = static_cast<double>(activeHeight) +
                            (vFront + vSync + vBack) / 2.0;
            if (interlaced) vtotal += field1Height; // full frame height
            if (clkMhz > 1.0 && htotal > 0.0 && vtotal > 0.0) {
                double raw = (clkMhz * 1e6) / (htotal * vtotal);
                if (interlaced) raw *= 2.0; // report the conventional field rate
                static const double kStandardRates[] = {
                        23.98, 24.0, 25.0, 29.97, 30.0, 50.0, 59.94, 60.0, 100.0, 119.88, 120.0};
                double best = 60.0, bestErr = 1e9;
                for (double r : kStandardRates) {
                    double err = std::fabs(raw - r);
                    if (err < bestErr) { bestErr = err; best = r; }
                }
                fps = (bestErr < best * 0.06) ? static_cast<float>(best) : static_cast<float>(raw);
            }
        }

        // Build the complete snapshot locally, then publish it in one step so
        // getVideoSignalInfo() on the render thread can never observe a
        // half-updated structure (CV-03).
        lgx2::VideoSignalInfo info{};
        info.valid = true;
        info.locked = geometryValid;
        info.cableDetected = cableDetected;
        info.interlaced = interlaced;
        info.activeWidth = geometryValid ? activeWidth : 0;
        info.activeHeight = geometryValid ? activeHeight : 0;
        info.totalLines = static_cast<uint16_t>(interlaced ? (activeHeight + field1Height) : activeHeight);
        info.vidStd = vidStd;
        info.audioSampleRate = audRate;
        info.audioLocked = audioLocked;
        info.measuredFps = fps;
        info.aviColorspace = aviCs;

        // The ADV7604 status registers are occasionally misread (I2C glitches
        // while the bulk stream is running). Require a changed snapshot to be
        // seen twice in a row before publishing it, otherwise a flaky read
        // would flap the HUD and restart the audio device every second.
        lgx2::VideoSignalInfo published;
        {
            std::lock_guard<std::mutex> lock(_signalMutex);
            // Publish immediately when the signal (re)acquires lock. Only the
            // loss of lock, and all other field changes, need two confirmations.
            // Without this, a real signal returning after a mode switch would
            // show the "no signal" splash for up to two poll intervals.
            bool acquired = _haveSignalInfo && info.locked && !_lastSignalInfo.locked;
            if (!_haveSignalInfo || acquired || sameSignalInfo(info, _lastSignalInfo)) {
                _lastSignalInfo = info;
                _pendingSignalInfo = info;
                _pendingSignalCount = 0;
                _haveSignalInfo = true;
            } else if (sameSignalInfo(info, _pendingSignalInfo)) {
                if (++_pendingSignalCount >= 2) {
                    _lastSignalInfo = info;
                    _pendingSignalInfo = info;
                    _pendingSignalCount = 0;
                }
            } else {
                _pendingSignalInfo = info;
                _pendingSignalCount = 1;
            }
            published = _lastSignalInfo;
        }

        // Log the snapshot that was actually published (not the raw read) so a
        // flaky register cannot spam the log with alternating values.
        if (published.locked != _lastLoggedLock || published.activeWidth != _lastLoggedWidth ||
            published.activeHeight != _lastLoggedHeight || published.vidStd != _lastLoggedStd ||
            published.interlaced != _lastLoggedInterlaced || published.audioSampleRate != _lastLoggedAudioRate ||
            published.aviColorspace != _lastLoggedColorspace) {
            printf("[ADV7604] Lock: %s | Active: %ux%u%s @ %.2f Hz | Lines: %u | Audio: %u Hz | Colorspace: %s | VID_STD: 0x%02X%s\n",
                   published.locked ? "YES" : "NO",
                   published.activeWidth,
                   published.interlaced ? published.activeHeight * 2 : published.activeHeight,
                   published.interlaced ? "i" : "p", published.measuredFps, published.totalLines,
                   published.audioSampleRate,
                   published.aviColorspace == 0 ? "RGB" : (published.aviColorspace == 1 ? "YCbCr 4:2:2" : "YCbCr 4:4:4"),
                   published.vidStd,
                   published.locked ? "" : " [geometry out of range]");
            _lastLoggedLock = published.locked;
            _lastLoggedWidth = published.activeWidth;
            _lastLoggedHeight = published.activeHeight;
            _lastLoggedStd = published.vidStd;
            _lastLoggedInterlaced = published.interlaced;
            _lastLoggedAudioRate = published.audioSampleRate;
            _lastLoggedColorspace = published.aviColorspace;
        }

        // NOTE: deliberately no automatic VID_STD reprogramming. The ADV7604
        // runs in HDMI auto-detect mode (VID_STD 0x06) and the parser derives
        // the mode from the actual frame word count, so re-writing VID_STD only
        // added I2C traffic (and device instability) with no benefit.
        fflush(stdout);
    }

    void UsbStream::setVideoStandard(uint8_t std) {
        if (!_dev) return;
        printf("[ADV7604] Reprogramming VID_STD -> 0x%02X\n", std);
        sendI2cWrite(0x20, 0x01, std);
        fflush(stdout);
    }

    bool UsbStream::sendResetStreamDma() {
        if (!_dev) return false;
        // Reverse engineered from libC877Driver Fx3Firmware::resetStreamDMA (0x0001044a):
        // Sends single byte command 0x14 over EP1 OUT to reset Cypress FX3 stream DMA.
        uint8_t cmd = 0x14;
        int actual = 0;
        std::lock_guard<std::mutex> lock(_controlMutex); // CV-03: serialise control traffic
        int rc = libusb_bulk_transfer(_dev, LIBUSB_ENDPOINT_OUT | 0x01, &cmd, 1, &actual, 500);
        return rc == 0 && actual == 1;
    }

    void UsbStream::setFpgaWork() {
        // Reverse engineered from libC877Driver CLFE3::SetWorkMode (0x0000d85c):
        // Read FPGA (I2C 0x10) reg 0x05, set bit 1 (mode |= 0x02), write back.
        if (!_dev) return;
        uint8_t mode = 0;
        if (sendI2cRead(0x10, 0x05, &mode, 1) != 1) {
            mode = 0x00;
        }
        sendI2cWrite(0x10, 0x05, static_cast<uint8_t>(mode | 0x02));
    }

    void UsbStream::setFpgaIdle() {
        // Reverse engineered from libC877Driver CLFE3::SetIdleMode (0x0000d8ac):
        // Read FPGA (I2C 0x10) reg 0x05, clear bit 1 (mode &= ~0x02), write back.
        if (!_dev) return;
        uint8_t mode = 0;
        if (sendI2cRead(0x10, 0x05, &mode, 1) != 1) {
            mode = 0x02;
        }
        sendI2cWrite(0x10, 0x05, static_cast<uint8_t>(mode & ~0x02));
    }

    void UsbStream::queueFrameRead(std::function<void(uint8_t *, uint32_t)> *onData) {
        _onFrameDataCallback = onData;

        // Allocate all PIPELINE_DEPTH bulk transfers immediately (CV-07: only
        // ever called from this thread - never from a libusb completion
        // callback, so shutdown cannot race the vector's construction)
        queueAllFrameReads();
        _readThread = std::thread(&UsbStream::readLoop, this);

        // Reset FX3 stream DMA to clear any internal FIFO pointers
        sendResetStreamDma();

        // Start FPGA video & audio streaming (mirroring CLFE3::SetWorkMode in vendor driver)
        setFpgaWork();

        // CV-03: start the control worker so periodic ADV7604 status polling and
        // automatic VID_STD alignment happen off the frame/display thread.
        _controlStop.store(false, std::memory_order_release);
        _controlThread = std::thread(&UsbStream::controlLoop, this);
    }

    void UsbStream::controlLoop() {
        while (!_controlStop.load(std::memory_order_acquire)) {
            queryVideoSignalStatus();
            // Poll ~10x/s. The ADV7604 geometry is the authoritative width used
            // to reject transitional frames during a resolution switch, so it
            // must refresh quickly; with the two-confirmation debounce a change
            // is published in ~200 ms instead of ~2 s. Wake every 50 ms so
            // shutdown is not delayed by the poll interval.
            for (int i = 0; i < 2 && !_controlStop.load(std::memory_order_acquire); ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        }
    }

    void UsbStream::update() {
        serviceStalledTransfers();

        if (_hasError.load(std::memory_order_acquire)) {
            std::lock_guard<std::mutex> errLock(_errorMutex);
            throw std::runtime_error(_errorMessage);
        }

        // CV-17: block for the next transfer instead of busy-spinning the main
        // thread. The completion callback notifies _queueCv; the timeout only
        // matters when the stream is idle (or shutting down), where it bounds
        // the latency of SDL event polling without burning a core.
        {
            std::unique_lock<std::mutex> lock(_queueMutex);
            if (_frameQueue.empty() && !_shuttingDown.load(std::memory_order_acquire)) {
                _queueCv.wait_for(lock, std::chrono::milliseconds(5), [this] {
                    return !_frameQueue.empty() || _shuttingDown.load(std::memory_order_acquire);
                });
            }
        }

        // CV-02: bound the work done per iteration so SDL event handling and
        // presentation cannot be starved while a backlog drains. The budget must
        // still exceed the capture rate (~60-120 transfers/s at 1080p) or the
        // queue slowly fills and starts dropping old transfers, which shows up
        // as persistent preview latency. 10 ms leaves the rest of a 60 Hz frame
        // for presentation while allowing several transfers to be drained.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(10);
        for (int processed = 0; processed < MAX_ITEMS_PER_UPDATE; ++processed) {
            QueuedTransfer item;
            {
                std::lock_guard<std::mutex> lock(_queueMutex);
                if (_frameQueue.empty()) break;
                item = std::move(_frameQueue.front());
                _frameQueue.pop();
            }

            // CV-02: report capture-to-parse latency so backlog problems are
            // measurable rather than only visible as a lagging preview
            auto age = std::chrono::steady_clock::now() - item.queuedAt;
            if (age > std::chrono::milliseconds(250)) {
                if (++_backlogWarnings <= 10 || (_backlogWarnings % 60) == 0) {
                    printf("[USB] Backlog: oldest queued transfer is %lld ms old (high-water %d)\n",
                           static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(age).count()),
                           _queueHighWater);
                    fflush(stdout);
                }
            }

            (*_onFrameDataCallback)(item.data.data(), static_cast<uint32_t>(item.data.size()));
            {
                std::lock_guard<std::mutex> lock(_queueMutex);
                _freeBuffers.push_back(std::move(item.data));
            }

            if (std::chrono::steady_clock::now() >= deadline) break;
        }
    }

    void UsbStream::onFrameData(libusb_transfer *transfer) {
        if (isShuttingDown()) return;
        if (_recordCap && _record.size() + (size_t)transfer->actual_length <= _recordCap) {
            if (_recordIndex.empty()) _recordT0 = std::chrono::steady_clock::now();
            _record.insert(_record.end(), transfer->buffer, transfer->buffer + transfer->actual_length);
            _recordIndex.emplace_back(
                std::chrono::duration<double>(std::chrono::steady_clock::now() - _recordT0).count(),
                (uint32_t)transfer->actual_length);
        }
        std::lock_guard<std::mutex> lock(_queueMutex);
        if (static_cast<int>(_frameQueue.size()) >= MAX_QUEUE_DEPTH) {
            // CV-02 overload policy: drop the OLDEST queued transfer so the
            // parser resumes near-live video as soon as the burst passes,
            // instead of grinding through a stale backlog. The resulting gap is
            // detected by the sequence check in Device::onFrameData, which
            // discards the incomplete frame and resynchronises on the next C0.
            _frameQueue.pop();
            _droppedTransfers++;
            auto now = std::chrono::steady_clock::now();
            if (now - _lastDropLog > std::chrono::seconds(2)) {
                _lastDropLog = now;
                printf("[USB] Queue full (%d): dropped oldest transfer (dropped=%d)\n",
                       MAX_QUEUE_DEPTH, _droppedTransfers);
                fflush(stdout);
            }
        }

        QueuedTransfer item;
        if (!_freeBuffers.empty()) {
            item.data = std::move(_freeBuffers.back());
            _freeBuffers.pop_back();
        } else {
            item.data.reserve(LGX_DATA_FRAME_LEN);
        }
        item.data.assign(transfer->buffer, transfer->buffer + transfer->actual_length);
        item.queuedAt = std::chrono::steady_clock::now();
        if (static_cast<int>(_frameQueue.size()) + 1 > _queueHighWater) {
            _queueHighWater = static_cast<int>(_frameQueue.size()) + 1;
        }
        _frameQueue.push(std::move(item));
        _queuedTransfers++;
        _queueCv.notify_one();  // CV-17: wake the main loop immediately
        if ((_queuedTransfers % 600) == 0) {
            printf("usb: queued=%d dropped=%d qdepth=%d high-water=%d\n",
                _queuedTransfers, _droppedTransfers, (int)_frameQueue.size(), _queueHighWater);
            fflush(stdout);
        }
    }

    void UsbStream::readLoop() {
        struct timeval tv{0, 50000};  // 50ms timeout so loop is responsive
        while (!isShuttingDown() || _activeTransfers.load(std::memory_order_acquire) > 0) {
            libusb_handle_events_timeout(nullptr, &tv);
        }
    }

    void UsbStream::shutdownStream() {
        if (_shutdownDone) return; // CV-06: safe to call from Device::shutdown *and* the destructor
        _shutdownDone = true;

        // CV-03: stop the control worker before any teardown I2C traffic so no
        // status query races setFpgaIdle()/device closure.
        _controlStop.store(true, std::memory_order_release);
        if (_controlThread.joinable()) {
            _controlThread.join();
        }

        // Mirror vendor AVSC877Device::StopStreaming -> Device::StopStream -> CLFE3::SetIdleMode
        //
        // CV-15/state-preserving shutdown: this deliberately does *not* touch
        // the ADV7604 or its EDID RAM. Only the FPGA stream-enable bit is
        // cleared (setting it idle) and the FX3 stream DMA is reset, so video
        // stops while the receiver's 256-byte EDID and register config stay in
        // place for the next run. There is no libusb_reset_device and
        // set_configuration is skipped when already set, so chip state survives
        // an app restart as long as the device stays powered.
        if (_dev) {
            setFpgaIdle();
            sendResetStreamDma();
        }
        _shuttingDown.store(true, std::memory_order_release);
        _queueCv.notify_all();  // CV-17: release any main-loop wait

        for (auto *transfer : _transfers) {
            libusb_cancel_transfer(transfer);
        }

        if (_readThread.joinable()) {
            _readThread.join();
        }

        {
            std::lock_guard<std::mutex> lock(_stallMutex);
            _stalledTransfers.clear(); // parked transfers are part of _transfers
        }
        for (auto *transfer : _transfers) {
            libusb_free_transfer(transfer);
        }
        _transfers.clear();

        if (!_recordPath.empty() && !_record.empty()) {
            if (FILE *fp = fopen(_recordPath.c_str(), "wb")) {
                fwrite(_record.data(), 1, _record.size(), fp);
                fclose(fp);
            }
            if (FILE *fp = fopen((_recordPath + ".idx").c_str(), "w")) {
                for (auto &e : _recordIndex) fprintf(fp, "%.6f %u\n", e.first, e.second);
                fclose(fp);
            }
            printf("Recorded %zu bytes (%zu transfers) to %s\n", _record.size(), _recordIndex.size(), _recordPath.c_str());
        }

        if (_dev) {
            // Flush any residual FIFO data while the buffer is still allocated
            // (it must be freed *after* this read, not before). CV-15: the
            // trailing clear_halt was removed - it costs ~5 s and is pointless
            // on exit since the handle is closed; a leftover halt is recovered
            // by serviceStalledTransfers() (CV-12) on the next run.
            int check = 0;
            libusb_bulk_transfer(_dev, LIBUSB_ENDPOINT_IN | 0x03, _frameBuffer, LGX_DATA_FRAME_LEN, &check, 50);
        }

        delete[] _frameBuffer;
        _frameBuffer = nullptr;

        closeDevice(); // CV-06: release interface + handle
    }

    void UsbStream::submitTransfer(libusb_transfer *transfer) {
        if (!isShuttingDown() && _dev != nullptr) {
            int rc = libusb_submit_transfer(transfer);
            if (rc == 0) {
                _submitFailures.store(0, std::memory_order_relaxed);
                _activeTransfers.fetch_add(1, std::memory_order_relaxed);
            } else if (!isShuttingDown()) {
                int failures = _submitFailures.fetch_add(1, std::memory_order_relaxed) + 1;
                fprintf(stderr, "[USB] libusb_submit_transfer failed: %d (consecutive: %d)\n", rc, failures);
                // CV-08: a submission failure that empties the pipeline must be
                // escalated instead of silently degrading to a dead preview.
                if (_activeTransfers.load(std::memory_order_relaxed) == 0) {
                    signalError("USB pipeline stalled: no transfers remain active and resubmission failed");
                }
            }
        }
    }

    void UsbStream::onTransferStalled(libusb_transfer *transfer) {
        {
            std::lock_guard<std::mutex> lock(_stallMutex);
            _stalledTransfers.push_back(transfer);
        }
        _stallPending.store(true, std::memory_order_release);
    }

    void UsbStream::serviceStalledTransfers() {
        if (!_stallPending.load(std::memory_order_acquire)) return;
        if (_dev == nullptr || isShuttingDown()) return;

        // CV-12: clear_halt runs here, on the main thread, outside the libusb
        // completion callback, and parked transfers are only resubmitted once
        // recovery has actually succeeded.
        int rc = libusb_clear_halt(_dev, LIBUSB_ENDPOINT_IN | 0x03);
        if (rc == LIBUSB_SUCCESS) {
            _stallPending.store(false, std::memory_order_release);
            _stallRecoveryAttempts = 0;
            std::vector<libusb_transfer *> parked;
            {
                std::lock_guard<std::mutex> lock(_stallMutex);
                parked.swap(_stalledTransfers);
            }
            if (!parked.empty()) {
                printf("[USB] Endpoint recovered from stall; resubmitting %zu transfer(s)\n", parked.size());
                fflush(stdout);
            }
            for (auto *transfer : parked) {
                submitTransfer(transfer);
            }
            return;
        }

        if (++_stallRecoveryAttempts >= MAX_STALL_RECOVERY_ATTEMPTS) {
            _stallPending.store(false, std::memory_order_release);
            _stallRecoveryAttempts = 0;
            fprintf(stderr, "[USB] clear_halt failed repeatedly (rc=%d)\n", rc);
            signalError("USB endpoint stalled and clear_halt keeps failing");
        }
    }

    int UsbStream::noteTransferError() {
        return _transferErrorCount.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    void UsbStream::signalError(const char *message) {
        // Only the first error is reported: a disconnect or failed recovery
        // completes several in-flight transfers at once, each of which would
        // otherwise print the same fatal message in a burst.
        if (_errorSignaled.exchange(true, std::memory_order_acq_rel)) return;
        {
            std::lock_guard<std::mutex> lock(_errorMutex);
            _errorMessage = message;
        }
        _hasError.store(true, std::memory_order_release);
    }

    void UsbStream::queueAllFrameReads() {
        // CV-07: called only from queueFrameRead() on the main thread, before
        // _readThread exists - the transfer vector is never mutated from a
        // completion callback.
        for (int s = 0; s < PIPELINE_DEPTH; s++) {
            libusb_transfer *transfer = libusb_alloc_transfer(0);
            libusb_fill_bulk_transfer(transfer, _dev, LIBUSB_ENDPOINT_IN | 0x03,
                                      _frameBuffer + (size_t)s * LGX_DATA_FRAME_LEN,
                                      LGX_DATA_FRAME_LEN,
                                      usbTransferComplete, this, 0);
            _transfers.push_back(transfer);
            submitTransfer(transfer);
        }
    }
}