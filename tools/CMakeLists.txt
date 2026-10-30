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

    # Real-track BG Mix transition measurement (offline + optional device).
    add_executable(bgmedia_mix_qa
        mix_qa/main.cpp
    )
    target_link_libraries(bgmedia_mix_qa PRIVATE bgmedia_miniaudio)
    target_compile_options(bgmedia_mix_qa PRIVATE ${BGMEDIA_WARNING_FLAGS})
    if(WIN32)
        target_link_libraries(bgmedia_mix_qa PRIVATE shell32)
    endif()
endif()

add_executable(bgmedia_motion_qa
    motion_qa/main.cpp
)
target_link_libraries(bgmedia_motion_qa PRIVATE bgmedia_rig)
target_compile_options(bgmedia_motion_qa PRIVATE ${BGMEDIA_WARNING_FLAGS})

add_executable(bgmedia_qa_matrix
    qa_matrix/main.cpp
)
target_link_libraries(bgmedia_qa_matrix PRIVATE bgmedia_rig)
target_compile_options(bgmedia_qa_matrix PRIVATE ${BGMEDIA_WARNING_FLAGS})

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
target_include_directories(bgmedia_mesh_clip PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(bgmedia_mesh_clip PRIVATE ${BGMEDIA_WARNING_FLAGS})

if(BGMEDIA_BUILD_MINIAUDIO)
    # Instrumented playback of a model + real track through the Desktop path (QA only).
    add_executable(bgmedia_motion_diag
        motion_diag/main.cpp
    )
    target_link_libraries(bgmedia_motion_diag PRIVATE bgmedia_rig bgmedia_miniaudio)
    target_include_directories(bgmedia_motion_diag PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_options(bgmedia_motion_diag PRIVATE ${BGMEDIA_WARNING_FLAGS})
    if(WIN32)
        target_link_libraries(bgmedia_motion_diag PRIVATE shell32)
    endif()
endif()
