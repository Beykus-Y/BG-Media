add_executable(bgmedia_video_demo Main.cpp)
target_link_libraries(bgmedia_video_demo PRIVATE BGMedia::VideoFFmpeg SDL3::SDL3)
target_compile_options(bgmedia_video_demo PRIVATE ${BGMEDIA_WARNING_FLAGS})

set(BGMEDIA_TOK_FIXTURE_ROOT "" CACHE PATH "BG Tok checkout for the optional Linux fixture demo")
set(BGMEDIA_TOK_FIXTURE_LIB "" CACHE FILEPATH "Prebuilt bg-tok-fixture-ffi static library")
if(BGMEDIA_TOK_FIXTURE_ROOT OR BGMEDIA_TOK_FIXTURE_LIB)
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR NOT TARGET bgmedia_video_cache OR
       NOT EXISTS "${BGMEDIA_TOK_FIXTURE_ROOT}/crates/bg-tok-fixture-ffi/include/bg_tok_fixture.h" OR
       NOT EXISTS "${BGMEDIA_TOK_FIXTURE_LIB}")
        message(FATAL_ERROR "BG Tok demo requires Linux, VIDEO_CACHE, fixture headers and a prebuilt fixture static library")
    endif()
    target_include_directories(bgmedia_video_demo PRIVATE "${BGMEDIA_TOK_FIXTURE_ROOT}/crates/bg-tok-fixture-ffi/include")
    target_compile_definitions(bgmedia_video_demo PRIVATE BGMEDIA_TOK_FIXTURE=1)
    target_link_libraries(bgmedia_video_demo PRIVATE BGMedia::VideoCache "${BGMEDIA_TOK_FIXTURE_LIB}" ${CMAKE_DL_LIBS} m)
endif()
