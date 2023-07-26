# - Find freetype
# Find the freetype libraries
#
#  This module defines the following variables:
#     FREETYPE_FOUND        - True if freetype is found
#     FREETYPE_INCLUDE_DIRS - include directories
#

find_package(PkgConfig)
pkg_check_modules(PC_FREETYPE freetype2)
find_path(FREETYPE_INCLUDE_DIRS NAMES ft2build.h HINTS ${PC_FREETYPE_INCLUDE_DIRS})

include(FindPackageHandleStandardArgs)
set(FPHSA_NAME_MISMATCHED 1)
find_package_handle_standard_args(freetype DEFAULT_MSG FREETYPE_INCLUDE_DIRS)
unset(FPHSA_NAME_MISMATCHED)
mark_as_advanced(FREETYPE_INCLUDE_DIRS)
