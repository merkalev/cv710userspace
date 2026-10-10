cmake_minimum_required(VERSION 3.18)

find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL3   REQUIRED IMPORTED_TARGET sdl3)
pkg_check_modules(LIBUSB REQUIRED IMPORTED_TARGET libusb-1.0)

set(BOOTSTRAP_SOURCES
        src/bootstrap/commanddata_cv710.cpp
        src/bootstrap/commanddata_cv710.h
        src/bootstrap/commanddata_cv710_fast.cpp)

set(SDLOUTPUT_SOURCES
        src/sdl/SdlVideoOutput.cpp
        src/sdl/SdlVideoOutput.h
        src/sdl/SdlAudioOutput.cpp
        src/sdl/SdlAudioOutput.h)

set(NULL_SOURCES
        src/NullVideoOutput.cpp
        src/NullVideoOutput.h
        src/NullAudioOutput.cpp
        src/NullAudioOutput.h)

set(FAKE_USB_SOURCES
        src/FakeUsbStream.cpp
        src/FakeUsbStream.h)

set(USB_SOURCES
        src/usb/UsbStream.cpp
        src/usb/UsbStream.h)

set(ERRORSINK_SOURCES
        src/SimpleErrorSink.cpp
        src/SimpleErrorSink.h)

add_library(liblgx-common STATIC
        ${SDLOUTPUT_SOURCES}
        ${NULL_SOURCES}
        ${USB_SOURCES}
        ${BOOTSTRAP_SOURCES}
        ${FAKE_USB_SOURCES}
        ${ERRORSINK_SOURCES}
        src/lgxcommon.h)

target_include_directories(liblgx-common PUBLIC src)
target_include_directories(liblgx-common PRIVATE ../src)

target_link_libraries(liblgx-common PUBLIC
        PkgConfig::SDL3
        PkgConfig::LIBUSB)


