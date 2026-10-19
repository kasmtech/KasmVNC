# Resolve the complete pkg-config private link closure. Absolute archive paths
# are intentional: Xvnc also consumes these through librfb.la, outside CMake.
pkg_check_modules(STATIC_FFMPEG REQUIRED libavformat libavcodec libswscale libavutil)
set(KASMVNC_FFMPEG_ARCHIVES "")
foreach(dependency IN LISTS STATIC_FFMPEG_STATIC_LIBRARIES)
    # OS and hardware-driver interfaces remain dynamic. Never bundle a vendor
    # driver into the server; it must match the host kernel driver.
    if(dependency MATCHES "^(m|dl|pthread|rt|atomic|gcc_s|va|va-drm|drm)$")
        list(APPEND KASMVNC_FFMPEG_ARCHIVES "-l${dependency}")
    else()
        unset(ffmpeg_archive CACHE)
        if(dependency MATCHES "^(avformat|avcodec|avutil|swscale|swresample)$")
            # Do not pair headers from a custom FFmpeg with distro archives.
            pkg_get_variable(ffmpeg_libdir "lib${dependency}" libdir)
            find_file(ffmpeg_archive NAMES "lib${dependency}.a"
                PATHS "${ffmpeg_libdir}" NO_DEFAULT_PATH)
        else()
            find_file(ffmpeg_archive NAMES "lib${dependency}.a"
                PATHS ${STATIC_FFMPEG_STATIC_LIBRARY_DIRS} ${CMAKE_CXX_IMPLICIT_LINK_DIRECTORIES})
        endif()
        if(NOT ffmpeg_archive)
            message(FATAL_ERROR "Static FFmpeg requires lib${dependency}.a; use a static FFmpeg dependency prefix")
        endif()
        list(APPEND KASMVNC_FFMPEG_ARCHIVES "${ffmpeg_archive}")
    endif()
endforeach()
