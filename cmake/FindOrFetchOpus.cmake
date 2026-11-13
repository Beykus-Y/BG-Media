# Locate libopus for the optional bgmedia_wave_opus adapter, fetching the
# pinned release only when no system package is available and
# BGMEDIA_FETCH_WAVE_DEPS is ON.
find_package(Opus QUIET)

if(NOT Opus_FOUND)
    find_path(OPUS_INCLUDE_DIR opus/opus.h)
    find_library(OPUS_LIBRARY NAMES opus)
    if(OPUS_INCLUDE_DIR AND OPUS_LIBRARY)
        if(NOT TARGET Opus::opus)
            add_library(Opus::opus UNKNOWN IMPORTED)
            set_target_properties(Opus::opus PROPERTIES
                IMPORTED_LOCATION "${OPUS_LIBRARY}"
                INTERFACE_INCLUDE_DIRECTORIES "${OPUS_INCLUDE_DIR}"
            )
        endif()
        set(Opus_FOUND TRUE)
    endif()
endif()

if(NOT Opus_FOUND)
    if(NOT BGMEDIA_FETCH_WAVE_DEPS)
        message(FATAL_ERROR
            "BGMEDIA_BUILD_WAVE_OPUS=ON but libopus was not found. "
            "Install libopus or enable BGMEDIA_FETCH_WAVE_DEPS.")
    endif()

    set(OPUS_BUILD_TESTING OFF CACHE BOOL "" FORCE)
    set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
    set(OPUS_BUILD_SHARED_LIBRARY OFF CACHE BOOL "" FORCE)
    set(OPUS_INSTALL_PKG_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
    set(OPUS_INSTALL_CMAKE_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
    set(BUILD_TESTING_SAVED "${BUILD_TESTING}")
    set(BUILD_TESTING OFF)

    FetchContent_Declare(opus
        URL https://github.com/xiph/opus/releases/download/v1.5.2/opus-1.5.2.tar.gz
        URL_HASH SHA256=65C1D2F78B9F2FB20082C38CBE47C951AD5839345876E46941612EE87F9A7CE1
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(opus)
    set(BUILD_TESTING "${BUILD_TESTING_SAVED}")
endif()

if(NOT TARGET Opus::opus)
    if(TARGET opus)
        add_library(Opus::opus ALIAS opus)
    else()
        message(FATAL_ERROR "Opus::opus target is unavailable")
    endif()
endif()
