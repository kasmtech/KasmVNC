option(KASMVNC_FFMPEG_ADAPTER_PROBE "Build the ffmpeg-adapter-probe test executable" OFF)

if (NOT KASMVNC_FFMPEG_ADAPTER_PROBE)
    return()
endif ()

add_executable(ffmpeg-adapter-probe adapter_probe.cxx)
target_include_directories(ffmpeg-adapter-probe PRIVATE ${CMAKE_SOURCE_DIR}/common)
target_link_libraries(ffmpeg-adapter-probe PRIVATE rfb ${CMAKE_DL_LIBS})
set_target_properties(ffmpeg-adapter-probe PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
