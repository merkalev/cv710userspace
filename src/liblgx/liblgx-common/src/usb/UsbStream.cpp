#include "UsbStream.h"

#include <libusb-1.0/libusb.h>
#include <exception>
#include <system_error>
#include <sstream>
#include <algorithm>
#include <cstring>

#include "bootstrap/commanddata_cv710.h"

static void probeTransferComplete(struct libusb_transfer *transfer) {
    static const uint8_t frameStart[] = { 0x00, 0xFF, 0xFF, 0xC0 };
    auto *stream = static_cast<libusb::UsbStream *>(transfer->user_data);
    stream->decrementActiveTransfers();

    if (stream->isShuttingDown()) {
        return;
    }

    if (transfer->status == LIBUSB_TRANSFER_COMPLETED) {
        // CV710 quirk: transfers are 2MB single-sub-chunk parts; alignment marker
        // may sit anywhere, not just at offset 0. Scan for it.
        const int n = transfer->actual_length;
        for (int off = 0; off + 4 <= n; off += 4) {
            if (memcmp(transfer->buffer + off, frameStart, 4) == 0) {
                // Enqueue from the marker onward so the first frame isn't a splice.
                if (off != 0) {
                    memmove(transfer->buffer, transfer->buffer + off, n - off);
                    transfer->actual_length = n - off;
                }
                stream->onFrameData(transfer);
                stream->queueAllFrameReads();
                return;
            }
        }
    }

    if (!stream->recordProbeAttempt()) {
        fprintf(stderr, "[USB] Notice: Probe finished; transitioning to deep reader pipeline\n");
        stream->queueAllFrameReads();
        return;
    }

    stream->submitTransfer(transfer);
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
    } else {
        // Transient error during capture (e.g. HDMI sync loss, mode change, timeout, stall, overflow)
        static int errorCount = 0;
        if (++errorCount <= 10 || (errorCount % 100) == 0) {
            fprintf(stderr, "[USB] Transfer non-fatal warning: status %d (count: %d)\n", transfer->status, errorCount);
        }
        if (transfer->status == LIBUSB_TRANSFER_STALL) {
            stream->clearHalt();
        }
    }

    stream->submitTransfer(transfer);
}


namespace libusb {
    static const int LGX_DATA_FRAME_LEN = 0x1FC000;

    UsbStream::UsbStream() : _dev{nullptr}, _onFrameDataCallback{} {
        libusb_init(nullptr);

        libusb_device **list = nullptr;
        ssize_t count = libusb_get_device_list(nullptr, &list);

        for (ssize_t idx = 0; idx < count; ++idx) {
            libusb_device *device = list[idx];
            libusb_device_descriptor desc{};

            libusb_get_device_descriptor(device, &desc);
            if (desc.idVendor == 0x07ca && desc.idProduct == 0x0710) {
                if (desc.bcdUSB >= 0x300) {
                    printf("AVerMedia CV710 (ExtremeCap U3) detected\n");
                    _availableDevices.push_back(lgx2::DeviceType::CV710);
                } else {
                    fprintf(stderr, "CV710 detected, but not on USB3. Ensure you are using a USB3 port and cable.\n");
                }
            }
        }

        libusb_free_device_list(list, (int) count);
        _frameBuffer = new uint8_t[LGX_DATA_FRAME_LEN * 16];

        if (const char *rec = getenv("LGX_RECORD")) {
            const char *mb = getenv("LGX_RECORD_MB");
            _recordPath = rec;
            _recordCap = (size_t)(mb ? atoi(mb) : 256) << 20;
            _record.reserve(_recordCap);
            printf("Recording raw EP83 stream to %s (%zu MB)\n", rec, _recordCap >> 20);
        }
    }

    UsbStream::~UsbStream() {
        if (!_shuttingDown && _readThread.joinable()) {
            shutdownStream();
        }
        delete[] _frameBuffer;
        _frameBuffer = nullptr;
        libusb_exit(nullptr);
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
        std::string targetCommands = cv710_setup_commands;

        if (_dev == nullptr) {
            throw std::runtime_error(
                    "Failed to open device - is it connected? Run lsusb to check and ensure you have installed the udev rules (and restarted udev if necessary!)");
        }

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

        int check = 0;
        int res = libusb_bulk_transfer(_dev, LIBUSB_ENDPOINT_IN | 0x03, _frameBuffer, LGX_DATA_FRAME_LEN, &check, 100);
        if (res != LIBUSB_ERROR_TIMEOUT) {
            printf("Device already streaming (%d bytes pending) - skipping init, going straight to video\n", check);
        } else {
            printf("Bootstrapping the device\n");
        }

        int actualLength;
        uint8_t transferBuffer[512]{0};

        if (res == LIBUSB_ERROR_TIMEOUT) {
        auto commands = std::istringstream{targetCommands};
        std::string command;
        int cmdIdx = 0;
        while (commands >> command) {
            if ((cmdIdx % 2000) == 0) { printf("bootstrap cmd %d: %s\n", cmdIdx, command.c_str()); fflush(stdout); }
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

        printf("Bootstrapping complete\n"); fflush(stdout);
        } // end fresh-bootstrap-only
    }

    void UsbStream::queueFrameRead(std::function<void(uint8_t *, uint32_t)> *onData) {
        _onFrameDataCallback = onData;

        _probeTransfer = libusb_alloc_transfer(0);

        libusb_fill_bulk_transfer(_probeTransfer, _dev, LIBUSB_ENDPOINT_IN | 0x03,
                                  _frameBuffer, LGX_DATA_FRAME_LEN,
                                  probeTransferComplete, this, 0);

        _transfers.push_back(_probeTransfer);
        _readThread = std::thread(&UsbStream::readLoop, this);
        submitTransfer(_probeTransfer);
    }

    void UsbStream::update() {
        if (_hasError.load(std::memory_order_acquire)) {
            throw std::runtime_error(_errorMessage);
        }

        while (true) {
            std::vector<uint8_t> frame;
            {
                std::lock_guard<std::mutex> lock(_queueMutex);
                if (_frameQueue.empty()) break;
                frame = std::move(_frameQueue.front());
                _frameQueue.pop();
            }
            (*_onFrameDataCallback)(frame.data(), static_cast<uint32_t>(frame.size()));
            {
                std::lock_guard<std::mutex> lock(_queueMutex);
                _freeBuffers.push_back(std::move(frame));
            }
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
        if (static_cast<int>(_frameQueue.size()) < MAX_QUEUE_DEPTH) {
            std::vector<uint8_t> frame;
            if (!_freeBuffers.empty()) {
                frame = std::move(_freeBuffers.back());
                _freeBuffers.pop_back();
            } else {
                frame.reserve(LGX_DATA_FRAME_LEN);
            }
            frame.assign(transfer->buffer, transfer->buffer + transfer->actual_length);
            _frameQueue.push(std::move(frame));
            _queuedTransfers++;
            if ((_queuedTransfers % 600) == 0) {
                printf("usb: queued=%d dropped=%d qdepth=%d\n",
                    _queuedTransfers, _droppedTransfers, (int)_frameQueue.size());
                fflush(stdout);
            }
        } else {
            _droppedTransfers++;
        }
    }

    void UsbStream::readLoop() {
        struct timeval tv{0, 50000};  // 50ms timeout so loop is responsive
        while (!isShuttingDown() || _activeTransfers.load(std::memory_order_acquire) > 0) {
            libusb_handle_events_timeout(nullptr, &tv);
        }
    }

    void UsbStream::shutdownStream() {
        _shuttingDown.store(true, std::memory_order_release);

        for (auto *transfer : _transfers) {
            libusb_cancel_transfer(transfer);
        }

        if (_readThread.joinable()) {
            _readThread.join();
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

        delete[] _frameBuffer;
        _frameBuffer = nullptr;
        if (_dev) {
            libusb_release_interface(_dev, 0);
            libusb_reset_device(_dev);
            libusb_close(_dev);
            _dev = nullptr;
        }
    }

    void UsbStream::submitTransfer(libusb_transfer *transfer) {
        if (!isShuttingDown() && _dev != nullptr) {
            int rc = libusb_submit_transfer(transfer);
            if (rc == 0) {
                _activeTransfers.fetch_add(1, std::memory_order_relaxed);
            } else if (!isShuttingDown()) {
                fprintf(stderr, "[USB] libusb_submit_transfer failed: %d\n", rc);
            }
        }
    }

    void UsbStream::clearHalt() {
        if (_dev) {
            libusb_clear_halt(_dev, LIBUSB_ENDPOINT_IN | 0x03);
        }
    }

    bool UsbStream::recordProbeAttempt() {
        return ++_probeAttempts <= MAX_PROBE_ATTEMPTS;
    }

    void UsbStream::signalError(const char *message) {
        _errorMessage = message;
        _hasError.store(true, std::memory_order_release);
    }

    void UsbStream::queueAllFrameReads() {
        static constexpr int PIPELINE_DEPTH = 8;
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