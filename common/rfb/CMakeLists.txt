set(RFB_SOURCES
        benchmark/benchmark.cxx
        Blacklist.cxx
        Congestion.cxx
        CConnection.cxx
        CMsgHandler.cxx
        CMsgReader.cxx
        CMsgWriter.cxx
        CSecurityPlain.cxx
        CSecurityStack.cxx
        CSecurityVeNCrypt.cxx
        CSecurityVncAuth.cxx
        ComparingUpdateTracker.cxx
        Configuration.cxx
        ConnParams.cxx
        CopyRectDecoder.cxx
        Cursor.cxx
        DecodeManager.cxx
        Decoder.cxx
        d3des.c
        EncCache.cxx
        EncodeManager.cxx
        Encoder.cxx
        HextileDecoder.cxx
        HextileEncoder.cxx
        JpegCompressor.cxx
        JpegDecompressor.cxx
        KeyRemapper.cxx
        LogWriter.cxx
        Logger.cxx
        Logger_file.cxx
        Logger_stdio.cxx
        Password.cxx
        PixelBuffer.cxx
        PixelFormat.cxx
        RREEncoder.cxx
        RREDecoder.cxx
        RawDecoder.cxx
        RawEncoder.cxx
        Region.cxx
        SConnection.cxx
        SMsgHandler.cxx
        SMsgReader.cxx
        SMsgWriter.cxx
        ServerCore.cxx
        Security.cxx
        SecurityServer.cxx
        SecurityClient.cxx
        SelfBench.cxx
        SSecurityPlain.cxx
        SSecurityStack.cxx
        SSecurityVncAuth.cxx
        SSecurityVeNCrypt.cxx
        ScaleFilters.cxx
        Timer.cxx
        TightDecoder.cxx
        TightEncoder.cxx
        TightJPEGEncoder.cxx
        TightWEBPEncoder.cxx
        TightQOIEncoder.cxx
        UpdateTracker.cxx
        VNCSConnectionST.cxx
        VNCServerST.cxx
        ZRLEEncoder.cxx
        ZRLEDecoder.cxx
        Watermark.cxx
        cpuid.cxx
        encodings.cxx
        util.cxx
        xxhash.c
        ffmpeg.cxx
        encoders/SoftwareEncoder.cxx
        benchmark/FfmpegFrameFeeder.cpp
        encoders/ScreenEncoderManager.cxx
        encoders/FFMPEGHWEncoder.cxx
        encoders/ScreenEncoderManager.cxx
        encoders/VideoEncoderFactory.cxx
        encoders/EncoderProbe.cpp
        encoders/EncoderConfiguration.cpp
        encoders/utils.cpp
)

if (UNIX)
    set(RFB_SOURCES ${RFB_SOURCES} Logger_syslog.cxx)
endif ()

if (WIN32)
    include_directories(${CMAKE_SOURCE_DIR}/win)
    set(RFB_SOURCES ${RFB_SOURCES} WinPasswdValidator.cxx)
endif (WIN32)

set(RFB_LIBRARIES ${JPEG_LIBRARIES} ${PNG_LIBRARIES} os rdr Xregion statgrab fmt)

cmake_host_system_information(RESULT DISTRO QUERY DISTRIB_INFO)
if ((CMAKE_CXX_COMPILER_ID MATCHES "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 10) OR
(DISTRO_PLATFORM_ID MATCHES "platform:el8"))
    set(RFB_LIBRARIES ${RFB_LIBRARIES} tbb)
endif ()

if (HAVE_PAM)
    set(RFB_SOURCES
            ${RFB_SOURCES}
            UnixPasswordValidator.cxx
            UnixPasswordValidator.h pam.c pam.h)
    set(RFB_LIBRARIES ${RFB_LIBRARIES} ${PAM_LIBS})
endif ()

if (GNUTLS_FOUND)
    set(RFB_SOURCES
            ${RFB_SOURCES}
            CSecurityTLS.cxx
            SSecurityTLS.cxx
    )
    set(RFB_LIBRARIES
            ${RFB_LIBRARIES}
            ${GNUTLS_LIBRARIES}
    )
endif ()

# SSE2

set(SSE2_SOURCES
        scale_sse2.cxx)

set(SCALE_DUMMY_SOURCES
        scale_dummy.cxx)

if (COMPILER_SUPPORTS_SSE2)
    set_source_files_properties(${SSE2_SOURCES} PROPERTIES COMPILE_FLAGS ${COMPILE_FLAGS} -msse2)
    set(RFB_SOURCES
            ${RFB_SOURCES}
            ${SSE2_SOURCES}
    )
else ()
    set(RFB_SOURCES
            ${RFB_SOURCES}
            ${SCALE_DUMMY_SOURCES}
    )
endif ()

find_package(PkgConfig REQUIRED)

pkg_check_modules(CPUID REQUIRED libcpuid)
pkg_check_modules(FMT REQUIRED fmt)
pkg_check_modules(VIDEO_ACCELERATION REQUIRED libva libva-drm libdrm)

find_package(TBB)
if (TBB_FOUND)
    set(RFB_LIBRARIES ${RFB_LIBRARIES} tbb)
else ()
    pkg_check_modules(TBB tbb)
    set(RFB_LIBRARIES ${RFB_LIBRARIES} ${TBB_LIBRARIES})
endif ()

find_library(LIBYUV_LIBRARY NAMES yuv)
if (LIBYUV_LIBRARY)
    set(RFB_LIBRARIES ${RFB_LIBRARIES} ${LIBYUV_LIBRARY})
else ()
    message(FATAL_ERROR "libyuv was not found")
endif ()

if (ENABLE_DEBUG_ENCODERS)
    set(RFB_DEFINITIONS ${RFB_DEFINITIONS} DEBUG_ENCODERS)
endif ()

add_library(rfb STATIC ${RFB_SOURCES})

target_include_directories(rfb PRIVATE
        ${CMAKE_SOURCE_DIR}/common
        ${JPEG_INCLUDE_DIR}
        ${PNG_INCLUDE_DIR}
        ${CMAKE_SOURCE_DIR}/unix/kasmvncpasswd
        ${CMAKE_SOURCE_DIR}/third_party/tinyxml2
        ${FFMPEG_INCLUDE_DIRS}
        ${TBB_INCLUDE_DIRS}
        ${CPUID_INCLUDE_DIRS}
        ${LIBYUV_INCLUDE_DIR}
)

set(RFB_LIBRARIES ${RFB_LIBRARIES} tinyxml2_objs ${TBB_LIBRARIES} ${CPUID_LIBRARIES} ${FMT_LIBRARIES} ${VIDEO_ACCELERATION_LIBRARIES})

if (DEBUG_FFMPEG)
    set(RFB_LIBRARIES ${RFB_LIBRARIES} ${FFMPEG_LIBRARIES})
endif ()

target_compile_definitions(rfb PRIVATE ${RFB_DEFINITIONS})

target_include_directories(rfb PUBLIC ${VIDEO_ACCELERATION_INCLUDE_DIRS})
target_link_libraries(rfb PUBLIC ${RFB_LIBRARIES})

if (UNIX)
    libtool_create_control_file(rfb)
endif ()
