cmake_minimum_required(VERSION 3.22.1)
project(sdrstudio LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Coeur DSP partage avec la version Windows : aucun code duplique.
set(SDR_CORE "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../core")
set(SDR_THIRD "${CMAKE_CURRENT_SOURCE_DIR}/third_party")

# ---------------------------------------------------------------------------
# libusb 1.0.30. En amont il n'y a pas de CMake (autotools seul) : on compile
# les sources avec le config.h Android officiel de libusb (android/config.h).
# ---------------------------------------------------------------------------
add_library(libusb1.0 STATIC
    ${SDR_THIRD}/libusb/libusb/core.c
    ${SDR_THIRD}/libusb/libusb/descriptor.c
    ${SDR_THIRD}/libusb/libusb/hotplug.c
    ${SDR_THIRD}/libusb/libusb/io.c
    ${SDR_THIRD}/libusb/libusb/strerror.c
    ${SDR_THIRD}/libusb/libusb/sync.c
    ${SDR_THIRD}/libusb/libusb/os/events_posix.c
    ${SDR_THIRD}/libusb/libusb/os/linux_netlink.c
    ${SDR_THIRD}/libusb/libusb/os/linux_usbfs.c
    ${SDR_THIRD}/libusb/libusb/os/threads_posix.c
)
target_include_directories(libusb1.0
    PUBLIC ${SDR_THIRD}/libusb/libusb
    PRIVATE ${SDR_THIRD}/libusb/android ${SDR_THIRD}/libusb/libusb/os)
target_compile_definitions(libusb1.0 PRIVATE HAVE_CONFIG_H=1)
target_compile_options(libusb1.0 PRIVATE -fvisibility=hidden)
set_target_properties(libusb1.0 PROPERTIES POSITION_INDEPENDENT_CODE ON)

# ---------------------------------------------------------------------------
# librtlsdr 0.5.3 (vendore). Patch local : rtlsdr_open_sys_device() pour
# accepter le descripteur fourni par UsbManager.
# ---------------------------------------------------------------------------
add_library(rtlsdr STATIC
    ${SDR_THIRD}/librtlsdr/src/librtlsdr.c
    ${SDR_THIRD}/librtlsdr/src/tuner_e4k.c
    ${SDR_THIRD}/librtlsdr/src/tuner_fc0012.c
    ${SDR_THIRD}/librtlsdr/src/tuner_fc0013.c
    ${SDR_THIRD}/librtlsdr/src/tuner_fc2580.c
    ${SDR_THIRD}/librtlsdr/src/tuner_r82xx.c
)
target_include_directories(rtlsdr PUBLIC ${SDR_THIRD}/librtlsdr/include)
target_link_libraries(rtlsdr PUBLIC libusb1.0)
set_target_properties(rtlsdr PROPERTIES POSITION_INDEPENDENT_CODE ON)

# ---------------------------------------------------------------------------
# Application
# ---------------------------------------------------------------------------
add_library(sdrstudio SHARED
    sdr_jni.cpp
    rtl_android.cpp
    "${SDR_CORE}/dsp.cpp"
    "${SDR_CORE}/source.cpp"
    "${SDR_CORE}/recorder.cpp"
)

target_include_directories(sdrstudio PRIVATE "${SDR_CORE}")
target_compile_options(sdrstudio PRIVATE -O3 -ffast-math)
target_link_libraries(sdrstudio PRIVATE rtlsdr libusb1.0 log)
