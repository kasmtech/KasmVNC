# Opt-in embedding of desktop libraries; GPU dispatch stays host-provided.
set(KASMVNC_STATIC_DESKTOP_PREFIX "" CACHE PATH "Prefix containing static desktop libraries")
set(KASMVNC_DESKTOP_MAKE "")
if(KASMVNC_STATIC_DESKTOP_PREFIX)
  set(CMAKE_POSITION_INDEPENDENT_CODE ON)
  find_package(PythonInterp 3 REQUIRED)
  set(KASMVNC_DESKTOP_MAKE
"# Run after libtool has expanded dependency_libs from .la files.
Xvnc_LINK = $(LIBTOOL) $(AM_V_lt) --tag=CXX $(AM_LIBTOOLFLAGS) $(LIBTOOLFLAGS) --mode=link ${PYTHON_EXECUTABLE} ${CMAKE_SOURCE_DIR}/builder/scripts/link-static-desktop --prefix ${KASMVNC_STATIC_DESKTOP_PREFIX} --runtime-prefix=\"${KASMVNC_STATIC_RUNTIME_PREFIX}\" --source-root ${CMAKE_SOURCE_DIR} -- $(CXX) $(AM_CXXFLAGS) $(CXXFLAGS) $(Xvnc_LDFLAGS) $(LDFLAGS) -o $@
")
endif()
file(WRITE "${CMAKE_BINARY_DIR}/kasmvnc-static-desktop.mk" "${KASMVNC_DESKTOP_MAKE}")

# Apply the same archive policy to shipped helper executables. Otherwise an
# EL9-built helper can retain a newer GLIBCXX ABI even when Xvnc is portable.
function(kasmvnc_static_desktop_target target)
  if(KASMVNC_STATIC_DESKTOP_PREFIX)
    set_target_properties(${target} PROPERTIES POSITION_INDEPENDENT_CODE ON)
    set_property(TARGET ${target} PROPERTY RULE_LAUNCH_LINK
      "${PYTHON_EXECUTABLE} ${CMAKE_SOURCE_DIR}/builder/scripts/link-static-desktop --prefix ${KASMVNC_STATIC_DESKTOP_PREFIX} --runtime-prefix=${KASMVNC_STATIC_RUNTIME_PREFIX} --source-root ${CMAKE_SOURCE_DIR} --")
  endif()
endfunction()
