cmake_minimum_required(VERSION 3.16)
project(LunarDesktopPC LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_definitions(UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS _WIN32_WINNT=0x0A00)

if(MSVC)
  add_compile_options(/W3 /utf-8 /EHsc)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
else()
  add_compile_options(-Wall -Wno-unused-function -Wno-unknown-pragmas -Wno-missing-field-initializers)
endif()

set(OUT_DIR ${CMAKE_BINARY_DIR}/LunarDesktop)

# ---------------- SteamVR driver ----------------
add_library(driver_lunar SHARED driver/driver.cpp driver/compositor.cpp)
target_include_directories(driver_lunar PRIVATE third_party/openvr)
target_link_libraries(driver_lunar PRIVATE d3d11 dxgi winmm)
set_target_properties(driver_lunar PROPERTIES PREFIX "" OUTPUT_NAME driver_lunar
  RUNTIME_OUTPUT_DIRECTORY ${OUT_DIR}/driver/lunar/bin/win64
  RUNTIME_OUTPUT_DIRECTORY_RELEASE ${OUT_DIR}/driver/lunar/bin/win64
  LIBRARY_OUTPUT_DIRECTORY ${OUT_DIR}/driver/lunar/bin/win64)
if(NOT MSVC)
  target_link_options(driver_lunar PRIVATE -static-libgcc -static-libstdc++ -static)
endif()

# ---------------- Host app ----------------
add_executable(LunarDesktop WIN32
  host/main.cpp host/app.cpp host/net.cpp host/encoder.cpp host/capture.cpp host/audio.cpp
  host/input.cpp host/steamvr.cpp host/settings.cpp host/ui_server.cpp host/log.cpp host/host.rc)
target_include_directories(LunarDesktop PRIVATE third_party/openvr host)
target_link_libraries(LunarDesktop PRIVATE d3d11 dxgi mfplat mfuuid mf ole32 oleaut32 uuid ws2_32 winmm shell32 user32 advapi32 strmiids)
set_target_properties(LunarDesktop PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${OUT_DIR}
  RUNTIME_OUTPUT_DIRECTORY_RELEASE ${OUT_DIR})
if(MSVC)
  target_link_options(LunarDesktop PRIVATE /MANIFEST:NO)
else()
  target_link_options(LunarDesktop PRIVATE -municode -static-libgcc -static-libstdc++ -static)
endif()
# The UI file is embedded as a resource; rebuild when it changes.
set_source_files_properties(host/host.rc PROPERTIES OBJECT_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/host/ui/index.html)

# Driver resources + openvr_api.dll next to the exe
add_custom_command(TARGET driver_lunar POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/driver/driver.vrdrivermanifest ${OUT_DIR}/driver/lunar/driver.vrdrivermanifest
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/driver/resources ${OUT_DIR}/driver/lunar/resources)
add_custom_command(TARGET LunarDesktop POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/third_party/openvr/openvr_api.dll ${OUT_DIR}/openvr_api.dll)
