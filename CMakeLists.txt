#
# Setup
#

cmake_minimum_required(VERSION 2.8)
if(POLICY CMP0022)
  cmake_policy(SET CMP0022 OLD)
endif()

# Internal cmake modules
set(CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/cmake/Modules)

include(CheckIncludeFiles)
include(CheckFunctionExists)
include(CheckLibraryExists)
include(CheckTypeSize)
include(CheckCSourceCompiles)
include(CheckCXXSourceCompiles)
include(CheckCXXCompilerFlag)
include(CheckCSourceRuns)

include(CMakeMacroLibtoolFile)

project(kasmvnc)
set(VERSION 0.9)

# The RC version must always be four comma-separated numbers
set(RCVERSION 0,9,0,0)

# Installation paths
set(BIN_DIR "${CMAKE_INSTALL_PREFIX}/bin")
set(DATA_DIR "${CMAKE_INSTALL_PREFIX}/share")
set(MAN_DIR "${DATA_DIR}/man")
set(LOCALE_DIR "${DATA_DIR}/locale")
set(DOC_DIR "${CMAKE_INSTALL_PREFIX}/share/doc/${CMAKE_PROJECT_NAME}-${VERSION}")

if(WIN32)
set(BIN_DIR "${CMAKE_INSTALL_PREFIX}")
set(DOC_DIR "${CMAKE_INSTALL_PREFIX}")
endif()

if(MSVC)
  message(FATAL_ERROR "KasmVNC cannot be built with Visual Studio.  Please use MinGW")
endif()

if(NOT BUILD_TIMESTAMP)
  set(BUILD_TIMESTAMP "")
  execute_process(COMMAND "date" "+%Y-%m-%d %H:%M" OUTPUT_VARIABLE BUILD_TIMESTAMP)
  string(REGEX REPLACE "\n" "" BUILD_TIMESTAMP ${BUILD_TIMESTAMP})
endif()

# Default to optimised builds instead of debug ones. Our code has no bugs ;)
# (CMake makes it fairly easy to toggle this back to Debug if needed)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

message(STATUS "CMAKE_BUILD_TYPE = ${CMAKE_BUILD_TYPE}")

message(STATUS "VERSION = ${VERSION}")
message(STATUS "BUILD_TIMESTAMP = ${BUILD_TIMESTAMP}")
add_definitions(-DBUILD_TIMESTAMP="${BUILD_TIMESTAMP}")

message(STATUS "WWWDIR = ${DATA_DIR}/kasmvnc/www")
add_definitions(-DWWWDIR="${DATA_DIR}/kasmvnc/www")

# We want to keep our asserts even in release builds so remove NDEBUG
set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} -UNDEBUG")
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -UNDEBUG")
set(CMAKE_C_FLAGS_RELWITHDEBINFO "${CMAKE_C_FLAGS_RELWITHDEBINFO} -UNDEBUG")
set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "${CMAKE_CXX_FLAGS_RELWITHDEBINFO} -UNDEBUG")
set(CMAKE_C_FLAGS_MINSIZEREL "${CMAKE_C_FLAGS_MINSIZEREL} -UNDEBUG")
set(CMAKE_CXX_FLAGS_MINSIZEREL "${CMAKE_CXX_FLAGS_MINSIZEREL} -UNDEBUG")

# Make sure we get a sane C version
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -std=gnu99")

# Enable OpenMP
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fopenmp")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fopenmp")

# Enable C++ 11
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=gnu++11")

# Tell the compiler to be stringent
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wformat=2")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wformat=2")
# Make sure we catch these issues whilst developing
IF(CMAKE_BUILD_TYPE MATCHES Debug)
  set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Werror")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Werror")
ENDIF()

option(ENABLE_ASAN "Enable address sanitizer support" OFF)
if(ENABLE_ASAN AND NOT WIN32 AND NOT APPLE)
  set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fsanitize=address")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fsanitize=address")
endif()

option(ENABLE_TSAN "Enable thread sanitizer support" OFF)
if(ENABLE_TSAN AND NOT WIN32 AND NOT APPLE AND CMAKE_SIZEOF_VOID_P MATCHES 8)
  set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fsanitize=thread")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fsanitize=thread")
endif()

if(NOT DEFINED BUILD_WINVNC)
  set(BUILD_WINVNC 1)
endif()

# Minimum version is Windows 7
if(WIN32)
  add_definitions(-D_WIN32_WINNT=0x0601)
endif()

if(CMAKE_SIZEOF_VOID_P MATCHES 8)
  message(STATUS "64-bit build")
else()
  message(STATUS "32-bit build")
endif()

# Versions of CMake before 2.8.7 do not properly support resource compilation
# with MinGW.  Boo!
if(MINGW AND "${CMAKE_VERSION}" VERSION_LESS "2.8.7")
  if(NOT DEFINED RC)
    set(CMAKE_RC_COMPILER_INIT windres)
  else()
    set(CMAKE_RC_COMPILER_INIT ${RC})
  endif()
  enable_language(RC)
  message(STATUS "Resource compiler: ${CMAKE_RC_COMPILER}")
  set(CMAKE_RC_COMPILE_OBJECT
    "<CMAKE_RC_COMPILER> <FLAGS> <DEFINES> -o <OBJECT> --output-format=coff <SOURCE>")
endif()

# MinGW64 has header support but no library support for IActiveDesktop, so we
# need to check for both the header and library and use our own implementation
# in common/os if either doesn't exist.
if(WIN32)
  check_c_source_compiles("#include <windows.h>\n#include <wininet.h>\n#include <shlobj.h>\nint main(int c, char** v) {IActiveDesktop iad; (void)iad; return 0;}" HAVE_ACTIVE_DESKTOP_H)
  check_c_source_compiles("#include <windows.h>\n#include <wininet.h>\n#include <shlobj.h>\nint main(int c, char** v) {GUID i = CLSID_ActiveDesktop; (void)i; return 0;}" HAVE_ACTIVE_DESKTOP_L)
endif()

# X11 stuff. It's in a if() so that we can say REQUIRED
if(UNIX AND NOT APPLE)
  find_package(X11 REQUIRED)
endif()

# Check for zlib
find_package(ZLIB REQUIRED)

# Check for libjpeg
find_package(JPEG REQUIRED)

# Staticly link libjpeg-turbo
set(JPEG_LIBRARIES "-Wl,-Bstatic -lturbojpeg -Wl,-Bdynamic")
# Warn if it doesn't seem to be the accelerated libjpeg that's found
set(CMAKE_REQUIRED_LIBRARIES ${JPEG_LIBRARIES})
set(CMAKE_REQUIRED_FLAGS -I${JPEG_INCLUDE_DIR})

set(JPEG_TEST_SOURCE "\n
  #include <stdio.h>\n
  #include <jpeglib.h>\n
  int main(void) {\n
    struct jpeg_compress_struct cinfo;\n
    struct jpeg_error_mgr jerr;\n
    cinfo.err=jpeg_std_error(&jerr);\n
    jpeg_create_compress(&cinfo);\n
    cinfo.input_components = 3;\n
    jpeg_set_defaults(&cinfo);\n
    cinfo.in_color_space = JCS_EXT_RGB;\n
    jpeg_default_colorspace(&cinfo);\n
    return 0;\n
  }")

if(CMAKE_CROSSCOMPILING)
  check_c_source_compiles("${JPEG_TEST_SOURCE}" FOUND_LIBJPEG_TURBO)
else()
  check_c_source_runs("${JPEG_TEST_SOURCE}" FOUND_LIBJPEG_TURBO)
endif()

set(CMAKE_REQUIRED_LIBRARIES)
set(CMAKE_REQUIRED_FLAGS)
set(CMAKE_REQUIRED_DEFINITIONS)

if(NOT FOUND_LIBJPEG_TURBO)
  message(STATUS "WARNING: You are not using libjpeg-turbo. Performance will suffer.")
endif()

include_directories(${JPEG_INCLUDE_DIR})

# Check for GNUTLS library
option(ENABLE_GNUTLS "Enable protocol encryption and advanced authentication" ON)
if(ENABLE_GNUTLS)
  find_package(GnuTLS)
  if (GNUTLS_FOUND)
    include_directories(${GNUTLS_INCLUDE_DIR})
    add_definitions("-DHAVE_GNUTLS")
    add_definitions(${GNUTLS_DEFINITIONS})
  endif()
endif()

# Check for PAM library
option(ENABLE_PAM "Enable PAM authentication support" ON)
if(ENABLE_PAM)
  check_include_files(security/pam_appl.h HAVE_PAM_H)
  set(CMAKE_REQUIRED_LIBRARIES -lpam)
  check_function_exists(pam_start HAVE_PAM_START)
  set(CMAKE_REQUIRED_LIBRARIES)
  if(HAVE_PAM_H AND HAVE_PAM_START)
    set(PAM_LIBS pam)
  else()
    set(ENABLE_PAM 0)
  endif()
endif()
set(HAVE_PAM ${ENABLE_PAM})

# Check for SSE2
check_cxx_compiler_flag(-msse2 COMPILER_SUPPORTS_SSE2)

# Generate config.h and make sure the source finds it
configure_file(config.h.in config.h)
add_definitions(-DHAVE_CONFIG_H)
include_directories(${CMAKE_BINARY_DIR})

include(cmake/StaticBuild.cmake)

add_subdirectory(common)

if(WIN32)
  add_subdirectory(win)
else()
  # No interest in building x related parts on Apple
  if(NOT APPLE)
    add_subdirectory(unix)
  endif()
endif()

if(ENABLE_NLS)
  add_subdirectory(po)
endif()

add_subdirectory(tests)


include(cmake/BuildPackages.cmake)

# uninstall
configure_file("${CMAKE_SOURCE_DIR}/cmake/cmake_uninstall.cmake.in"
  "cmake_uninstall.cmake" IMMEDIATE @ONLY)

add_custom_target(uninstall COMMAND ${CMAKE_COMMAND} -P cmake_uninstall.cmake)
