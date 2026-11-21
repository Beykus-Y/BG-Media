find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
    libavformat>=59 libavcodec>=59 libavutil>=57 libswscale libswresample>=4)
add_library(bgmedia_video_ffmpeg STATIC FFmpeg.cpp)
add_library(BGMedia::VideoFFmpeg ALIAS bgmedia_video_ffmpeg)
target_link_libraries(bgmedia_video_ffmpeg PUBLIC bgmedia_video PRIVATE PkgConfig::FFMPEG)
target_compile_options(bgmedia_video_ffmpeg PRIVATE ${BGMEDIA_WARNING_FLAGS})
