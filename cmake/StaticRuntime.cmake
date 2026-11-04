# Optional application-library prefix. Host graphics and desktop integration
# libraries remain shared; this does not create a fully static Linux executable.
set(KASMVNC_STATIC_RUNTIME_PREFIX "" CACHE PATH "Prefix containing static PNG, zlib and WebP")
set(KASMVNC_OPENSSL_PREFIX "" CACHE PATH "Portable OpenSSL 3.0 build SDK; empty uses distro OpenSSL")
set(KASMVNC_CRYPTO_LIBRARY crypto)
set(KASMVNC_RUNTIME_MAKE "")
if(KASMVNC_OPENSSL_PREFIX)
    foreach(name ssl crypto)
        if(NOT EXISTS "${KASMVNC_OPENSSL_PREFIX}/lib/lib${name}.so.3")
            message(FATAL_ERROR "Missing OpenSSL 3 SDK shared library: ${name}")
        endif()
        if(EXISTS "${KASMVNC_OPENSSL_PREFIX}/lib/lib${name}.a")
            message(FATAL_ERROR "The OpenSSL SDK must not contain static archives")
        endif()
    endforeach()
    if(NOT EXISTS "${KASMVNC_OPENSSL_PREFIX}/include/openssl/opensslv.h")
        message(FATAL_ERROR "Missing OpenSSL SDK headers")
    endif()
    file(READ "${KASMVNC_OPENSSL_PREFIX}/include/openssl/opensslv.h" openssl_version)
    if(NOT openssl_version MATCHES "OPENSSL_VERSION_MAJOR +3" OR
       NOT openssl_version MATCHES "OPENSSL_VERSION_MINOR +0")
        message(FATAL_ERROR "Portable compilation requires OpenSSL 3.0 headers")
    endif()
    # The SDK is only a linker input: installed binaries resolve the host DSOs.
    set(CMAKE_SKIP_RPATH TRUE)
    add_library(kasmvnc_shared_crypto SHARED IMPORTED)
    set_target_properties(kasmvnc_shared_crypto PROPERTIES
        IMPORTED_LOCATION "${KASMVNC_OPENSSL_PREFIX}/lib/libcrypto.so.3"
        IMPORTED_SONAME "libcrypto.so.3")
    set(KASMVNC_CRYPTO_LIBRARY kasmvnc_shared_crypto)
endif()
if(KASMVNC_STATIC_RUNTIME_PREFIX)
    set(runtime_lib "${KASMVNC_STATIC_RUNTIME_PREFIX}/lib")
    foreach(name png16 z webp sharpyuv)
        if(NOT EXISTS "${runtime_lib}/lib${name}.a")
            message(FATAL_ERROR "Missing static runtime archive: ${runtime_lib}/lib${name}.a")
        endif()
    endforeach()
    include_directories(BEFORE "${KASMVNC_STATIC_RUNTIME_PREFIX}/include")
    set(PNG_LIBRARIES "${runtime_lib}/libpng16.a;${runtime_lib}/libz.a")
    set(PNG_INCLUDE_DIR "${KASMVNC_STATIC_RUNTIME_PREFIX}/include")
    set(ZLIB_LIBRARIES "${runtime_lib}/libz.a")
    if(EXISTS "${runtime_lib}/libssl.a" OR EXISTS "${runtime_lib}/libcrypto.a" OR
       EXISTS "${KASMVNC_STATIC_RUNTIME_PREFIX}/include/openssl")
        message(FATAL_ERROR "Obsolete static OpenSSL in runtime prefix; rebuild the dependency image")
    endif()
    # Avoid exporting our private copies into subsequently loaded GPU drivers.
    set(private_archives "libpng16.a:libz.a:libwebp.a:libsharpyuv.a:libavformat.a:libavcodec.a:libavutil.a:libswscale.a:libswresample.a:libx264.a:libx265.a:libSvtAv1Enc.a")
    set(KASMVNC_RUNTIME_MAKE
"KASMVNC_WEBP_LIBS = ${runtime_lib}/libwebp.a ${runtime_lib}/libsharpyuv.a
KASMVNC_RUNTIME_CPPFLAGS = -I${KASMVNC_STATIC_RUNTIME_PREFIX}/include
KASMVNC_RUNTIME_LDFLAGS = -static-libstdc++ -Wl,--exclude-libs,${private_archives}
")
endif()
if(KASMVNC_OPENSSL_PREFIX)
    include_directories(BEFORE "${KASMVNC_OPENSSL_PREFIX}/include")
    string(APPEND KASMVNC_RUNTIME_MAKE
"KASMVNC_TLS_LIBS = ${KASMVNC_OPENSSL_PREFIX}/lib/libssl.so.3 ${KASMVNC_OPENSSL_PREFIX}/lib/libcrypto.so.3
KASMVNC_RUNTIME_CPPFLAGS := -I${KASMVNC_OPENSSL_PREFIX}/include $(KASMVNC_RUNTIME_CPPFLAGS)
")
endif()
file(WRITE "${CMAKE_BINARY_DIR}/kasmvnc-static-runtime.mk" "${KASMVNC_RUNTIME_MAKE}")
