# SDL_GPU 3D adapter. It is a separate target so the importer/retargeter can be
# built and tested without SDL, and so another backend can replace this one
# without touching BG Beat, BG Sync, BG Motion or the importer.

add_library(bgmedia_sdlgpu STATIC
    src/SdlGpuVrmRenderer.cpp
)
add_library(BGMedia::SdlGpu ALIAS bgmedia_sdlgpu)

target_include_directories(bgmedia_sdlgpu
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(bgmedia_sdlgpu
    PUBLIC
        bgmedia_vrm
        bgmedia_rig
    PRIVATE
        SDL3::SDL3
)
target_compile_features(bgmedia_sdlgpu PUBLIC cxx_std_20)
target_compile_options(bgmedia_sdlgpu PRIVATE ${BGMEDIA_WARNING_FLAGS})
if(MSVC)
    target_compile_definitions(bgmedia_sdlgpu PRIVATE
        _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN
    )
endif()
