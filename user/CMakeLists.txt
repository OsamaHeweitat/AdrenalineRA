cmake_minimum_required(VERSION 3.28)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
  else()
    message(FATAL_ERROR "Please define VITASDK to point to your SDK path!")
  endif()
endif()

project(adrenaline_user)
set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
include("${VITASDK}/share/vita.cmake" REQUIRED)

include(ExternalProject)

if(DEFINED NIGHTLY AND NIGHTLY EQUAL 1)
	set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wl,-q -Wall -O3 -nostartfiles -fno-lto -nostdlib -fno-builtin -DNIGHTLY=1")
else()
	set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wl,-q -Wall -O3 -nostartfiles -fno-lto -nostdlib -fno-builtin")
endif()
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-rtti -fno-exceptions")

ExternalProject_Add(libvita2dfbo
    DOWNLOAD_DIR ${CMAKE_CURRENT_BINARY_DIR}
    GIT_REPOSITORY https://github.com/frangarcj/vita2dlib.git
    GIT_TAG        fbo
    SOURCE_DIR ${CMAKE_CURRENT_BINARY_DIR}/libvita2dfbo
    BUILD_IN_SOURCE 1
    CONFIGURE_COMMAND ""
    BUILD_COMMAND make -C libvita2d
    INSTALL_COMMAND ""
)

add_library(libvita2dfbo_libA STATIC IMPORTED GLOBAL)
set_property(TARGET libvita2dfbo_libA
    PROPERTY IMPORTED_LOCATION
    ${CMAKE_CURRENT_BINARY_DIR}/libvita2dfbo/libvita2d/libvita2d.a
)

ExternalProject_Add(vitashadercollection
    DOWNLOAD_DIR ${CMAKE_CURRENT_BINARY_DIR}
    URL https://github.com/VitaArchive/vita-shader-collection/releases/download/master-0.2-v1767160600/vita-shader-collection.tar.gz
    SOURCE_DIR ${CMAKE_CURRENT_BINARY_DIR}/vitashadercollection
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    BUILD_IN_SOURCE 1
    CONFIGURE_COMMAND ""
    BUILD_COMMAND ""
    INSTALL_COMMAND ""
)

add_library(vitashadercollection_libA STATIC IMPORTED GLOBAL)
set_property(TARGET vitashadercollection_libA
    PROPERTY IMPORTED_LOCATION
    ${CMAKE_CURRENT_BINARY_DIR}/vitashadercollection/lib/libvitashaders.a
)


# Builds
FUNCTION(ADD_RESOURCES out_var)
  SET(result)
  FOREACH(in_f ${ARGN})
  SET(out_f "${CMAKE_CURRENT_BINARY_DIR}/${in_f}.o")
  GET_FILENAME_COMPONENT(out_dir ${out_f} DIRECTORY)
  ADD_CUSTOM_COMMAND(OUTPUT ${out_f}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${out_dir}
    COMMAND ${CMAKE_LINKER} -r -b binary -o ${out_f} ${in_f}
    DEPENDS ${in_f}
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Building resource ${out_f}"
    VERBATIM
    )
  LIST(APPEND result ${out_f})
  ENDFOREACH()
  SET(${out_var} "${result}" PARENT_SCOPE)
ENDFUNCTION()

file(GLOB res_files RELATIVE
  ${CMAKE_CURRENT_SOURCE_DIR}
  flash0/kd/*.prx
  flash0/kd/*.bin
  flash0/vsh/module/*.prx
  flash0/*.bin
)
add_resources(cef_res ${res_files})

set(LUA_INCLUDE_DIR "/usr/local/vitasdk/arm-vita-eabi/include/luajit-2.1")
# set(LUA_LIBRARIES "/path/to/lua/lib/liblua.so")

# Add the rcheevos include directory
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/rcheevos/include
${CMAKE_CURRENT_SOURCE_DIR}/rcheevos/src
${CMAKE_CURRENT_SOURCE_DIR}/rcheevos/src/rapi
${CMAKE_CURRENT_SOURCE_DIR}/rcheevos/src/rcheevos
${CMAKE_CURRENT_SOURCE_DIR}/rcheevos/src/rhash
${LUA_INCLUDE_DIR}
)

# Gather the rcheevos source files
file(GLOB RCHEEVOS_SOURCES
  ${CMAKE_CURRENT_SOURCE_DIR}/rcheevos/src/*.c
  ${CMAKE_CURRENT_SOURCE_DIR}/rcheevos/src/rapi/*.c
  ${CMAKE_CURRENT_SOURCE_DIR}/rcheevos/src/rcheevos/*.c
  ${CMAKE_CURRENT_SOURCE_DIR}/rcheevos/src/rhash/*.c
)

# Ensure RC_CLIENT_SUPPORTS_HASH is defined so rcheevos hash support is enabled.
# RC_NO_THREADS avoids pulling Vita pthread/newlib thread glue into this module.
add_definitions(-DRC_CLIENT_SUPPORTS_HASH -DRC_NO_THREADS)

add_executable(adrenaline_user
  ${cef_res}
  main.c
  pops.c
  titleinfo.c
  flashfs.c
  msfs.c
  menu.c
  states.c
  usb.c
  utils.c
  math_utils.c
  retroachievements_network.c
  retroachievements_iso_parser.c
  retroachievements_cache.c
  retroachievements_ui.c
  retroachievements_credentials.c
  lz4/lz4.c
  retroachievements.c
  virtualsfo.c
  vita_newlib_compat.c
  ${RCHEEVOS_SOURCES}  # Add rcheevos source files
  dummy_fini.c
  lodepng/lodepng.c
)

add_dependencies(adrenaline_user libvita2dfbo)
add_dependencies(adrenaline_user vitashadercollection)

target_include_directories(adrenaline_user PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/libvita2dfbo/libvita2d/include)
target_include_directories(adrenaline_user PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/vitashadercollection/includes)
target_link_directories(adrenaline_user PRIVATE ${CMAKE_BINARY_DIR}/kernel)
target_link_libraries(adrenaline_user
  AdrenalineKernel_stub
  taihen_stub
  libvita2dfbo_libA
  z
  png
  jpeg
  vitashadercollection_libA
  SceRtabi_stub
  SceLibc_stub
  SceAppMgr_stub
  SceAppUtil_stub
  SceAudio_stub
  SceCommonDialog_stub
  SceCompat_stub
  SceCtrl_stub
  SceDisplay_stub
  SceKernelDmacMgr_stub
  SceGxm_stub
  SceHttp_stub_weak
  SceIofilemgr_stub
  SceKernelModulemgr_stub
  SceKernelThreadMgr_stub
  SceLibKernel_stub
  SceMtpIfDriver_stub
  SceNet_stub_weak
  SceNetCtl_stub_weak
  SceNotificationUtil_stub_weak
  ScePgf_stub_weak
  ScePower_stub
  SceProcessmgr_stub
  SceRtc_stub
  SceSblSsMgr_stub 
  SceScreenShot_stub_weak
  SceShellSvc_stub
  SceSsl_stub_weak
  SceSysmem_stub
  SceSysmodule_stub
  SceUdcd_stub
  SceUsbstorVStorDriver_stub
  SceVshBridge_stub
  SceLiveArea_stub_weak
  luajit-5.1
  m
  dl
  z
  # ${LUA_LIBRARIES}
  SceAVConfig_stub
)

vita_create_self(adrenaline_user.suprx adrenaline_user CONFIG exports.yml UNSAFE)

#vita_create_stubs(ustubs adrenaline_user ${CMAKE_CURRENT_SOURCE_DIR}/exports.yml)

#add_custom_target(copy
#  COMMAND cp adrenaline_user.suprx F:/app/PSPEMUCFW/sce_module/adrenaline_user.suprx
#  DEPENDS adrenaline_user.suprx
#)

#add_custom_target(pkg
#  COMMAND cp adrenaline_user.suprx ../../bubble/pkg/sce_module/adrenaline_user.suprx
#  DEPENDS adrenaline_user.suprx
#)

#add_custom_target(updater
#  COMMAND bin2c adrenaline_user.suprx ../../cef/updater/adrenaline_user.h adrenaline_user
#  DEPENDS adrenaline_user.suprx
#)

add_custom_target(user_all
  ALL
  DEPENDS adrenaline_user.suprx-self
)
