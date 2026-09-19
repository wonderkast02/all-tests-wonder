# SPDX-License-Identifier: MIT
cmake_minimum_required(VERSION 3.22)
project(DriveGpuLab VERSION 0.1.0 LANGUAGES C)

include(CTest)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

option(DGL_WARNINGS_AS_ERRORS "Treat Drive GPU Lab warnings as errors" ON)

add_library(dgl_common STATIC
  src/dgl_ring.c
  src/common.c
  src/sha256.c
)
target_include_directories(dgl_common PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${CMAKE_CURRENT_SOURCE_DIR}/src
)

function(dgl_strict target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus)
    if(DGL_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE /WX)
    endif()
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    if(DGL_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
  endif()
endfunction()

dgl_strict(dgl_common)

if(WIN32)
  include(FetchContent)
  FetchContent_Declare(
    VulkanHeaders
    GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers.git
    # Vulkan-Headers v1.4.357, pinned to the immutable commit behind the tag.
    GIT_TAG e3b1eec08173d6b825cd3ac88c885a63b621504a
    GIT_SHALLOW TRUE
  )
  FetchContent_MakeAvailable(VulkanHeaders)

  add_executable(G720Probe WIN32
    src/probe_win32.c
    src/probe_win.rc
  )
  target_include_directories(G720Probe PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
  target_link_libraries(G720Probe PRIVATE
    dgl_common
    ws2_32
    psapi
    user32
    gdi32
    version
    shell32
  )
  dgl_strict(G720Probe)

  add_library(G720VkLayer SHARED layer/vk_layer_drive.c)
  target_include_directories(G720VkLayer PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${vulkanheaders_SOURCE_DIR}/include
  )
  target_link_libraries(G720VkLayer PRIVATE ws2_32)
  set_target_properties(G720VkLayer PROPERTIES PREFIX "" OUTPUT_NAME "G720VkLayer")
  dgl_strict(G720VkLayer)
  if(MSVC)
    # Vulkan loader dispatch intentionally converts the generic PFN_vkVoidFunction
    # returned by vkGet*ProcAddr to the exact command prototype required by the ABI.
    target_compile_options(G720VkLayer PRIVATE /wd4191)
  endif()

  configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/layer/VK_LAYER_DRIVE_G720_telemetry.json.in
    ${CMAKE_CURRENT_BINARY_DIR}/VK_LAYER_DRIVE_G720_telemetry.json
    COPYONLY
  )
  add_custom_command(TARGET G720VkLayer POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      ${CMAKE_CURRENT_BINARY_DIR}/VK_LAYER_DRIVE_G720_telemetry.json
      $<TARGET_FILE_DIR:G720VkLayer>/VK_LAYER_DRIVE_G720_telemetry.json
  )
else()
  add_executable(g720-hostd src/hostd_posix.c)
  target_include_directories(g720-hostd PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
  target_link_libraries(g720-hostd PRIVATE dgl_common)
  dgl_strict(g720-hostd)
endif()

if(BUILD_TESTING)
  add_executable(test_ring tests/test_ring.c)
  target_link_libraries(test_ring PRIVATE dgl_common)
  dgl_strict(test_ring)
  add_test(NAME ring COMMAND test_ring)

  add_executable(test_common tests/test_common.c)
  target_link_libraries(test_common PRIVATE dgl_common)
  dgl_strict(test_common)
  add_test(NAME common COMMAND test_common)
endif()
