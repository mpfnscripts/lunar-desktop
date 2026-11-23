cmake_minimum_required(VERSION 3.22.1)
project(lunardesktop CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_CXX_FLAGS_RELEASE "-O2")

find_package(OpenXR REQUIRED CONFIG)

add_library(native_app_glue STATIC ${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c)
target_include_directories(native_app_glue PUBLIC ${ANDROID_NDK}/sources/android/native_app_glue)
set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -u ANativeActivity_onCreate")

add_library(lunardesktop SHARED
  main.cpp
  xr.cpp
  gfx.cpp
  font.cpp
  ui.cpp
  scene.cpp
  stream.cpp
  decoder.cpp
  audio.cpp
  platform.cpp
  app.cpp
  stb_impl.cpp)

target_include_directories(lunardesktop PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party
  ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../shared)

target_compile_definitions(lunardesktop PRIVATE XR_USE_PLATFORM_ANDROID XR_USE_GRAPHICS_API_OPENGL_ES)
target_compile_options(lunardesktop PRIVATE -Wall -Wno-unused-function -Wno-missing-braces)

target_link_libraries(lunardesktop
  native_app_glue
  OpenXR::openxr_loader
  android
  log
  EGL
  GLESv3
  mediandk
  aaudio
  nativewindow
  jnigraphics)
