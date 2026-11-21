add_executable(bgmedia_video_demo Main.cpp)
target_link_libraries(bgmedia_video_demo PRIVATE BGMedia::VideoFFmpeg SDL3::SDL3)
target_compile_options(bgmedia_video_demo PRIVATE ${BGMEDIA_WARNING_FLAGS})
