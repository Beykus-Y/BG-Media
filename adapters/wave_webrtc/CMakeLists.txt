include(${PROJECT_SOURCE_DIR}/cmake/FindOrFetchLibDataChannel.cmake)

add_library(bgmedia_wave_webrtc STATIC
    src/WebRtcWaveTransport.cpp
)
add_library(BGMedia::WaveWebRtc ALIAS bgmedia_wave_webrtc)
target_include_directories(bgmedia_wave_webrtc
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_link_libraries(bgmedia_wave_webrtc PUBLIC bgmedia_core ${BGMEDIA_LIBDATACHANNEL_TARGET})
target_compile_features(bgmedia_wave_webrtc PUBLIC cxx_std_20)
target_compile_options(bgmedia_wave_webrtc PRIVATE ${BGMEDIA_WARNING_FLAGS})
