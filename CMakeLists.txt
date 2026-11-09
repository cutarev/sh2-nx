cmake_minimum_required(VERSION 3.20)
project(sh2nx C)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

file(GLOB GEN CONFIGURE_DEPENDS src/recomp/gen/*.c)
file(GLOB RT CONFIGURE_DEPENDS src/runtime/*.c src/win32/*.c src/d3d8/*.c src/game/*.c)

add_executable(sh2 ${GEN} ${RT})
target_include_directories(sh2 PRIVATE src/recomp/gen)
# Lifted code relies on wrapping arithmetic and type punning through guest memory.
target_compile_options(sh2 PRIVATE -fwrapv -fno-strict-aliasing)
set_source_files_properties(${GEN} PROPERTIES COMPILE_OPTIONS "-w")
# The launcher's images are built into the binary (src/d3d8/launcher.c).
file(SHA1 ${CMAKE_SOURCE_DIR}/assets/splash.png SPLASH_SHA1)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS assets/splash.png)
set_source_files_properties(src/d3d8/launcher.c PROPERTIES COMPILE_DEFINITIONS
                            "SRC_DIR=\"${CMAKE_SOURCE_DIR}\";SPLASH_SHA1=\"${SPLASH_SHA1}\""
                            OBJECT_DEPENDS "${CMAKE_SOURCE_DIR}/assets/splash.png;${CMAKE_SOURCE_DIR}/docs/banner.png;${CMAKE_SOURCE_DIR}/assets/font.png")

# The movies (src/win32/bink.c): FFmpeg's Bink decoders. On Fedora: dnf install ffmpeg-free-devel.
find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavformat libavcodec libswscale libswresample libavutil)
target_link_libraries(sh2 PRIVATE PkgConfig::FFMPEG)

if(NINTENDO_SWITCH)
  # Built with the devkitPro toolchain (tools/build-switch.sh); OpenGL comes through EGL and mesa.
  target_include_directories(sh2 PRIVATE ${DEVKITPRO}/portlibs/switch/include)
  target_link_libraries(sh2 PRIVATE SDL2 EGL glapi drm_nouveau stdc++ pthread m)
  # The release NRO: no log (log=1, nxlink), call trace or debugging hooks. -DSH2_LEAN=OFF brings them back.
  option(SH2_LEAN "Switch build without logs, traces or debugging hooks" ON)
  if(SH2_LEAN)
    target_compile_definitions(sh2 PRIVATE SH2_LEAN RECOMP_NO_ICALL_TRACE)
  endif()
  nx_generate_nacp(sh2.nacp NAME "Silent Hill 2" AUTHOR "sh2-nx" VERSION "1.03")
  nx_create_nro(sh2 OUTPUT sh2-nx.nro NACP sh2.nacp NOICON)
else()
  # GL is loaded at run time through SDL (src/d3d8/gl.c), so nothing links libGL.
  find_package(SDL2 REQUIRED)
  target_link_libraries(sh2 PRIVATE SDL2::SDL2 m pthread)
endif()
