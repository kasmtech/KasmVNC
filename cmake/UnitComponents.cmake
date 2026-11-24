# Minimal production components shared with the out-of-tree unit harness.
# No Xorg, installed server, graphics device or packaging configuration required.
set(KASMVNC_UNIT_COMMON_SOURCES
  common/rdr/InStream.cxx common/rdr/Exception.cxx
  common/rfb/PixelFormat.cxx common/rfb/Region.cxx common/Xregion/Region.c
  common/rfb/encoders/EncoderConfiguration.cpp common/rfb/encoders/utils.cpp
  common/network/jsonescape.c common/network/cJSON.c
  common/network/StunServer.cxx)
function(kasmvnc_unit_components root)
  list(TRANSFORM KASMVNC_UNIT_COMMON_SOURCES PREPEND "${root}/")
  add_library(kasmvnc_unit_common STATIC ${KASMVNC_UNIT_COMMON_SOURCES})
  target_include_directories(kasmvnc_unit_common PUBLIC "${root}/common" "${root}/unix/kasmvncpasswd")
  target_compile_features(kasmvnc_unit_common PUBLIC cxx_std_20)
  if(EXISTS "${root}/unix/vncserver-native/config.cxx")
    find_path(YAML_INCLUDE_DIR yaml.h REQUIRED)
    find_library(YAML_LIBRARY yaml REQUIRED)
    add_library(kasmvnc_unit_launcher STATIC "${root}/unix/vncserver-native/config.cxx")
    target_include_directories(kasmvnc_unit_launcher PUBLIC "${root}/unix/vncserver-native" "${YAML_INCLUDE_DIR}")
    target_compile_features(kasmvnc_unit_launcher PUBLIC cxx_std_20)
    target_link_libraries(kasmvnc_unit_launcher PUBLIC "${YAML_LIBRARY}")
  endif()
endfunction()
