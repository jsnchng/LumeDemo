cmake_minimum_required(VERSION 3.24)
project(LumeDemo)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

#
# Target: Vulkan. Install VulkanSDK from https://vulkan.lunarg.com/sdk/home#windows.
#
find_package(Vulkan REQUIRED)
set_target_properties(Vulkan::Vulkan PROPERTIES INTERFACE_COMPILE_DEFINITIONS VK_USE_PLATFORM_WIN32_KHR=1)

#
# Target: AGPBaseAPI
#
if(NOT TARGET AGPBase::AGPBaseAPI)
  add_subdirectory(LumeBase)
endif()

#
# Target: AGPEngineAPI
#
if(NOT TARGET AGPEngine::AGPEngineAPI)
  add_subdirectory(LumeEngine)
  set(ENGINE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/LumeEngine")
endif()

#
# Target: AGPRenderAPI
#
if(NOT TARGET AGPRender::AGPRenderAPI)
  add_subdirectory(LumeRender)
  set(RENDER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/LumeRender")
endif()

#
# Target: AGP3DAPI
#
if(NOT TARGET AGP3D::AGP3DAPI)
  add_subdirectory(Lume3D)
  set(3D_DIR "${CMAKE_CURRENT_SOURCE_DIR}/Lume3D")
endif()

#
# Target: LumePTAPI
#
if(NOT TARGET LumePT::LumePTAPI)
  add_subdirectory(LumePluginTemplate)
endif()

#
# Extract LumeShaderCompiler.exe from LumeRender
#
if(NOT EXISTS "${CMAKE_BINARY_DIR}/LumeShaderCompiler.exe")
  if(NOT EXISTS "${RENDER_DIR}/tools/LumeShaderCompiler.zip")
    message(FATAL_ERROR "Please run:\n"
      "git submodule update --init\n"
      "and then re-run cmake.")
  else()
    # Cmake 3.20+
    file(ARCHIVE_EXTRACT
      INPUT "${RENDER_DIR}/tools/LumeShaderCompiler.zip"
      DESTINATION "${CMAKE_BINARY_DIR}")
  endif()
endif()

set_target_properties(AGPEngineDLL PROPERTIES
  ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/$<CONFIG>
  LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/$<CONFIG>
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/$<CONFIG>)

set_target_properties(PluginAGPRender PROPERTIES
  ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/$<CONFIG>/plugins
  LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/$<CONFIG>/plugins
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/$<CONFIG>/plugins)

set_target_properties(PluginAGP3D PROPERTIES
  ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/$<CONFIG>/plugins
  LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/$<CONFIG>/plugins
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/$<CONFIG>/plugins)

set_target_properties(PluginLumePT PROPERTIES
  ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/$<CONFIG>/plugins
  LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/$<CONFIG>/plugins
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/$<CONFIG>/plugins)

#
# Target: glfw
#
if(NOT TARGET glfw)
  add_subdirectory(third_party/glfw-3.4 glfw)
endif()

#
# Target: DemoCommon
#
add_library(DemoCommon
  ${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp)
target_include_directories(DemoCommon PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(DemoCommon PRIVATE
  AGPBase::AGPBaseAPI
  AGPEngine::AGPEngineAPI
  AGPRender::AGPRenderAPI)
target_link_libraries(DemoCommon PUBLIC
  Vulkan::Vulkan
  glfw)
target_compile_definitions(DemoCommon PUBLIC RENDER_HAS_VULKAN_BACKEND=1)
target_compile_definitions(DemoCommon PUBLIC NOMINMAX)

#
# Target: LumeDemo.exe
#
add_executable(LumeDemo src/application.cpp)
add_dependencies(LumeDemo
  PluginAGPRender
  PluginAGP3D
  PluginLumePT
  DemoCommon)
target_link_libraries(LumeDemo PRIVATE
  AGPBase::AGPBaseAPI
  AGPEngine::AGPEngineAPI
  AGPRender::AGPRenderAPI
  AGP3D::AGP3DAPI
  LumePT::LumePTAPI
  AGPEngine::AGPEngineDLL
  AGPEngine::ComponentHelperSDK
  DemoCommon)

add_custom_target(CopyAssets ALL
  COMMAND ${CMAKE_COMMAND} -E copy_directory
          ${CMAKE_SOURCE_DIR}/assets/glTF
          ${CMAKE_BINARY_DIR}/$<CONFIG>/app/assets/glTF
  COMMAND ${CMAKE_COMMAND} -E copy_directory
          ${CMAKE_SOURCE_DIR}/LumeRender/assets/render
          ${CMAKE_BINARY_DIR}/$<CONFIG>/app/assets/render
  COMMAND ${CMAKE_COMMAND} -E copy_directory
          ${CMAKE_SOURCE_DIR}/Lume3D/assets/3d
          ${CMAKE_BINARY_DIR}/$<CONFIG>/app/assets/3d
  COMMAND ${CMAKE_COMMAND} -E copy_directory
          ${CMAKE_SOURCE_DIR}/LumePluginTemplate/assets/pt
          ${CMAKE_BINARY_DIR}/$<CONFIG>/app/assets/pt)
add_dependencies(CopyAssets LumeDemo)
