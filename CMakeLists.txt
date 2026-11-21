cmake_minimum_required(VERSION 3.24)

project(BGMedia
    VERSION 0.3.2
    DESCRIPTION "BG Media - music analysis and choreography synchronization engine"
    LANGUAGES C CXX
)

# ---------------------------------------------------------------------------
# Build options. The core is dependency-free and is always buildable on its
# own; adapters and the desktop harness are optional.
# ---------------------------------------------------------------------------
option(BGMEDIA_BUILD_VIDEO          "Build the optional BG Video feed component" OFF)
option(BGMEDIA_BUILD_VIDEO_FFMPEG   "Build the optional FFmpeg video adapter" OFF)
option(BGMEDIA_BUILD_VIDEO_DEMO     "Build the SDL3 video player demo" OFF)
option(BGMEDIA_BUILD_TESTS          "Build unit and integration tests" ON)
option(BGMEDIA_BUILD_TOOLS          "Build command line tools" ON)
option(BGMEDIA_BUILD_MINIAUDIO      "Build the miniaudio decoding adapter" ON)
option(BGMEDIA_BUILD_WAVE_OPUS      "Build the optional libopus BG Wave codec adapter" OFF)
option(BGMEDIA_BUILD_WAVE_WEBRTC    "Build the optional libdatachannel BG Wave P2P transport adapter" OFF)
option(BGMEDIA_FETCH_WAVE_DEPS      "Download libopus / libdatachannel with FetchContent when not installed" ON)
option(BGMEDIA_BUILD_DESKTOP        "Build the SDL3 desktop test application" ON)
option(BGMEDIA_FETCH_SDL3           "Download SDL3 with FetchContent when not installed" ON)
option(BGMEDIA_WARNINGS_AS_ERRORS   "Treat compiler warnings as errors" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

include(GNUInstallDirs)
include(FetchContent)
include(CTest)

# Keep all binaries in one place for easy running on Windows and Linux.
set(BGMEDIA_RUNTIME_DIR "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${BGMEDIA_RUNTIME_DIR}")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG "${BGMEDIA_RUNTIME_DIR}")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE "${BGMEDIA_RUNTIME_DIR}")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${BGMEDIA_RUNTIME_DIR}")

set(BGMEDIA_WARNING_FLAGS
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive- /Zc:__cplusplus /utf-8>
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Wshadow>
)
if(BGMEDIA_WARNINGS_AS_ERRORS)
    list(APPEND BGMEDIA_WARNING_FLAGS $<$<CXX_COMPILER_ID:MSVC>:/WX> $<$<CXX_COMPILER_ID:GNU,Clang>:-Werror>)
endif()

# ---------------------------------------------------------------------------
# third_party: KissFFT (BSD-3-Clause). It is compiled as C and kept private to
# the core, so consumers of BG Media do not inherit an FFT dependency.
# ---------------------------------------------------------------------------
add_subdirectory(third_party/kissfft)

# ---------------------------------------------------------------------------
# Core library: pure C++20, no SDL, no miniaudio, no platform audio output.
# ---------------------------------------------------------------------------
add_library(bgmedia_core STATIC
    src/audio/AudioData.cpp
    src/audio/WavDecoder.cpp
    src/analysis/Analyzer.cpp
    src/analysis/SpectralAnalysis.cpp
    src/analysis/TempoEstimator.cpp
    src/analysis/BeatTracker.cpp
    src/analysis/EventDetection.cpp
    src/analysis/AnalysisConfig.cpp
    src/cache/AnalysisCache.cpp
    src/sync/PlaybackClock.cpp
    src/choreography/Choreography.cpp
    src/mix/MixTypes.cpp
    src/mix/PcmTrack.cpp
    src/mix/MixFeatureMap.cpp
    src/mix/MixFeatureCache.cpp
    src/mix/VocalDetector.cpp
    src/mix/HarmonicAnalyzer.cpp
    src/mix/TimeStretch.cpp
    src/mix/SpectralEq.cpp
    src/mix/MixPlanner.cpp
    src/mix/MixEngine.cpp
    src/mix/MixSession.cpp
    src/mix/MixSync.cpp
    src/wave/WaveRingBuffer.cpp
    src/wave/WaveResampler.cpp
    src/wave/WaveCodec.cpp
    src/wave/WaveJitterBuffer.cpp
    src/wave/WaveSignaling.cpp
    src/wave/WaveTransport.cpp
    src/wave/WaveAudioPipeline.cpp
)
add_library(BGMedia::Core ALIAS bgmedia_core)
target_include_directories(bgmedia_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_features(bgmedia_core PUBLIC cxx_std_20)
target_compile_options(bgmedia_core PRIVATE ${BGMEDIA_WARNING_FLAGS})
target_include_directories(bgmedia_core PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third_party/kissfft)
target_sources(bgmedia_core PRIVATE $<TARGET_OBJECTS:bgmedia_kissfft>)
find_package(Threads REQUIRED)
target_link_libraries(bgmedia_core PUBLIC Threads::Threads)
if(MSVC)
    target_compile_definitions(bgmedia_core PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
if(WIN32)
    target_link_libraries(bgmedia_core PUBLIC bcrypt)
endif()

# ---------------------------------------------------------------------------
# VRM 0.x importer and humanoid retargeter. Both are pure C++20: no SDL, no
# GPU API, no window. The desktop renderer is the only consumer of SDL_GPU.
# ---------------------------------------------------------------------------
add_library(bgmedia_vrm STATIC
    src/vrm/VrmModel.cpp
    src/vrm/VrmLoader.cpp
    src/vrm/ThighColliders.cpp
    src/vrm/CgltfImpl.cpp
    src/vrm/StbImageImpl.cpp
)
add_library(BGMedia::Vrm ALIAS bgmedia_vrm)
target_include_directories(bgmedia_vrm
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/cgltf
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/stb
)
target_compile_features(bgmedia_vrm PUBLIC cxx_std_20)
target_compile_options(bgmedia_vrm PRIVATE ${BGMEDIA_WARNING_FLAGS})
if(MSVC)
    target_compile_definitions(bgmedia_vrm PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
endif()

add_library(bgmedia_rig STATIC
    src/animation/HumanoidRetargeter.cpp
    src/animation/TwoBoneSolver.cpp
    src/animation/SecondaryDynamics.cpp
    src/motion3/BeatClock.cpp
    src/motion3/MusicalTimeline.cpp
    src/motion3/MusicalTimeWarp.cpp
    src/motion3/DanceClip.cpp
    src/motion3/AliveLayer.cpp
    src/motion3/ClipFootwork.cpp
    src/motion3/MotionPrimitive.cpp
    src/motion3/ProceduralChoreography.cpp
    src/motion3/DanceDirector.cpp
    src/motion3/DancePlayer.cpp
    src/motion3/TaskSpaceIntent.cpp
    src/motion3/WristPlanner.cpp
    src/motion3/ElbowSolver.cpp
)
add_library(BGMedia::Rig ALIAS bgmedia_rig)
target_include_directories(bgmedia_rig
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/motion3
)
target_link_libraries(bgmedia_rig PUBLIC bgmedia_core bgmedia_vrm)
target_compile_features(bgmedia_rig PUBLIC cxx_std_20)
target_compile_options(bgmedia_rig PRIVATE ${BGMEDIA_WARNING_FLAGS})

if(BGMEDIA_BUILD_VIDEO)
    add_library(bgmedia_video STATIC src/video/Feed.cpp src/video/Playback.cpp)
    add_library(BGMedia::Video ALIAS bgmedia_video)
    target_include_directories(bgmedia_video PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
    target_compile_features(bgmedia_video PUBLIC cxx_std_20)
    target_compile_options(bgmedia_video PRIVATE ${BGMEDIA_WARNING_FLAGS})
    target_link_libraries(bgmedia_video PUBLIC Threads::Threads)
    install(TARGETS bgmedia_video EXPORT BGMediaTargets ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
endif()

if(BGMEDIA_BUILD_VIDEO_FFMPEG)
    if(NOT BGMEDIA_BUILD_VIDEO)
        message(FATAL_ERROR "BGMEDIA_BUILD_VIDEO_FFMPEG requires BGMEDIA_BUILD_VIDEO")
    endif()
    add_subdirectory(adapters/video_ffmpeg)
endif()

# ---------------------------------------------------------------------------
# Adapters: miniaudio decoding only. Playback belongs to the host application,
# which keeps the engine independent from any particular audio output API.
# ---------------------------------------------------------------------------
if(BGMEDIA_BUILD_MINIAUDIO)
    add_subdirectory(adapters/miniaudio)
endif()

if(BGMEDIA_BUILD_WAVE_OPUS)
    add_subdirectory(adapters/wave_opus)
endif()

if(BGMEDIA_BUILD_WAVE_WEBRTC)
    add_subdirectory(adapters/wave_webrtc)
endif()

# ---------------------------------------------------------------------------
# Deterministic synthetic signals shared by the tools and the unit tests. This
# target must exist whenever the tests are built too, otherwise an ASan or
# core-only configuration (BGMEDIA_BUILD_TOOLS=OFF) cannot compile the tests
# that include "support/SyntheticSignals.hpp".
# ---------------------------------------------------------------------------
if(BGMEDIA_BUILD_TOOLS OR (BGMEDIA_BUILD_TESTS AND BUILD_TESTING))
    add_library(bgmedia_test_signals STATIC
        ${CMAKE_CURRENT_SOURCE_DIR}/tools/support/SyntheticSignals.cpp
    )
    target_link_libraries(bgmedia_test_signals PUBLIC bgmedia_core)
    target_include_directories(bgmedia_test_signals PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/tools)
    target_compile_features(bgmedia_test_signals PUBLIC cxx_std_20)
    target_compile_options(bgmedia_test_signals PRIVATE ${BGMEDIA_WARNING_FLAGS})
endif()

# ---------------------------------------------------------------------------
# Tools and tests.
# ---------------------------------------------------------------------------
if(BGMEDIA_BUILD_TOOLS)
    add_subdirectory(tools)
endif()


# ---------------------------------------------------------------------------
# Desktop harness: SDL3 UI + miniaudio playback. This target is the only place
# that knows about a window, a mouse or a sound card.
# ---------------------------------------------------------------------------
if(BGMEDIA_BUILD_DESKTOP OR BGMEDIA_BUILD_VIDEO_DEMO)
    include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/FindOrFetchSDL3.cmake)
endif()
if(BGMEDIA_BUILD_VIDEO_DEMO)
    if(NOT BGMEDIA_BUILD_VIDEO_FFMPEG)
        message(FATAL_ERROR "BGMEDIA_BUILD_VIDEO_DEMO requires BGMEDIA_BUILD_VIDEO_FFMPEG")
    endif()
    add_subdirectory(apps/video_demo)
endif()
if(BGMEDIA_BUILD_DESKTOP)
    if(NOT BGMEDIA_BUILD_MINIAUDIO)
        message(FATAL_ERROR "BGMEDIA_BUILD_DESKTOP requires BGMEDIA_BUILD_MINIAUDIO")
    endif()
    add_subdirectory(adapters/sdlgpu)
    add_subdirectory(apps/desktop)
endif()

if(BGMEDIA_BUILD_TESTS AND BUILD_TESTING)
    add_subdirectory(tests)
endif()

# ---------------------------------------------------------------------------
# Installation of the reusable core.
# ---------------------------------------------------------------------------
install(TARGETS bgmedia_core bgmedia_vrm bgmedia_rig EXPORT BGMediaTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
install(DIRECTORY include/bgmedia DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT BGMediaTargets
    FILE BGMediaTargets.cmake
    NAMESPACE BGMedia::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/BGMedia
)
