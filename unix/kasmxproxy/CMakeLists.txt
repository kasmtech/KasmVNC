include_directories(${X11_INCLUDE_DIR})

add_executable(kasmxproxy
  xxhash.c
  kasmxproxy.c)

target_link_libraries(kasmxproxy ${X11_LIBRARIES} ${X11_XTest_LIB} ${X11_Xrandr_LIB}
                                 ${X11_Xcursor_LIB} ${X11_Xfixes_LIB})

install(TARGETS kasmxproxy DESTINATION ${BIN_DIR})
install(FILES kasmxproxy.man DESTINATION ${MAN_DIR}/man1 RENAME kasmxproxy.1)
