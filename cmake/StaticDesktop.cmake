# Portable linkage policy; desktop libraries can remain host-provided.
set(KASMVNC_STATIC_DESKTOP_PREFIX "" CACHE PATH "Prefix containing static desktop libraries")
option(KASMVNC_OWN_GLIBC_CRT "Use KasmVNC startup for glibc 2.34+ portable executables" OFF)
if(KASMVNC_OWN_GLIBC_CRT AND NOT KASMVNC_DYNAMIC_DESKTOP)
  message(FATAL_ERROR "Owned glibc startup requires KASMVNC_DYNAMIC_DESKTOP")
endif()
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
  if(KASMVNC_OWN_GLIBC_CRT)
    set(desktop_policy "${desktop_policy} --owned-glibc-crt")
    set(KASMVNC_CRT_LINK_INPUTS
      "${CMAKE_SOURCE_DIR}/builder/portable/crt/start-amd64.S"
      "${CMAKE_SOURCE_DIR}/builder/portable/crt/start-arm64.S"
      "${CMAKE_SOURCE_DIR}/builder/portable/crt/bootstrap.c"
      "${CMAKE_SOURCE_DIR}/builder/portable/crt/atexit.c"
      "${CMAKE_SOURCE_DIR}/builder/portable/crt/metadata.S"
      "${CMAKE_SOURCE_DIR}/builder/portable/glibc_crt.py"
      "${CMAKE_SOURCE_DIR}/builder/scripts/link-static-desktop")
  endif()
  set(KASMVNC_STATIC_LINK_BASE
    "\"${PYTHON_EXECUTABLE}\" \"${CMAKE_SOURCE_DIR}/builder/scripts/link-static-desktop\" --prefix \"${KASMVNC_STATIC_DESKTOP_PREFIX}\" --runtime-prefix=\"${KASMVNC_STATIC_RUNTIME_PREFIX}\" --openssl-prefix=\"${KASMVNC_OPENSSL_PREFIX}\" --source-root \"${CMAKE_SOURCE_DIR}\"${desktop_policy}")
  set(KASMVNC_STATIC_LINK_WRAPPER
    "${KASMVNC_STATIC_LINK_BASE} --")
  set(KASMVNC_DESKTOP_MAKE
"# Run after libtool has expanded dependency_libs from .la files.
Xvnc_LINK = $(LIBTOOL) $(AM_V_lt) --tag=CXX $(AM_LIBTOOLFLAGS) $(LIBTOOLFLAGS) --mode=link ${KASMVNC_STATIC_LINK_WRAPPER} $(CXX) $(AM_CXXFLAGS) $(CXXFLAGS) $(Xvnc_LDFLAGS) $(LDFLAGS) -o $@
")
  if(KASMVNC_OWN_GLIBC_CRT)
    # Automake includes this fragment after Xvnc's generated prerequisite
    # expansion, so append a rule rather than updating Xvnc_DEPENDENCIES.
    set(crt_make_inputs "")
    foreach(input IN LISTS KASMVNC_CRT_LINK_INPUTS)
      string(REPLACE "$" "$$" make_input "${input}")
      string(REPLACE "#" "\\#" make_input "${make_input}")
      string(REPLACE " " "\\ " make_input "${make_input}")
      set(crt_make_inputs "${crt_make_inputs} ${make_input}")
    endforeach()
    set(KASMVNC_DESKTOP_MAKE "${KASMVNC_DESKTOP_MAKE}\nXvnc$(EXEEXT):${crt_make_inputs}\n")
  endif()
endif()
file(WRITE "${CMAKE_BINARY_DIR}/kasmvnc-static-desktop.mk" "${KASMVNC_DESKTOP_MAKE}")

# Apply the desktop dependency policy to shipped helper executables.
# RUNTIME_ONLY shares executable flags without adding desktop dependencies.
# The final ELF audit checks their compiler ABI versions as well as Xvnc.
function(kasmvnc_static_desktop_target target)
  cmake_parse_arguments(POLICY "RUNTIME_ONLY" "" "" ${ARGN})
  if(KASMVNC_OWN_GLIBC_CRT)
    set_property(TARGET ${target} APPEND PROPERTY LINK_DEPENDS ${KASMVNC_CRT_LINK_INPUTS})
  endif()
  if(POLICY_RUNTIME_ONLY)
    if(KASMVNC_OWN_GLIBC_CRT)
      set_target_properties(${target} PROPERTIES POSITION_INDEPENDENT_CODE ON)
      set_property(TARGET ${target} PROPERTY RULE_LAUNCH_LINK
        "${KASMVNC_STATIC_LINK_BASE} --runtime-only --")
      return()
    endif()
    # Small C helpers need the portable executable flags, not the desktop
    # dependency closure. Preserve activation for runtime-prefix-only builds.
    if(KASMVNC_STATIC_RUNTIME_PREFIX)
      set_target_properties(${target} PROPERTIES POSITION_INDEPENDENT_CODE ON)
      set_property(TARGET ${target} APPEND_STRING PROPERTY LINK_FLAGS
        " -pie -Wl,--as-needed -Wl,-Map=${CMAKE_CURRENT_BINARY_DIR}/${target}.link-map")
    endif()
    return()
  endif()
  if(KASMVNC_STATIC_DESKTOP_PREFIX OR KASMVNC_DYNAMIC_DESKTOP)
    set_target_properties(${target} PROPERTIES POSITION_INDEPENDENT_CODE ON)
    set_property(TARGET ${target} PROPERTY RULE_LAUNCH_LINK
      "${KASMVNC_STATIC_LINK_WRAPPER}")
  endif()
endfunction()
