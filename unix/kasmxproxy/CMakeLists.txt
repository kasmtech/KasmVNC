include_directories(${X11_INCLUDE_DIR})

add_executable(kasmxproxy
  kasmxproxy.c)

target_link_libraries(kasmxproxy ${X11_LIBRARIES} ${X11_XTest_LIB})

install(TARGETS kasmxproxy DESTINATION ${BIN_DIR})
