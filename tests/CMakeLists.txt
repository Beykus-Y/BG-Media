add_executable(bgmedia_tests
    test_main.cpp
    test_audio.cpp
    test_analysis.cpp
    test_cache.cpp
    test_sync.cpp
    test_choreography.cpp
    test_motion.cpp
)
target_link_libraries(bgmedia_tests PRIVATE
    bgmedia_core
    bgmedia_test_signals
)
if(TARGET bgmedia_miniaudio)
    target_link_libraries(bgmedia_tests PRIVATE bgmedia_miniaudio)
    target_compile_definitions(bgmedia_tests PRIVATE BGMEDIA_HAVE_MINIAUDIO=1)
endif()
target_include_directories(bgmedia_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(bgmedia_tests PRIVATE
    BGMEDIA_TEST_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
)
target_compile_options(bgmedia_tests PRIVATE ${BGMEDIA_WARNING_FLAGS})

foreach(test_name audio analysis cache sync choreography motion)
    add_test(NAME bgmedia.${test_name} COMMAND bgmedia_tests ${test_name}.)
    set_tests_properties(bgmedia.${test_name} PROPERTIES
        TIMEOUT 120
        LABELS "unit"
    )
endforeach()
