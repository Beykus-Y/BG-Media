# SDL3 is acquired by cmake/FindOrFetchSDL3.cmake at the top level because the
# SDL_GPU 3D adapter and the desktop harness are separate targets.

add_executable(bgmedia_desktop
    main.cpp
    DesktopApp.cpp
    MiniaudioPlayer.cpp
    TextRenderer.cpp
    StbTruetypeImpl.cpp
    VrmScene.cpp
    Ui.cpp
)
target_link_libraries(bgmedia_desktop PRIVATE
    bgmedia_miniaudio
    bgmedia_rig
    bgmedia_sdlgpu
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
endif()
if(WIN32)
    target_link_libraries(bgmedia_desktop PRIVATE shell32 psapi)
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

# Development helper: same importer/retargeter/renderer as the desktop app, but
# renders one frame headlessly to a BMP. It is useful for CI-style visual
# smoke tests without a window.
add_executable(bgmedia_vrm_preview
    ${CMAKE_SOURCE_DIR}/tools/vrm_preview/main.cpp
)
target_link_libraries(bgmedia_vrm_preview PRIVATE
    bgmedia_rig
    bgmedia_sdlgpu
    SDL3::SDL3
)
target_compile_features(bgmedia_vrm_preview PRIVATE cxx_std_20)
target_compile_options(bgmedia_vrm_preview PRIVATE ${BGMEDIA_WARNING_FLAGS})
if(MSVC)
    target_compile_definitions(bgmedia_vrm_preview PRIVATE
        _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN
    )
endif()

# Contact-sheet renderer for dance clips (development / QA tool).
add_executable(bgmedia_motion_sheet
    ${CMAKE_SOURCE_DIR}/tools/motion_sheet/main.cpp
)
target_link_libraries(bgmedia_motion_sheet PRIVATE
    bgmedia_rig
    bgmedia_sdlgpu
    SDL3::SDL3
)
target_compile_features(bgmedia_motion_sheet PRIVATE cxx_std_20)
target_compile_options(bgmedia_motion_sheet PRIVATE ${BGMEDIA_WARNING_FLAGS})
if(MSVC)
    target_compile_definitions(bgmedia_motion_sheet PRIVATE
        _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN
    )
endif()

# CPU-vs-GPU skinning agreement check (silhouette IoU). Development / QA tool.
add_executable(bgmedia_skin_check
    ${CMAKE_SOURCE_DIR}/tools/skin_check/main.cpp
)
target_link_libraries(bgmedia_skin_check PRIVATE
    bgmedia_rig
    bgmedia_sdlgpu
    SDL3::SDL3
)
target_compile_features(bgmedia_skin_check PRIVATE cxx_std_20)
target_compile_options(bgmedia_skin_check PRIVATE ${BGMEDIA_WARNING_FLAGS})
if(MSVC)
    target_compile_definitions(bgmedia_skin_check PRIVATE
        _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN
    )
endif()
