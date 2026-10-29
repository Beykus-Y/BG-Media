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
    test_vrm1.cpp
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
target_include_directories(bgmedia_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/src/motion3)
target_compile_definitions(bgmedia_tests PRIVATE
    BGMEDIA_TEST_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
    BGMEDIA_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)
target_compile_options(bgmedia_tests PRIVATE ${BGMEDIA_WARNING_FLAGS})
if(MSVC)
    target_compile_definitions(bgmedia_tests PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
endif()

foreach(test_name audio analysis cache sync choreography motion timeline warp sync31 vrm vrm1 retarget dance motion32 quality spatial spring intent planner elbow soft_ik triangle alive beat_clock)
    add_test(NAME bgmedia.${test_name} COMMAND bgmedia_tests ${test_name}.)
    set_tests_properties(bgmedia.${test_name} PROPERTIES
        TIMEOUT 120
        LABELS "unit"
    )
endforeach()
