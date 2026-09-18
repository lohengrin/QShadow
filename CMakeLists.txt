cmake_minimum_required(VERSION 3.16)
project(qshadow VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Multimedia)
find_package(ALSA REQUIRED)
find_package(JPEG REQUIRED)
find_package(Threads REQUIRED)
find_package(Vulkan REQUIRED)

# ---------------------------------------------------------------------------
# SPIR-V shaders
#
# The shader set is compiled to SPIR-V here when a compiler is available and
# otherwise the committed copies in shaders/spv/ are used, so a build never
# depends on having the shader toolchain installed.
# ---------------------------------------------------------------------------
option(QSHADOW_SHADER_COMPILE
       "Compile GLSL shaders to SPIR-V at build time (needs glslc/glslangValidator)" ON)

set(QSHADOW_SHADERS
  fullscreen.vert
  passthrough.frag
  hq4x.frag
  xbrz.frag
  crt-geom.frag
  crt-royale.frag
  fsr_easu.comp
  fsr_rcas.comp
)

set(QSHADOW_SPV_DIR "${CMAKE_CURRENT_SOURCE_DIR}/shaders/spv")
set(QSHADOW_SPV_COMPILED FALSE)

if(QSHADOW_SHADER_COMPILE)
  find_program(QSHADOW_SHADER_COMPILER
    NAMES glslc glslangValidator
    HINTS
      "$ENV{ANDROID_NDK_HOME}/shader-tools/linux-x86_64"
      "$ENV{ANDROID_HOME}/emulator/lib64/vulkan"
      "$ENV{ANDROID_SDK_ROOT}/emulator/lib64/vulkan")
endif()

if(QSHADOW_SHADER_COMPILER)
  message(STATUS "Compiling shaders with ${QSHADOW_SHADER_COMPILER}")
  set(QSHADOW_SPV_DIR "${CMAKE_CURRENT_BINARY_DIR}/spv")
  file(MAKE_DIRECTORY "${QSHADOW_SPV_DIR}")
  get_filename_component(QSHADOW_COMPILER_NAME "${QSHADOW_SHADER_COMPILER}" NAME)
  add_custom_target(qshadow_shaders_spv)

  foreach(shader IN LISTS QSHADOW_SHADERS)
    get_filename_component(_ext "${shader}" EXT)
    string(SUBSTRING "${_ext}" 1 -1 _stage)
    set(_src "${CMAKE_CURRENT_SOURCE_DIR}/shaders/${shader}")
    set(_out "${QSHADOW_SPV_DIR}/${shader}.spv")
    if(QSHADOW_COMPILER_NAME MATCHES "glslc")
      add_custom_command(OUTPUT "${_out}"
        COMMAND "${QSHADOW_SHADER_COMPILER}" "-fshader-stage=${_stage}"
                "-I" "${CMAKE_CURRENT_SOURCE_DIR}/third_party/fsr"
                -o "${_out}" "${_src}"
        DEPENDS "${_src}"
                "${CMAKE_CURRENT_SOURCE_DIR}/third_party/fsr/ffx_a.h"
                "${CMAKE_CURRENT_SOURCE_DIR}/third_party/fsr/ffx_fsr1.h"
        VERBATIM)
    else()
      add_custom_command(OUTPUT "${_out}"
        COMMAND "${QSHADOW_SHADER_COMPILER}" -V -S "${_stage}"
                "-I${CMAKE_CURRENT_SOURCE_DIR}/third_party/fsr"
                -o "${_out}" "${_src}"
        DEPENDS "${_src}"
                "${CMAKE_CURRENT_SOURCE_DIR}/third_party/fsr/ffx_a.h"
                "${CMAKE_CURRENT_SOURCE_DIR}/third_party/fsr/ffx_fsr1.h"
        VERBATIM)
    endif()
    add_custom_target(qshadow_shader_${shader} DEPENDS "${_out}")
    add_dependencies(qshadow_shaders_spv qshadow_shader_${shader})
  endforeach()
  set(QSHADOW_SPV_COMPILED TRUE)
else()
  message(STATUS "No glslc/glslangValidator found; using committed SPIR-V in shaders/spv/")
endif()

set(QSHADOW_SPV_FILES)
foreach(shader IN LISTS QSHADOW_SHADERS)
  set(_out "${QSHADOW_SPV_DIR}/${shader}.spv")
  list(APPEND QSHADOW_SPV_FILES "${_out}")
  set_source_files_properties("${_out}" PROPERTIES
    QT_RESOURCE_ALIAS "shaders/${shader}.spv")
endforeach()

# ---------------------------------------------------------------------------
# Application
# ---------------------------------------------------------------------------
qt_add_executable(qshadow
  resources.qrc
  src/main.cpp
  src/device.h src/device.cpp
  src/capture.h src/capture.cpp
  src/decoder.h src/decoder.cpp
  src/videowidget.h src/videowidget.cpp
  src/vulkanrenderer.h src/vulkanrenderer.cpp
  src/fsrconsts.h
  src/audio.h src/audio.cpp
  src/mainwindow.h src/mainwindow.cpp
)

qt_add_resources(qshadow qshadow_spv
  PREFIX "/"
  FILES ${QSHADOW_SPV_FILES}
)

if(QSHADOW_SPV_COMPILED)
  add_dependencies(qshadow qshadow_shaders_spv)
endif()

target_link_libraries(qshadow PRIVATE
  Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Multimedia
  ALSA::ALSA JPEG::JPEG Threads::Threads Vulkan::Vulkan
)

target_compile_options(qshadow PRIVATE -Wall -Wextra)
