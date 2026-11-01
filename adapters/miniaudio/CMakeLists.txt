add_library(bgmedia_miniaudio STATIC
    src/MiniaudioDecoder.cpp
    src/MiniaudioImpl.cpp
    src/MiniaudioMixDevice.cpp
    src/MiniaudioMixLoader.cpp
)
add_library(BGMedia::Miniaudio ALIAS bgmedia_miniaudio)
target_include_directories(bgmedia_miniaudio
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../..
        ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/miniaudio
)
target_link_libraries(bgmedia_miniaudio PUBLIC bgmedia_core)
target_compile_features(bgmedia_miniaudio PUBLIC cxx_std_20)
target_compile_options(bgmedia_miniaudio PRIVATE ${BGMEDIA_WARNING_FLAGS})

# miniaudio's backends on Linux need pthread and the dynamic loader.
if(UNIX AND NOT APPLE)
    find_package(Threads REQUIRED)
    target_link_libraries(bgmedia_miniaudio PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
endif()

install(TARGETS bgmedia_miniaudio EXPORT BGMediaTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
install(DIRECTORY include/bgmedia DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
