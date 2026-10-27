add_library(bgmedia_test_signals STATIC
    support/SyntheticSignals.cpp
)
target_link_libraries(bgmedia_test_signals PUBLIC bgmedia_core)
target_include_directories(bgmedia_test_signals PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(bgmedia_test_signals PUBLIC cxx_std_20)
target_compile_options(bgmedia_test_signals PRIVATE ${BGMEDIA_WARNING_FLAGS})

add_executable(bgmedia_fixturegen
    fixturegen/main.cpp
)
target_link_libraries(bgmedia_fixturegen PRIVATE bgmedia_test_signals)
target_compile_options(bgmedia_fixturegen PRIVATE ${BGMEDIA_WARNING_FLAGS})
if(WIN32)
    target_link_libraries(bgmedia_fixturegen PRIVATE shell32)
endif()

add_executable(bgmedia_benchmark
    benchmark/main.cpp
)
target_link_libraries(bgmedia_benchmark PRIVATE bgmedia_test_signals)
target_compile_options(bgmedia_benchmark PRIVATE ${BGMEDIA_WARNING_FLAGS})
if(WIN32)
    target_link_libraries(bgmedia_benchmark PRIVATE shell32)
endif()

if(BGMEDIA_BUILD_MINIAUDIO)
    add_executable(bgmedia_analyze
        analyze/main.cpp
    )
    target_link_libraries(bgmedia_analyze PRIVATE bgmedia_miniaudio)
    target_include_directories(bgmedia_analyze PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_options(bgmedia_analyze PRIVATE ${BGMEDIA_WARNING_FLAGS})
    if(WIN32)
        target_link_libraries(bgmedia_analyze PRIVATE shell32)
    endif()
endif()

add_executable(bgmedia_motion_qa
    motion_qa/main.cpp
)
target_link_libraries(bgmedia_motion_qa PRIVATE bgmedia_rig)
target_compile_options(bgmedia_motion_qa PRIVATE ${BGMEDIA_WARNING_FLAGS})

add_executable(bgmedia_procedural_dance
    procedural_dance/main.cpp
)
target_link_libraries(bgmedia_procedural_dance PRIVATE bgmedia_rig)
target_compile_options(bgmedia_procedural_dance PRIVATE ${BGMEDIA_WARNING_FLAGS})

add_executable(bgmedia_sync_qa
    sync_qa/main.cpp
)
target_link_libraries(bgmedia_sync_qa PRIVATE bgmedia_rig bgmedia_test_signals)
target_include_directories(bgmedia_sync_qa PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../tests)
target_compile_options(bgmedia_sync_qa PRIVATE ${BGMEDIA_WARNING_FLAGS})

add_executable(bgmedia_mesh_clip
    mesh_clip/main.cpp
)
target_link_libraries(bgmedia_mesh_clip PRIVATE bgmedia_rig)
target_compile_options(bgmedia_mesh_clip PRIVATE ${BGMEDIA_WARNING_FLAGS})
