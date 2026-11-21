file(MAKE_DIRECTORY "${OUTPUT}")
foreach(index RANGE 1 3)
    math(EXPR frequency "330 * ${index}")
    math(EXPR hue "70 * ${index}")
    execute_process(COMMAND "${FFMPEG}" -nostdin -v error -y
        -f lavfi -i testsrc2=size=320x180:rate=25
        -f lavfi -i sine=frequency=${frequency}:sample_rate=44100
        -t 2 -vf hue=h=${hue} -filter_threads 1 -c:v libx264 -threads 1 -bf 2 -g 25 -c:a aac -movflags +faststart
        "${OUTPUT}/${index}.mp4" COMMAND_ERROR_IS_FATAL ANY)
endforeach()
file(WRITE "${OUTPUT}/broken.mp4" "not an MP4")
file(WRITE "${OUTPUT}/empty.mp4" "")
