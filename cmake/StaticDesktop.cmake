# Portable linkage policy; desktop libraries can remain host-provided.
set(KASMVNC_STATIC_DESKTOP_PREFIX "" CACHE PATH "Prefix containing static desktop libraries")
set(KASMVNC_DESKTOP_MAKE "")
if(KASMVNC_STATIC_DESKTOP_PREFIX OR KASMVNC_DYNAMIC_DESKTOP)
  set(CMAKE_POSITION_INDEPENDENT_CODE ON)
  find_package(PythonInterp 3 REQUIRED)
  # Both consumers execute through a shell; quote each path, including an empty
  # runtime prefix, once here rather than maintaining two command variants.
  set(desktop_policy "")
  if(KASMVNC_DYNAMIC_DESKTOP)
    set(desktop_policy " --host-shared")
  endif()
  set(KASMVNC_STATIC_LINK_WRAPPER
    "\"${PYTHON_EXECUTABLE}\" \"${CMAKE_SOURCE_DIR}/builder/scripts/link-static-desktop\" --prefix \"${KASMVNC_STATIC_DESKTOP_PREFIX}\" --runtime-prefix=\"${KASMVNC_STATIC_RUNTIME_PREFIX}\" --openssl-prefix=\"${KASMVNC_OPENSSL_PREFIX}\" --source-root \"${CMAKE_SOURCE_DIR}\"${desktop_policy} --")
  set(KASMVNC_DESKTOP_MAKE
"# Run after libtool has expanded dependency_libs from .la files.
Xvnc_LINK = $(LIBTOOL) $(AM_V_lt) --tag=CXX $(AM_LIBTOOLFLAGS) $(LIBTOOLFLAGS) --mode=link ${KASMVNC_STATIC_LINK_WRAPPER} $(CXX) $(AM_CXXFLAGS) $(CXXFLAGS) $(Xvnc_LDFLAGS) $(LDFLAGS) -o $@
")
endif()
file(WRITE "${CMAKE_BINARY_DIR}/kasmvnc-static-desktop.mk" "${KASMVNC_DESKTOP_MAKE}")

# Apply the same dependency policy to every shipped helper executable.
# The final ELF audit checks their compiler ABI versions as well as Xvnc.
function(kasmvnc_static_desktop_target target)
  if(KASMVNC_STATIC_DESKTOP_PREFIX OR KASMVNC_DYNAMIC_DESKTOP)
    set_target_properties(${target} PROPERTIES POSITION_INDEPENDENT_CODE ON)
    set_property(TARGET ${target} PROPERTY RULE_LAUNCH_LINK
      "${KASMVNC_STATIC_LINK_WRAPPER}")
  endif()
endfunction()
