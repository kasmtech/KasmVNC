# Standalone cache regression tests; no server or legacy viewer dependencies.
cmake_minimum_required(VERSION 3.10)
project(enccache_tests LANGUAGES CXX)

enable_testing()
add_executable(enccache enccache.cxx ../../common/rfb/EncCache.cxx)
target_include_directories(enccache PRIVATE ../../common)
target_compile_features(enccache PRIVATE cxx_std_20)
target_compile_options(enccache PRIVATE -Wall -Wextra -Werror)

option(ENABLE_ASAN "Enable address and leak checks (Linux)" OFF)
if(ENABLE_ASAN)
    target_compile_options(enccache PRIVATE -fsanitize=address -fno-omit-frame-pointer)
    set_property(TARGET enccache APPEND_STRING PROPERTY LINK_FLAGS " -fsanitize=address")
endif()

foreach(case ordering lookup replacement destruction)
    add_test(NAME enccache_${case} COMMAND enccache ${case})
endforeach()
