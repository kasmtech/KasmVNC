# Each object target sees exactly one FFmpeg installed-header tree. Versioned
# namespaces isolate private C++ implementations; no AV type is in the public API.
set(KASMVNC_FFMPEG_OBJECTS "")
if(KASMVNC_FFMPEG_ADAPTERS)
    set(adapter_families 60 61)
else()
    set(adapter_families legacy)
endif()
foreach(family IN LISTS adapter_families)
    set(adapter_target kasmvnc_ffmpeg_${family})
    add_library(${adapter_target} OBJECT ${CMAKE_SOURCE_DIR}/common/rfb/ffmpeg/Backend.cxx)
    set_target_properties(${adapter_target} PROPERTIES POSITION_INDEPENDENT_CODE ON INTERPROCEDURAL_OPTIMIZATION OFF)
    target_compile_definitions(${adapter_target} PRIVATE
        KASMVNC_FFMPEG_NAMESPACE=kasm_ffmpeg_${family}
        KASMVNC_FFMPEG_FACTORY=make_ffmpeg_${family})
    target_include_directories(${adapter_target} PRIVATE ${CMAKE_SOURCE_DIR}/common ${LIBYUV_INCLUDE_DIR})
    if(KASMVNC_FFMPEG_ADAPTERS)
        set(header_path "${KASMVNC_FFMPEG_HEADERS_ROOT}/${family}/include")
        if(NOT EXISTS "${header_path}/libavcodec/avcodec.h")
            message(FATAL_ERROR "Missing FFmpeg ${family} installed headers: ${header_path}")
        endif()
        target_include_directories(${adapter_target} BEFORE PRIVATE "${header_path}")
        target_compile_definitions(${adapter_target} PRIVATE KASMVNC_EXPECTED_AVCODEC=${family})
    else()
        target_include_directories(${adapter_target} BEFORE PRIVATE ${FFMPEG_INCLUDE_DIRS})
        target_compile_definitions(${adapter_target} PRIVATE KASMVNC_EXPECTED_AVCODEC=0)
    endif()
    # AV layouts are intentionally different across translation units. Keep LTO
    # from trying to merge or reason across those independently compiled layouts.
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${adapter_target} PRIVATE -fno-lto)
    endif()
    list(APPEND KASMVNC_FFMPEG_OBJECTS $<TARGET_OBJECTS:${adapter_target}>)
endforeach()
