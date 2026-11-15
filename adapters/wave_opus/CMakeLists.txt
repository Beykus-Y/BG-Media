include(${PROJECT_SOURCE_DIR}/cmake/FindOrFetchOpus.cmake)

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
