# Optional application-library prefix. Host graphics and desktop integration
# libraries remain shared; this does not create a fully static Linux executable.
set(KASMVNC_STATIC_RUNTIME_PREFIX "" CACHE PATH "Prefix containing static OpenSSL, PNG, zlib and WebP")
set(KASMVNC_CRYPTO_LIBRARY crypto)
set(KASMVNC_RUNTIME_MAKE "")
if(KASMVNC_STATIC_RUNTIME_PREFIX)
    set(runtime_lib "${KASMVNC_STATIC_RUNTIME_PREFIX}/lib")
    foreach(name ssl crypto png16 z webp sharpyuv)
        if(NOT EXISTS "${runtime_lib}/lib${name}.a")
            message(FATAL_ERROR "Missing static runtime archive: ${runtime_lib}/lib${name}.a")
        endif()
    endforeach()
    include_directories(BEFORE "${KASMVNC_STATIC_RUNTIME_PREFIX}/include")
    set(PNG_LIBRARIES "${runtime_lib}/libpng16.a;${runtime_lib}/libz.a")
    set(PNG_INCLUDE_DIR "${KASMVNC_STATIC_RUNTIME_PREFIX}/include")
    set(ZLIB_LIBRARIES "${runtime_lib}/libz.a")
    # Share the system dependency closure between CMake and the Xorg makefile.
    set(KASMVNC_RUNTIME_SYSTEM_LIBRARIES ${CMAKE_DL_LIBS} -pthread)
    set(runtime_system_flags "")
    foreach(system_library IN LISTS KASMVNC_RUNTIME_SYSTEM_LIBRARIES)
        if(IS_ABSOLUTE "${system_library}" OR system_library MATCHES "^-")
            set(runtime_system_flags "${runtime_system_flags} ${system_library}")
        else()
            set(runtime_system_flags "${runtime_system_flags} -l${system_library}")
        endif()
    endforeach()
    string(STRIP "${runtime_system_flags}" runtime_system_flags)
    # An imported target preserves the exact archive even when CMake considers
    # this prefix an implicit compiler search directory (notably EL9).
    add_library(kasmvnc_static_crypto STATIC IMPORTED)
    set_target_properties(kasmvnc_static_crypto PROPERTIES
        IMPORTED_LOCATION "${runtime_lib}/libcrypto.a"
        INTERFACE_LINK_LIBRARIES "${KASMVNC_RUNTIME_SYSTEM_LIBRARIES}")
    set(KASMVNC_CRYPTO_LIBRARY kasmvnc_static_crypto)
    # Avoid exporting our private copies into subsequently loaded GPU drivers.
    set(private_archives "libssl.a:libcrypto.a:libpng16.a:libz.a:libwebp.a:libsharpyuv.a:libavformat.a:libavcodec.a:libavutil.a:libswscale.a:libswresample.a:libx264.a:libx265.a:libSvtAv1Enc.a")
    set(KASMVNC_RUNTIME_MAKE
"KASMVNC_TLS_LIBS = ${runtime_lib}/libssl.a ${runtime_lib}/libcrypto.a ${runtime_system_flags}
KASMVNC_WEBP_LIBS = ${runtime_lib}/libwebp.a ${runtime_lib}/libsharpyuv.a
KASMVNC_RUNTIME_CPPFLAGS = -I${KASMVNC_STATIC_RUNTIME_PREFIX}/include
KASMVNC_RUNTIME_LDFLAGS = -static-libstdc++ -Wl,--exclude-libs,${private_archives}
")
endif()
file(WRITE "${CMAKE_BINARY_DIR}/kasmvnc-static-runtime.mk" "${KASMVNC_RUNTIME_MAKE}")
