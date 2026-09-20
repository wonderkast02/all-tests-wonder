cmake_minimum_required(VERSION 3.22)
project(G720VkLoaderCheck C)

if(NOT WIN32)
  message(FATAL_ERROR "G720VkLoaderCheck is a Windows-only lab tool")
endif()

if(NOT DEFINED VULKAN_HEADERS_INCLUDE)
  message(FATAL_ERROR "VULKAN_HEADERS_INCLUDE must point to Vulkan-Headers/include")
endif()

add_executable(G720VkLoaderCheck loader_check.c)
target_include_directories(G720VkLoaderCheck PRIVATE "${VULKAN_HEADERS_INCLUDE}")
target_compile_definitions(G720VkLoaderCheck PRIVATE UNICODE _UNICODE)
target_link_libraries(G720VkLoaderCheck PRIVATE user32)

if(MSVC)
  target_compile_options(G720VkLoaderCheck PRIVATE /W4 /WX /permissive- /utf-8)
endif()
