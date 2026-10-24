# SDL3 is only needed by the desktop harness. The engine and its tests build
# without it. When no system SDL3 is available, CMake downloads the pinned
# release. The application itself never performs network requests.
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
    set(SDL_GPU OFF CACHE BOOL "" FORCE)
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

add_executable(bgmedia_desktop
    main.cpp
    DesktopApp.cpp
    MiniaudioPlayer.cpp
    TextRenderer.cpp
    StbTruetypeImpl.cpp
    Ui.cpp
)
target_link_libraries(bgmedia_desktop PRIVATE
    bgmedia_miniaudio
    SDL3::SDL3
)
target_include_directories(bgmedia_desktop PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/third_party/stb
    ${CMAKE_SOURCE_DIR}/third_party/miniaudio
)
target_compile_features(bgmedia_desktop PRIVATE cxx_std_20)
target_compile_options(bgmedia_desktop PRIVATE ${BGMEDIA_WARNING_FLAGS})
if(MSVC)
    target_compile_definitions(bgmedia_desktop PRIVATE
        _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN
    )
    target_link_libraries(bgmedia_desktop PRIVATE shell32)
endif()

if(UNIX AND NOT APPLE)
    find_package(Threads REQUIRED)
    target_link_libraries(bgmedia_desktop PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
endif()

# The font assets must sit next to the executable (or in a parent directory).
add_custom_command(TARGET bgmedia_desktop POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/assets
        $<TARGET_FILE_DIR:bgmedia_desktop>/assets
    COMMENT "Copying BG Media UI assets"
)

install(TARGETS bgmedia_desktop RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY ${CMAKE_SOURCE_DIR}/assets DESTINATION ${CMAKE_INSTALL_BINDIR})
