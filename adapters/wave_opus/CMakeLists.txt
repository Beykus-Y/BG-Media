find_package(Opus QUIET)
if(NOT Opus_FOUND)
    find_path(OPUS_INCLUDE_DIR opus/opus.h)
    find_library(OPUS_LIBRARY NAMES opus)
    if(OPUS_INCLUDE_DIR AND OPUS_LIBRARY)
        add_library(Opus::opus UNKNOWN IMPORTED)
        set_target_properties(Opus::opus PROPERTIES
            IMPORTED_LOCATION "${OPUS_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${OPUS_INCLUDE_DIR}"
        )
        set(Opus_FOUND TRUE)
    endif()
endif()

if(NOT Opus_FOUND)
    message(FATAL_ERROR "BGMEDIA_BUILD_WAVE_OPUS=ON but libopus was not found.")
endif()

add_library(bgmedia_wave_opus STATIC
    src/OpusWaveCodec.cpp
)
add_library(BGMedia::WaveOpus ALIAS bgmedia_wave_opus)
target_include_directories(bgmedia_wave_opus
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_link_libraries(bgmedia_wave_opus PUBLIC bgmedia_core Opus::opus)
target_compile_features(bgmedia_wave_opus PUBLIC cxx_std_20)
target_compile_options(bgmedia_wave_opus PRIVATE ${BGMEDIA_WARNING_FLAGS})
