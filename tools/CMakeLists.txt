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

    # Batch listening-test renderer: real BG Beat + planner + engine, WAV out.
    add_executable(bgmedia_mix_listening
        mix_listening/main.cpp
    )
    target_link_libraries(bgmedia_mix_listening PRIVATE bgmedia_miniaudio)
    target_compile_options(bgmedia_mix_listening PRIVATE ${BGMEDIA_WARNING_FLAGS})
    if(WIN32)
        target_link_libraries(bgmedia_mix_listening PRIVATE shell32)
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

add_executable(bgmedia_wave_spike
    wave_spike/main.cpp
)
target_link_libraries(bgmedia_wave_spike PRIVATE bgmedia_core)
target_compile_options(bgmedia_wave_spike PRIVATE ${BGMEDIA_WARNING_FLAGS})

if(BGMEDIA_BUILD_WAVE_WEBRTC)
    add_executable(bgmedia_wave_p2p_peer
        wave_p2p_peer/main.cpp
    )
    target_link_libraries(bgmedia_wave_p2p_peer PRIVATE bgmedia_wave_webrtc)
    if(BGMEDIA_BUILD_WAVE_OPUS)
        target_link_libraries(bgmedia_wave_p2p_peer PRIVATE bgmedia_wave_opus)
        target_compile_definitions(bgmedia_wave_p2p_peer PRIVATE BGMEDIA_HAVE_WAVE_OPUS=1)
    endif()
    target_compile_options(bgmedia_wave_p2p_peer PRIVATE ${BGMEDIA_WARNING_FLAGS})
endif()

if(TARGET bgmedia_vox_whisper)
    add_executable(bgmedia_vox_transcribe
        vox_transcribe/main.cpp
    )
    target_link_libraries(bgmedia_vox_transcribe PRIVATE bgmedia_vox_whisper)
    target_include_directories(bgmedia_vox_transcribe PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_options(bgmedia_vox_transcribe PRIVATE ${BGMEDIA_WARNING_FLAGS})
    if(MSVC)
        target_compile_definitions(bgmedia_vox_transcribe PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
    endif()
endif()

if(TARGET bgmedia_vox)
    add_executable(bgmedia_vox_enhance
        vox_enhance/main.cpp
    )
    target_link_libraries(bgmedia_vox_enhance PRIVATE bgmedia_vox)
    target_include_directories(bgmedia_vox_enhance PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_options(bgmedia_vox_enhance PRIVATE ${BGMEDIA_WARNING_FLAGS})
    if(MSVC)
        target_compile_definitions(bgmedia_vox_enhance PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
    endif()
endif()
