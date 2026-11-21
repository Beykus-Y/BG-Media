# This directory is owned by this test under the CMake build tree.
file(REMOVE_RECURSE "${CACHE}")
execute_process(COMMAND "${CMAKE_COMMAND}" -E env
    SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy SDL_RENDER_DRIVER=software
    "${DEMO}" --verify --stress --tok-cache "${CACHE}"
    "${FIXTURES}/1.mp4" "${FIXTURES}/2.mp4" "${FIXTURES}/3.mp4"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors)
message(STATUS "${output}")
if(NOT result EQUAL 0)
    message(FATAL_ERROR "BG Tok integration failed (${result}): ${errors}")
endif()
