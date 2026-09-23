include_directories(${CMAKE_SOURCE_DIR}/common)

add_executable(kasmvncpasswd 
  kasmvncpasswd.c
  kasmpasswd.c)

target_link_libraries(kasmvncpasswd crypt crypto)

install(TARGETS kasmvncpasswd DESTINATION ${BIN_DIR})
