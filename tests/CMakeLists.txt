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
