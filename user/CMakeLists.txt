cmake_minimum_required(VERSION 3.2)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
  else()
    message(FATAL_ERROR "Please define VITASDK to point to your SDK path!")
  endif()
endif()

project(adrenaline_user)
include("${VITASDK}/share/vita.cmake" REQUIRED)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wl,-q -Wall -O3 -nostartfiles -fno-lto")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-rtti -fno-exceptions")

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

# Ensure RC_CLIENT_SUPPORTS_HASH is defined so rcheevos hash support is enabled
add_definitions(-DRC_CLIENT_SUPPORTS_HASH)

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
  lz4/lz4.c
  retroachievements.c
  virtualsfo.c
  ${RCHEEVOS_SOURCES}  # Add rcheevos source files
  dummy_fini.c
)

target_link_directories(adrenaline_user PRIVATE ${CMAKE_BINARY_DIR}/kernel)
target_link_libraries(adrenaline_user
  AdrenalineKernel_stub
  taihen_stub
  vita2d
  z
  png
  jpeg
  vitashaders
  c
  m
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
  pthread
  m
  dl
  z
  # ${LUA_LIBRARIES}
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

