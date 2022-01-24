include_directories(${CMAKE_SOURCE_DIR}/common ${CMAKE_SOURCE_DIR}/unix/kasmvncpasswd)

set(NETWORK_SOURCES
  GetAPIMessager.cxx
  Blacklist.cxx
  Socket.cxx
  TcpSocket.cxx
  cJSON.c
  jsonescape.c
  websocket.c
  websockify.c
  ${CMAKE_SOURCE_DIR}/unix/kasmvncpasswd/kasmpasswd.c)

if(NOT WIN32)
  set(NETWORK_SOURCES ${NETWORK_SOURCES} UnixSocket.cxx)
endif()

add_library(network STATIC ${NETWORK_SOURCES})

if(WIN32)
	target_link_libraries(network ws2_32)
endif()

if(UNIX)
  libtool_create_control_file(network)
endif()
