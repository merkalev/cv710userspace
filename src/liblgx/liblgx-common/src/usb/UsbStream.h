#ifndef LGX2USERSPACE_USBSTREAM_H
#define LGX2USERSPACE_USBSTREAM_H

#include <vector>
#include <queue>
#include <thread>
#include <mutex>
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
        bool recordProbeAttempt();
        void clearHalt();

        bool isShuttingDown() const { return _shuttingDown.load(std::memory_order_acquire); }
        void decrementActiveTransfers() { _activeTransfers.fetch_sub(1, std::memory_order_relaxed); }

        void queueAllFrameReads();

        void setVideoInput(lgx2::VideoInputSource source) override;
        void queryVideoSignalStatus() override;
        void setVideoStandard(uint8_t std) override;
        bool sendI2cWrite(uint8_t slave7Bit, uint8_t reg, uint8_t val);
        int sendI2cRead(uint8_t slave7Bit, uint8_t reg, uint8_t *data, uint8_t len);

    private:
        static constexpr int MAX_QUEUE_DEPTH = 128;
        int _droppedTransfers{0};
        int _queuedTransfers{0};

        libusb_device_handle *_dev;
        lgx2::VideoInputSource _inputSource{lgx2::VideoInputSource::HDMI};

        std::vector<libusb_transfer *> _transfers;
        libusb_transfer *_probeTransfer{nullptr};
        std::atomic<int> _activeTransfers{0};

        std::vector<lgx2::DeviceType> _availableDevices;

        std::function<void(uint8_t *, uint32_t)> *_onFrameDataCallback;

        uint8_t *_frameBuffer;
        std::atomic<bool> _shuttingDown{false};
        std::atomic<bool> _hasError{false};
        std::string _errorMessage;

        std::thread _readThread;
        std::mutex _queueMutex;
        std::queue<std::vector<uint8_t>> _frameQueue;
        std::vector<std::vector<uint8_t>> _freeBuffers;
        std::chrono::steady_clock::time_point _lastSubmitTime{};

        static constexpr int MAX_PROBE_ATTEMPTS = 8;
        int _probeAttempts{0};

        // Raw recorder (env LGX_RECORD=path, LGX_RECORD_MB=size). Records every
        // completed transfer on the USB thread *before* the queue-drop decision,
        // so the file is the exact wire stream. Written out at shutdown.
        std::string _recordPath;
        std::vector<uint8_t> _record;
        size_t _recordCap{0};
        std::vector<std::pair<double, uint32_t>> _recordIndex;  // (t sec, bytes)
        std::chrono::steady_clock::time_point _recordT0{};

        void readLoop();
    };
}

#endif //LGX2USERSPACE_USBSTREAM_H
