# FFmpeg runtime adapter check: links librfb directly, so no Xorg build and no
# installation is required. Kept separate from tests/CMakeLists.txt, which is
# gated by TESTS and still references the removed native viewer.
#
#   cmake -DKASMVNC_FFMPEG_ADAPTER_PROBE=ON . && make ffmpeg-adapter-probe
#   ./ffmpeg-adapter-probe <output-prefix> <expected-major> [libx264|libx265]
#   ./ffmpeg-adapter-probe --check-hardware-options <expected-major>
#   ./ffmpeg-adapter-probe --expect-unavailable
option(KASMVNC_FFMPEG_ADAPTER_PROBE "Build the ffmpeg-adapter-probe test executable" OFF)

if (NOT KASMVNC_FFMPEG_ADAPTER_PROBE)
    return()
endif ()

add_executable(ffmpeg-adapter-probe adapter_probe.cxx)
target_include_directories(ffmpeg-adapter-probe PRIVATE ${CMAKE_SOURCE_DIR}/common)
target_link_libraries(ffmpeg-adapter-probe PRIVATE rfb ${CMAKE_DL_LIBS})
# Keep the binary where it was produced before (the build root).
set_target_properties(ffmpeg-adapter-probe PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
