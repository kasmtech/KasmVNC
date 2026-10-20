include_directories(${CMAKE_SOURCE_DIR}/common)

add_executable(kasmvncpasswd 
  kasmvncpasswd.c
  kasmpasswd.c)

set(kasmvnc_crypt_library crypt)
if(KASMVNC_STATIC_DESKTOP_PREFIX)
  find_library(KASMVNC_STATIC_CRYPT NAMES libcrypt.a)
  if(NOT KASMVNC_STATIC_CRYPT)
    message(FATAL_ERROR "The static desktop profile requires libcrypt.a for kasmvncpasswd")
  endif()
  add_library(kasmvnc_static_crypt STATIC IMPORTED)
  set_target_properties(kasmvnc_static_crypt PROPERTIES IMPORTED_LOCATION "${KASMVNC_STATIC_CRYPT}")
  set(kasmvnc_crypt_library kasmvnc_static_crypt)
endif()
target_link_libraries(kasmvncpasswd ${kasmvnc_crypt_library} ${KASMVNC_CRYPTO_LIBRARY})

if(KASMVNC_STATIC_RUNTIME_PREFIX)
  set_target_properties(kasmvncpasswd PROPERTIES POSITION_INDEPENDENT_CODE ON)
  set_property(TARGET kasmvncpasswd APPEND_STRING PROPERTY LINK_FLAGS " -pie -Wl,--as-needed -Wl,-Map=${CMAKE_CURRENT_BINARY_DIR}/kasmvncpasswd.link-map")
endif()

install(TARGETS kasmvncpasswd DESTINATION ${BIN_DIR})
