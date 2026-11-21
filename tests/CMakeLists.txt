add_executable(bgmedia_tests
    test_main.cpp
    test_audio.cpp
    test_analysis.cpp
    test_cache.cpp
    test_sync.cpp
    test_choreography.cpp
    test_motion.cpp
    test_timeline.cpp
    test_warp.cpp
    test_musical_sync.cpp
    test_vrm.cpp
    test_retarget.cpp
    test_dance.cpp
    test_motion32.cpp
    test_quality.cpp
    test_spatial.cpp
    test_spring.cpp
    test_intent.cpp
    test_wrist_planner.cpp
    test_elbow_solver.cpp
    test_soft_ik.cpp
    test_triangle.cpp
    test_alive.cpp
    test_beat_clock.cpp
    test_mix.cpp
    test_ui.cpp
    test_wave.cpp
    test_wave_real.cpp
)
target_link_libraries(bgmedia_tests PRIVATE
    bgmedia_core
    bgmedia_rig
    bgmedia_test_signals
)
if(TARGET bgmedia_miniaudio)
    target_link_libraries(bgmedia_tests PRIVATE bgmedia_miniaudio)
    target_compile_definitions(bgmedia_tests PRIVATE BGMEDIA_HAVE_MINIAUDIO=1)
endif()
if(TARGET bgmedia_wave_opus)
    target_link_libraries(bgmedia_tests PRIVATE bgmedia_wave_opus)
    target_compile_definitions(bgmedia_tests PRIVATE BGMEDIA_HAVE_WAVE_OPUS=1)
endif()
if(TARGET bgmedia_wave_webrtc)
    target_link_libraries(bgmedia_tests PRIVATE bgmedia_wave_webrtc)
    target_compile_definitions(bgmedia_tests PRIVATE BGMEDIA_HAVE_WAVE_WEBRTC=1)
    if(TARGET bgmedia_wave_p2p_peer)
        add_dependencies(bgmedia_tests bgmedia_wave_p2p_peer)
        target_compile_definitions(bgmedia_tests PRIVATE
            BGMEDIA_WAVE_P2P_PEER_EXE="$<TARGET_FILE:bgmedia_wave_p2p_peer>")
    endif()
endif()
target_include_directories(bgmedia_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/src/motion3 ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/apps/desktop)
target_compile_definitions(bgmedia_tests PRIVATE
    BGMEDIA_TEST_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
    BGMEDIA_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)
target_compile_options(bgmedia_tests PRIVATE ${BGMEDIA_WARNING_FLAGS})
if(MSVC)
    target_compile_definitions(bgmedia_tests PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
endif()

foreach(test_name audio analysis cache sync choreography motion timeline warp sync31 vrm retarget dance motion32 quality spatial spring intent planner elbow soft_ik triangle alive beat_clock mix ui wave)
    add_test(NAME bgmedia.${test_name} COMMAND bgmedia_tests ${test_name}.)
    set_tests_properties(bgmedia.${test_name} PROPERTIES
        TIMEOUT 120
        LABELS "unit"
    )
endforeach()

if(TARGET bgmedia_wave_opus OR TARGET bgmedia_wave_webrtc)
    add_test(NAME bgmedia.wave_real COMMAND bgmedia_tests wave_real.)
    set_tests_properties(bgmedia.wave_real PROPERTIES
        TIMEOUT 120
        LABELS "integration"
    )
endif()

if(TARGET bgmedia_video)
    add_executable(bgmedia_video_tests test_video_feed.cpp)
    target_link_libraries(bgmedia_video_tests PRIVATE bgmedia_video)
    target_compile_options(bgmedia_video_tests PRIVATE ${BGMEDIA_WARNING_FLAGS})
    add_test(NAME bgmedia.video_feed COMMAND bgmedia_video_tests)
    set_tests_properties(bgmedia.video_feed PROPERTIES TIMEOUT 30 LABELS unit)
endif()

if(TARGET bgmedia_video_ffmpeg)
    find_program(BGMEDIA_FFMPEG_EXECUTABLE ffmpeg)
    find_program(BGMEDIA_FFPROBE_EXECUTABLE ffprobe)
    if(BGMEDIA_FFMPEG_EXECUTABLE AND BGMEDIA_FFPROBE_EXECUTABLE)
        add_executable(bgmedia_video_ffmpeg_tests test_video_ffmpeg.cpp)
        target_link_libraries(bgmedia_video_ffmpeg_tests PRIVATE BGMedia::VideoFFmpeg)
        target_compile_options(bgmedia_video_ffmpeg_tests PRIVATE ${BGMEDIA_WARNING_FLAGS})
        set(video_fixtures "${CMAKE_CURRENT_BINARY_DIR}/video-fixtures")
        add_test(NAME bgmedia.video_fixtures COMMAND "${CMAKE_COMMAND}"
            "-DFFMPEG=${BGMEDIA_FFMPEG_EXECUTABLE}" "-DOUTPUT=${video_fixtures}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/VideoFixtures.cmake")
        set_tests_properties(bgmedia.video_fixtures PROPERTIES FIXTURES_SETUP video_mp4 TIMEOUT 30)
        add_test(NAME bgmedia.video_mp4 COMMAND bgmedia_video_ffmpeg_tests "${video_fixtures}")
        set_tests_properties(bgmedia.video_mp4 PROPERTIES FIXTURES_REQUIRED video_mp4 TIMEOUT 15)
        add_test(NAME bgmedia.video_packet_timestamps COMMAND "${BGMEDIA_FFPROBE_EXECUTABLE}"
            -v error -select_streams v:0 -show_entries packet=pts_time,dts_time
            -of csv=p=0 "${video_fixtures}/1.mp4")
        set_tests_properties(bgmedia.video_packet_timestamps PROPERTIES FIXTURES_REQUIRED video_mp4
            PASS_REGULAR_EXPRESSION "0\\.000000,-0\\.080000" TIMEOUT 10)
        if(TARGET bgmedia_video_demo)
            add_test(NAME bgmedia.video_demo COMMAND bgmedia_video_demo --verify --stress
                "${video_fixtures}/1.mp4" "${video_fixtures}/2.mp4" "${video_fixtures}/3.mp4")
            set_tests_properties(bgmedia.video_demo PROPERTIES FIXTURES_REQUIRED video_mp4 TIMEOUT 20
                ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_AUDIODRIVER=dummy;SDL_RENDER_DRIVER=software")
        endif()
    else()
        message(STATUS "Video fixtures require ffmpeg (libx264/AAC) and ffprobe executables")
    endif()
endif()
