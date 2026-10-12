cmake_minimum_required(VERSION 3.20)
project(AirPlayXbox C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_C_STANDARD 11)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")

set(VCPKG_PREFIX "${CMAKE_SOURCE_DIR}/vcpkg_installed/x64-uwp" CACHE PATH "vcpkg install directory for x64-uwp")
list(APPEND CMAKE_PREFIX_PATH "${VCPKG_PREFIX}")

add_compile_options(/utf-8)
add_compile_definitions(_DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR)
add_compile_definitions(
    _CRT_SECURE_NO_WARNINGS _WINSOCK_DEPRECATED_NO_WARNINGS _CRT_NONSTDC_NO_WARNINGS
    NOMINMAX WIN32_LEAN_AND_MEAN)

# ---------------------------------------------------------------- libplist
file(GLOB PLIST_SRC third_party/libplist/src/*.c third_party/libplist/libcnary/node*.c)
add_library(plist STATIC ${PLIST_SRC})
target_include_directories(plist PUBLIC third_party/libplist/include
    PRIVATE third_party/libplist/src third_party/libplist/libcnary/include)
target_compile_definitions(plist PUBLIC LIBPLIST_STATIC PRIVATE PACKAGE_VERSION="2.7.0")
target_compile_options(plist PRIVATE /wd4244 /wd4267 /wd4996 /wd4090)

# ---------------------------------------------------------------- UxPlay lib (AirPlay protocol)
find_package(OpenSSL REQUIRED)
file(GLOB UXPLAY_SRC
    third_party/uxplay-lib/*.c
    third_party/uxplay-lib/playfair/*.c
    third_party/uxplay-lib/llhttp/*.c
    third_party/uxplay-lib/mdnsd/*.c)
add_library(uxplay STATIC ${UXPLAY_SRC})
target_include_directories(uxplay
    PUBLIC third_party/uxplay-lib
    PRIVATE third_party/compat third_party/uxplay-lib/playfair third_party/uxplay-lib/llhttp)
target_compile_definitions(uxplay PUBLIC WIN32 STANDALONE TARGET_POSIX NOHOLD)
target_compile_options(uxplay PRIVATE /FIuxplay_win_fixes.h /wd4244 /wd4267 /wd4996 /wd4018 /wd4146 /wd4047 /wd4024)
target_link_libraries(uxplay PUBLIC plist OpenSSL::SSL OpenSSL::Crypto ws2_32 iphlpapi)

# ---------------------------------------------------------------- App
foreach(lib avcodec avutil swresample)
    find_library(FFMPEG_${lib} NAMES ${lib} PATHS "${VCPKG_PREFIX}/lib" NO_DEFAULT_PATH REQUIRED)
endforeach()

file(GLOB APP_SRC src/*.cpp)
add_executable(AirPlayXbox WIN32 ${APP_SRC})
target_include_directories(AirPlayXbox PRIVATE src third_party/compat "${VCPKG_PREFIX}/include")
target_compile_options(AirPlayXbox PRIVATE /EHsc /permissive- /bigobj /W3)
target_link_libraries(AirPlayXbox PRIVATE uxplay
    ${FFMPEG_avcodec} ${FFMPEG_swresample} ${FFMPEG_avutil}
    windowsapp d3d11 dxgi d2d1 dwrite xaudio2 ws2_32 iphlpapi bcrypt)
target_link_options(AirPlayXbox PRIVATE /MAP /DEBUG /OPT:REF /OPT:ICF)
