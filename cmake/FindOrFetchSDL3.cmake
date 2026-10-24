# Locate SDL3 for the optional desktop/3D targets, fetching the pinned release
# only when no system package is available. The core library and its tests do
# not include this file.
find_package(SDL3 3.2 CONFIG QUIET)

if(NOT SDL3_FOUND)
    if(NOT BGMEDIA_FETCH_SDL3)
        message(FATAL_ERROR
            "SDL3 was not found. Install SDL3 (SDL3_DIR) or enable BGMEDIA_FETCH_SDL3.")
    endif()

    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
    set(SDL_TEST OFF CACHE BOOL "" FORCE)
    set(SDL_TESTS OFF CACHE BOOL "" FORCE)
    set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
    set(SDL_AUDIO OFF CACHE BOOL "" FORCE)
    set(SDL_JOYSTICK OFF CACHE BOOL "" FORCE)
    set(SDL_HAPTIC OFF CACHE BOOL "" FORCE)
    set(SDL_SENSOR OFF CACHE BOOL "" FORCE)
    set(SDL_CAMERA OFF CACHE BOOL "" FORCE)
    set(SDL_GPU ON CACHE BOOL "" FORCE)
    set(SDL_WERROR OFF CACHE BOOL "" FORCE)

    FetchContent_Declare(SDL3
        URL https://www.libsdl.org/release/SDL3-3.4.18.tar.gz
        URL_HASH SHA256=9C75CF16330322C217DEDD2E0609F1124F1B54B8633E763467B4684D0F4334A3
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(SDL3)
endif()

if(NOT TARGET SDL3::SDL3)
    message(FATAL_ERROR "SDL3 target is unavailable")
endif()
