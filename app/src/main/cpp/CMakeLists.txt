cmake_minimum_required(VERSION 3.22.1)
project(airplaytv C CXX)

# fetched by fetch-deps.sh; OpenSSL built by build-openssl.sh
set(THIRD_PARTY ${CMAKE_CURRENT_SOURCE_DIR}/../../../../third_party)
set(UXPLAY_LIB ${THIRD_PARTY}/UxPlay/lib)
set(LIBPLIST ${THIRD_PARTY}/libplist)
set(OPENSSL ${THIRD_PARTY}/openssl-android/${ANDROID_ABI})
set(ALAC ${THIRD_PARTY}/alac/codec)

# Apple's ALAC decoder, for AirPlay music (audio-only) streams
add_library(alac STATIC
    ${ALAC}/ALACDecoder.cpp ${ALAC}/ALACBitUtilities.c ${ALAC}/EndianPortable.c
    ${ALAC}/ag_dec.c ${ALAC}/dp_dec.c ${ALAC}/matrix_dec.c
    alac_bridge.cpp)
target_include_directories(alac PRIVATE ${ALAC})
# EndianPortable.c only recognises x86 as little-endian; ARM is too
target_compile_definitions(alac PRIVATE TARGET_RT_LITTLE_ENDIAN=1)
target_compile_options(alac PRIVATE -O2 -Wno-everything)

# libplist (C sources only; the C++ wrapper is not needed)
file(GLOB PLIST_SRCS ${LIBPLIST}/src/*.c ${LIBPLIST}/libcnary/*.c)
add_library(plist STATIC ${PLIST_SRCS})
target_include_directories(plist
    PUBLIC ${LIBPLIST}/include
    PRIVATE ${LIBPLIST}/src ${LIBPLIST}/libcnary/include)
target_compile_definitions(plist PRIVATE
    LIBPLIST_STATIC HAVE_GMTIME_R HAVE_LOCALTIME_R HAVE_MEMMEM HAVE_STRNDUP HAVE_STRPTIME
    HAVE_TM_TM_GMTOFF HAVE_TM_TM_ZONE PACKAGE_VERSION="2.7.0")

# OpenSSL libcrypto, prebuilt for this ABI by build-openssl.sh
add_library(crypto STATIC IMPORTED)
set_target_properties(crypto PROPERTIES IMPORTED_LOCATION ${OPENSSL}/lib/libcrypto.a)

# UxPlay's AirPlay server, announced by its own mDNS responder (lib/mdnsd) through the
# Android dnssd backend in place of Bonjour/Avahi
file(GLOB UXPLAY_SRCS
    ${UXPLAY_LIB}/*.c
    ${UXPLAY_LIB}/playfair/*.c
    ${UXPLAY_LIB}/llhttp/*.c)
add_library(airplay STATIC ${UXPLAY_SRCS} ${UXPLAY_LIB}/mdnsd/mdnsd.c dnssd_android.c)
target_include_directories(airplay PUBLIC
    ${UXPLAY_LIB} ${UXPLAY_LIB}/playfair ${UXPLAY_LIB}/llhttp ${OPENSSL}/include)
target_compile_definitions(airplay PUBLIC
    NOHOLD PLIST_210 PLIST_230 LIBPLIST_STATIC OPENSSL_API_COMPAT=0x10101000L
    MDNSD_ALWAYS_UNICAST_REPLY _REENTRANT _LARGEFILE64_SOURCE)
target_compile_options(airplay PRIVATE -O2 -Wno-unused-variable -Wno-unused-function)
target_link_libraries(airplay PUBLIC plist crypto)

add_library(airplaytv SHARED native-lib.c)
target_link_libraries(airplaytv airplay alac log)
