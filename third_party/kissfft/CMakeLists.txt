add_library(bgmedia_kissfft OBJECT
    kiss_fft.c
    kiss_fftr.c
)
target_include_directories(bgmedia_kissfft PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(bgmedia_kissfft PRIVATE c_std_99)
set_target_properties(bgmedia_kissfft PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(MSVC)
    target_compile_options(bgmedia_kissfft PRIVATE /W0)
else()
    target_compile_options(bgmedia_kissfft PRIVATE -w)
endif()
