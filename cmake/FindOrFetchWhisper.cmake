# Locate whisper.cpp for the optional bgmedia_vox_whisper adapter, fetching the
# pinned release only when no installed package is found and
# BGMEDIA_FETCH_VOX_DEPS is ON. Provides the target `whisper`.
find_package(whisper CONFIG QUIET)

if(NOT TARGET whisper)
    if(NOT BGMEDIA_FETCH_VOX_DEPS)
        message(FATAL_ERROR
            "BGMEDIA_VOX_WHISPER=ON but whisper.cpp was not found. "
            "Install it (find_package(whisper CONFIG)) or enable BGMEDIA_FETCH_VOX_DEPS.")
    endif()

    set(WHISPER_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(WHISPER_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(WHISPER_BUILD_SERVER OFF CACHE BOOL "" FORCE)
    set(WHISPER_CURL OFF CACHE BOOL "" FORCE)
    set(WHISPER_SDL2 OFF CACHE BOOL "" FORCE)
    set(GGML_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(GGML_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    # A distributable build must not be tuned to the build machine's CPU. Hosts
    # that build only for themselves can pass -DGGML_NATIVE=ON.
    set(GGML_NATIVE OFF CACHE BOOL "" FORCE)
    set(BUILD_SHARED_LIBS_SAVED "${BUILD_SHARED_LIBS}")
    set(BUILD_TESTING_SAVED "${BUILD_TESTING}")
    set(BUILD_SHARED_LIBS OFF)
    set(BUILD_TESTING OFF)

    FetchContent_Declare(whisper_cpp
        GIT_REPOSITORY https://github.com/ggml-org/whisper.cpp.git
        GIT_TAG v1.9.5
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(whisper_cpp)
    set(BUILD_SHARED_LIBS "${BUILD_SHARED_LIBS_SAVED}")
    set(BUILD_TESTING "${BUILD_TESTING_SAVED}")
endif()

if(NOT TARGET whisper)
    message(FATAL_ERROR "whisper target is unavailable")
endif()
