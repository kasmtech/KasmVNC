include_directories(${CMAKE_SOURCE_DIR}/common ${CMAKE_SOURCE_DIR}/unix/kasmvncpasswd ${FFMPEG_INCLUDE_DIRS})

set(NETWORK_SOURCES
        GetAPIMessager.cxx
        Blacklist.cxx
        iceip.cxx
        StunServer.cxx
        Socket.cxx
        TcpSocket.cxx
        cJSON.c
        jsonescape.c
        websocket.c
        websockify.c

        ${CMAKE_SOURCE_DIR}/unix/kasmvncpasswd/kasmpasswd.c)

if (NOT WIN32)
    set(NETWORK_SOURCES ${NETWORK_SOURCES} UnixSocket.cxx)
endif ()

set(NETWORK_SOURCES ${NETWORK_SOURCES}
        ${CMAKE_SOURCE_DIR}/common/rfb/webrtc/WebRTCMediaTransport.cxx
        ${CMAKE_SOURCE_DIR}/common/rfb/webrtc/WebRTCImageTransport.cxx
        ${CMAKE_SOURCE_DIR}/common/rfb/webrtc/RtcIceServers.cxx
        ${CMAKE_SOURCE_DIR}/common/rfb/webrtc/TurnConfig.cxx
        ${CMAKE_SOURCE_DIR}/common/rfb/webrtc/ImageChannelStream.cxx)

add_library(network STATIC ${NETWORK_SOURCES})

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBSRTP2 QUIET libsrtp2)

find_library(KASM_SRTP2_STATIC
    NAMES libsrtp2.a
    HINTS "$ENV{HOME}/.local/lib" "$ENV{HOME}/.local/lib64"
    NO_DEFAULT_PATH)
if(NOT KASM_SRTP2_STATIC)
    find_library(KASM_SRTP2_STATIC NAMES libsrtp2.a)
endif()
if(KASM_SRTP2_STATIC)
    message(STATUS "Linking libsrtp2 statically: ${KASM_SRTP2_STATIC}")
    set(KASM_SRTP2_LIBS ${KASM_SRTP2_STATIC} crypto)
elseif(LIBSRTP2_FOUND)
    message(WARNING "No static libsrtp2.a found; linking libsrtp2 "
        "dynamically (package will depend on the distro libsrtp2). "
        "Run builder/scripts/build-srtp2 to build a static copy.")
    set(KASM_SRTP2_LIBS srtp2)
else()
    message(FATAL_ERROR
        "libsrtp2 is required: no static libsrtp2.a and no libsrtp2 "
        "pkg-config module found. Run builder/scripts/build-srtp2, or "
        "install the development package: "
        "  Debian/Ubuntu/Kali: libsrtp2-dev "
        "  Fedora/Oracle:      libsrtp-devel (EPEL on EL8/EL9) "
        "  openSUSE:           libsrtp2-devel "
        "  Alpine:             libsrtp-dev")
endif()

foreach(_lib datachannel juice usrsctp)
    find_library(KASM_${_lib}_STATIC
        NAMES lib${_lib}.a
        HINTS "$ENV{HOME}/.local/lib" "$ENV{HOME}/.local/lib64"
        NO_DEFAULT_PATH)
    if(NOT KASM_${_lib}_STATIC)
        message(FATAL_ERROR
            "Static lib${_lib}.a not found. Build the WebRTC dependencies "
            "with builder/scripts/build-srtp2 followed by "
            "builder/scripts/build-libdatachannel (installs into ~/.local).")
    endif()
endforeach()

find_path(KASM_LIBDATACHANNEL_INCLUDE_DIR rtc/rtc.hpp
    HINTS "$ENV{HOME}/.local/include"
    NO_DEFAULT_PATH)
if(NOT KASM_LIBDATACHANNEL_INCLUDE_DIR)
    message(FATAL_ERROR
        "libdatachannel headers (rtc/rtc.hpp) not found. "
        "Run builder/scripts/build-libdatachannel.")
endif()

target_include_directories(network PUBLIC ${KASM_LIBDATACHANNEL_INCLUDE_DIR})
target_compile_definitions(network PUBLIC
    RTC_STATIC RTC_ENABLE_WEBSOCKET=0 RTC_ENABLE_MEDIA=1)
target_link_libraries(network
    ${KASM_datachannel_STATIC}
    ${KASM_juice_STATIC}
    ${KASM_usrsctp_STATIC}
    ${KASM_SRTP2_LIBS})

if(WIN32)
	target_link_libraries(network ws2_32)
endif()

if(UNIX)
  libtool_create_control_file(network)
endif()
