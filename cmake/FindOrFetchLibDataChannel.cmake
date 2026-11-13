# Locate libdatachannel for the optional bgmedia_wave_webrtc adapter, fetching
# the pinned release only when no system package is available and
# BGMEDIA_FETCH_WAVE_DEPS is ON.
if(NOT EXISTS "${CMAKE_BINARY_DIR}/_deps/libdatachannel-src/CMakeLists.txt" AND
   NOT TARGET datachannel-static AND NOT TARGET datachannel AND
   NOT TARGET LibDataChannel::LibDataChannelStatic AND NOT TARGET LibDataChannel::LibDataChannel)
    find_package(LibDataChannel QUIET)
endif()

if(NOT LibDataChannel_FOUND)
    if(NOT BGMEDIA_FETCH_WAVE_DEPS)
        message(FATAL_ERROR
            "BGMEDIA_BUILD_WAVE_WEBRTC=ON but libdatachannel was not found. "
            "Install libdatachannel or enable BGMEDIA_FETCH_WAVE_DEPS.")
    endif()

    set(NO_WEBSOCKET ON CACHE BOOL "" FORCE)
    set(NO_EXAMPLES ON CACHE BOOL "" FORCE)
    set(NO_TESTS ON CACHE BOOL "" FORCE)
    set(NO_MEDIA OFF CACHE BOOL "" FORCE)
    set(USE_NICE OFF CACHE BOOL "" FORCE)
    set(USE_GNUTLS OFF CACHE BOOL "" FORCE)
    set(USE_MBEDTLS OFF CACHE BOOL "" FORCE)
    set(TEST_APPS OFF CACHE BOOL "" FORCE)
    set(WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)
    set(BUILD_SHARED_LIBS_SAVED "${BUILD_SHARED_LIBS}")
    set(BUILD_SHARED_LIBS OFF)
    set(BUILD_TESTING_SAVED "${BUILD_TESTING}")
    set(BUILD_TESTING OFF)

    FetchContent_Declare(libdatachannel
        GIT_REPOSITORY https://github.com/paullouisageneau/libdatachannel.git
        GIT_TAG v0.22.5
        GIT_SHALLOW TRUE
        GIT_SUBMODULES "deps/libjuice;deps/libsrtp;deps/usrsctp;deps/plog"
    )
    FetchContent_MakeAvailable(libdatachannel)
    set(BUILD_SHARED_LIBS "${BUILD_SHARED_LIBS_SAVED}")
    set(BUILD_TESTING "${BUILD_TESTING_SAVED}")
endif()

if(TARGET LibDataChannel::LibDataChannelStatic)
    set(BGMEDIA_LIBDATACHANNEL_TARGET LibDataChannel::LibDataChannelStatic)
elseif(TARGET LibDataChannel::LibDataChannel)
    set(BGMEDIA_LIBDATACHANNEL_TARGET LibDataChannel::LibDataChannel)
elseif(TARGET datachannel-static)
    set(BGMEDIA_LIBDATACHANNEL_TARGET datachannel-static)
elseif(TARGET datachannel)
    set(BGMEDIA_LIBDATACHANNEL_TARGET datachannel)
else()
    message(FATAL_ERROR "LibDataChannel target is unavailable")
endif()
