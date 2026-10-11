#ifndef LGX2USERSPACE_USBSTREAM_H
#define LGX2USERSPACE_USBSTREAM_H

#include <vector>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <chrono>
#include <libusb-1.0/libusb.h>
#include "lgxdevice.h"

namespace libusb {

    class UsbStream : public lgx2::Stream {
    public:
        explicit UsbStream();
        ~UsbStream() override;

        bool deviceAvailable(lgx2::DeviceType deviceType) override;
        void streamSetupCommands(lgx2::DeviceType deviceType) override;
        void queueFrameRead(std::function<void(uint8_t *, uint32_t)> *onData) override;
        void update() override;

        void onFrameData(libusb_transfer *transfer);

        void shutdownStream() override;

        void submitTransfer(libusb_transfer *transfer);
        void signalError(const char *message);
        void onTransferStalled(libusb_transfer *transfer);
        int noteTransferError();

        bool isShuttingDown() const { return _shuttingDown.load(std::memory_order_acquire); }
        void decrementActiveTransfers() { _activeTransfers.fetch_sub(1, std::memory_order_relaxed); }

        void setVideoInput(lgx2::VideoInputSource source) override;
        void setFastBootstrap(bool fast) override { _fastBootstrap = fast; }
        void queryVideoSignalStatus() override;
        void setVideoStandard(uint8_t std) override;
        void setCvx8897Conversion(const std::string &mode) override;
        void setFpgaIdle();
        void setFpgaWork();
        bool sendResetStreamDma();
        lgx2::VideoSignalInfo getVideoSignalInfo() const override {
            std::lock_guard<std::mutex> lock(_signalMutex);
            return _lastSignalInfo;
        }
        bool sendI2cWrite(uint8_t slave7Bit, uint8_t reg, uint8_t val);
        int sendI2cRead(uint8_t slave7Bit, uint8_t reg, uint8_t *data, uint8_t len);

    private:
        // CV-02: bounded backlog. 128 entries allowed ~254 MiB of stale video to
        // pile up; 32 keeps the queue shallow while absorbing scheduling jitter.
        static constexpr int MAX_QUEUE_DEPTH = 32;
        // CV-02: upper bound on transfers parsed per update() call so SDL event
        // handling and presentation stay responsive while the queue drains.
        // 32 items (one full queue) lets a single update fully recover a burst;
        // the wall-clock budget below still caps the stall.
        static constexpr int MAX_ITEMS_PER_UPDATE = 32;
        // CV-12: give up on endpoint recovery after this many failed clear_halt tries
        static constexpr int MAX_STALL_RECOVERY_ATTEMPTS = 10;

        struct QueuedTransfer {
            std::vector<uint8_t> data;
            std::chrono::steady_clock::time_point queuedAt;
        };

        void readLoop();
        void controlLoop();              // CV-03: ADV7604 polling off the video thread
        void serviceStalledTransfers();  // CV-12: clear_halt outside the completion path
        void queueAllFrameReads();       // CV-07: main-thread only, never from a callback
        void closeDevice();              // CV-06: exception-safe handle/interface release
        void maybeApplyCvx8897(const lgx2::VideoSignalInfo &info);  // CV-28

        // CV-28f: the vendor lowers the CVX8897 with SetMode()/HWReset() and only
        // then uploads the register table with SetParam(). SetMode(2) (used for
        // the converted output) pulses the converter's active-low reset line --
        // PCA9539 GPIO expander at 7-bit I2C 0x74, output port 0 bit 4 -- toggles
        // the output-enable bit (port 0 bit 3) and rewrites CVX8897 regs 0x8a
        // bit 1 / 0x0d bit 7. Uploading the table without this preamble leaves
        // the output PLL dead ("HDMI clock recovering" forever).
        bool pca9539SetGpio(uint8_t port, uint8_t pin, bool dirInput, bool value);
        bool cvx8897SetAllOutputModeBits(bool set);
        // Vendor SetMode(2) = converted output, SetMode(1) = native/passthrough.
        bool cvx8897SetModeImpl(bool converted);
        bool cvx8897SetModeEnable() { return cvx8897SetModeImpl(true); }
        bool cvx8897SetModeNative() { return cvx8897SetModeImpl(false); }

        int _droppedTransfers{0};
        int _queuedTransfers{0};
        int _queueHighWater{0};
        int _backlogWarnings{0};
        std::chrono::steady_clock::time_point _lastDropLog{};

        libusb_device_handle *_dev;
        bool _interfaceClaimed{false};
        bool _libusbInited{false};
        bool _shutdownDone{false};
        lgx2::VideoInputSource _inputSource{lgx2::VideoInputSource::HDMI};
        bool _fastBootstrap{false};

        // CV-28: CVX8897 hardware frame-rate converter. The request is set from
        // the CLI before initialise(); it is applied once, either on the initial
        // status query (explicit mode, or "half" when the source is already
        // locked) or on the first locked snapshot seen by the control thread.
        std::string _cvx8897Request{};
        bool _cvx8897Pending{false};
        bool _cvx8897Applied{false};
        int _cvx8897NoMatchTries{0};

        std::vector<libusb_transfer *> _transfers;
        std::atomic<int> _activeTransfers{0};
        std::atomic<int> _submitFailures{0};     // CV-08
        std::atomic<int> _transferErrorCount{0}; // CV-14: was a function-local static

        // CV-12: transfers parked while the endpoint halt is cleared by the
        // main thread in update()
        std::mutex _stallMutex;
        std::vector<libusb_transfer *> _stalledTransfers;
        std::atomic<bool> _stallPending{false};
        int _stallRecoveryAttempts{0};

        std::vector<lgx2::DeviceType> _availableDevices;

        std::function<void(uint8_t *, uint32_t)> *_onFrameDataCallback;

        uint8_t *_frameBuffer;
        std::atomic<bool> _shuttingDown{false};
        std::atomic<bool> _hasError{false};
        std::atomic<bool> _errorSignaled{false}; // CV-08: report only the first fatal error
        mutable std::mutex _errorMutex;
        std::string _errorMessage;

        // CV-03: control worker state. _controlMutex serialises every EP1
        // control transaction (held only inside the sendI2c*/sendResetStreamDma
        // leaf functions, so no nesting occurs); _signalMutex only guards the
        // cached status snapshot and is held briefly so the render thread never
        // blocks behind a slow I2C transaction.
        std::thread _controlThread;
        std::atomic<bool> _controlStop{false};
        std::mutex _controlMutex;
        mutable std::mutex _signalMutex;

        lgx2::VideoSignalInfo _lastSignalInfo{};

        // CV-13: a changed status snapshot must be observed twice before it is
        // published, so flaky I2C reads cannot flap the HUD / audio device.
        lgx2::VideoSignalInfo _pendingSignalInfo{};
        int _pendingSignalCount{0};
        bool _haveSignalInfo{false};

        // CV-14: status-change logging state (previously function-local statics)
        uint16_t _lastLoggedWidth{0};
        uint16_t _lastLoggedHeight{0};
        uint8_t _lastLoggedStd{0xFF};
        bool _lastLoggedLock{false};
        bool _lastLoggedOutOfRange{false};
        bool _lastLoggedInterlaced{false};
        uint32_t _lastLoggedAudioRate{0};
        uint8_t _lastLoggedColorspace{0xFF};
        uint32_t _statusQueryFailures{0};
        // CV-24: STDI telemetry change-tracking in the status log.
        bool _lastLoggedHadStdi{false};
        uint16_t _lastLoggedStdiLcf{0};
        uint16_t _lastLoggedStdiBl{0};

        std::thread _readThread;
        std::mutex _queueMutex;
        std::condition_variable _queueCv;  // CV-17: wake the main loop on new data
        std::queue<QueuedTransfer> _frameQueue;
        std::vector<std::vector<uint8_t>> _freeBuffers;

        // Raw recorder (env LGX_RECORD=path, LGX_RECORD_MB=size). Records every
        // completed transfer on the USB thread *before* the queue-drop decision,
        // so the file is the exact wire stream. Written out at shutdown.
        std::string _recordPath;
        std::vector<uint8_t> _record;
        size_t _recordCap{0};
        std::vector<std::pair<double, uint32_t>> _recordIndex;  // (t sec, bytes)
        std::chrono::steady_clock::time_point _recordT0{};
    };
}

#endif //LGX2USERSPACE_USBSTREAM_H
