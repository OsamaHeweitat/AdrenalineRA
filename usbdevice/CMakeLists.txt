cmake_minimum_required(VERSION 3.28)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
  else()
    message(FATAL_ERROR "Please define VITASDK to point to your SDK path!")
  endif()
endif()

project(usbdevice)
set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
include("${VITASDK}/share/vita.cmake" REQUIRED)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wl,-q -Wall -O3 -nostdlib")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-rtti -fno-exceptions")

add_executable(usbdevice
  main.c
)

target_link_libraries(usbdevice
  SceUdcdForDriver_stub
  SceIofilemgrForDriver_stub
  SceSysclibForDriver_stub
  taihenForKernel_stub
)

vita_create_self(usbdevice.skprx usbdevice CONFIG exports.yml UNSAFE)

add_custom_target(usbdevice_all
  ALL
  DEPENDS adrenaline_kernel.skprx-self
)
