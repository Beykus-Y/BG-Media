include(${PROJECT_SOURCE_DIR}/cmake/FindOrFetchWhisper.cmake)

add_library(bgmedia_vox_whisper STATIC
    src/VoxWhisper.cpp
    src/VoxWhisperCapi.cpp
)
add_library(BGMedia::VoxWhisper ALIAS bgmedia_vox_whisper)
target_include_directories(bgmedia_vox_whisper
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
        ${PROJECT_SOURCE_DIR}/src/vox
)
target_link_libraries(bgmedia_vox_whisper PUBLIC bgmedia_vox PRIVATE whisper)
# Treat the upstream whisper/ggml headers as system headers so that strict
# warning flags (/W4 /WX, -Werror) do not fail on code we do not own.
foreach(_bgm_dep whisper ggml)
    if(TARGET ${_bgm_dep})
        get_target_property(_bgm_inc ${_bgm_dep} INTERFACE_INCLUDE_DIRECTORIES)
        if(_bgm_inc)
            target_include_directories(bgmedia_vox_whisper SYSTEM PRIVATE ${_bgm_inc})
        endif()
    endif()
endforeach()
target_compile_features(bgmedia_vox_whisper PUBLIC cxx_std_20)
target_compile_options(bgmedia_vox_whisper PRIVATE ${BGMEDIA_WARNING_FLAGS})
if(MSVC)
    target_compile_definitions(bgmedia_vox_whisper PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
endif()
