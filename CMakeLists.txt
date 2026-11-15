cmake_minimum_required(VERSION 3.16)
project(qshadow VERSION 2.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Multimedia)
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
  yuv2rgb.comp
  yuyv2rgb.comp
)

set(QSHADOW_SPV_DIR "${CMAKE_CURRENT_SOURCE_DIR}/shaders/spv")
set(QSHADOW_SPV_COMPILED FALSE)

if(QSHADOW_SHADER_COMPILE)
  find_program(QSHADOW_SHADER_COMPILER
    NAMES glslc glslangValidator
    HINTS
      "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/tools/glslang"
      "$ENV{VULKAN_SDK}/Bin"
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
#
# Everything except main() lives in a static library so the tests link the very
# same code. Qt resources (the SPIR-V shaders) belong to the executable.

# Platform backend: everything that talks to the operating system's capture stack
# lives behind the interfaces in src/platform/platform.h, one directory per OS.
if(UNIX AND NOT APPLE)
  find_package(ALSA REQUIRED)
  set(QSHADOW_PLATFORM_SOURCES
    src/platform/linux/platform_linux.cpp
    src/platform/linux/sysfs_usb.h src/platform/linux/sysfs_usb.cpp
    src/platform/linux/v4l2_common.h
    src/platform/linux/v4l2_device.h src/platform/linux/v4l2_device.cpp
    src/platform/linux/v4l2_capture.h src/platform/linux/v4l2_capture.cpp
    src/platform/linux/alsa_audio.h src/platform/linux/alsa_audio.cpp
  )
  set(QSHADOW_PLATFORM_LIBS ALSA::ALSA)
elseif(WIN32)
  set(QSHADOW_PLATFORM_SOURCES
    src/platform/windows/platform_windows.cpp
    src/platform/windows/com_common.h src/platform/windows/com_common.cpp
    src/platform/windows/mf_device.h src/platform/windows/mf_device.cpp
    src/platform/windows/mf_capture.h src/platform/windows/mf_capture.cpp
    src/platform/windows/wasapi_audio.h src/platform/windows/wasapi_audio.cpp
  )
  set(QSHADOW_PLATFORM_LIBS mfplat mf mfreadwrite mfuuid ole32 oleaut32 propsys avrt strmiids)
else()
  message(FATAL_ERROR "QShadow has no capture backend for this platform yet "
                      "(see src/platform/platform.h for the interfaces to implement)")
endif()

add_library(qshadow_core STATIC
  src/device.h
  src/capture.h
  src/decoder.h src/decoder.cpp
  src/videoframe.h
  src/videowidget.h src/videowidget.cpp
  src/vulkanrenderer.h src/vulkanrenderer.cpp
  src/fsrconsts.h
  src/audio.h src/audio.cpp
  src/audiodrift.h
  src/audiopipeline.h src/audiopipeline.cpp
  src/platform/platform.h
  src/mainwindow.h src/mainwindow.cpp
  ${QSHADOW_PLATFORM_SOURCES}
)
target_include_directories(qshadow_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(qshadow_core PUBLIC
  Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Multimedia
  JPEG::JPEG Threads::Threads Vulkan::Vulkan ${QSHADOW_PLATFORM_LIBS}
)
if(MSVC)
  target_compile_options(qshadow_core PUBLIC /W4)
else()
  target_compile_options(qshadow_core PUBLIC -Wall -Wextra)
endif()

qt_add_executable(qshadow WIN32
  resources.qrc
  src/main.cpp
)
if(WIN32)
  # Explorer / taskbar icon embedded in the exe.
  target_sources(qshadow PRIVATE icons/qshadow.rc)
endif()

qt_add_resources(qshadow qshadow_spv
  PREFIX "/"
  FILES ${QSHADOW_SPV_FILES}
)

if(QSHADOW_SPV_COMPILED)
  add_dependencies(qshadow qshadow_shaders_spv)
endif()

target_link_libraries(qshadow PRIVATE qshadow_core)

# Windows: put the Qt plugins (platforms/qwindows.dll, ...) next to the exe so it
# starts from the build tree, and ship them with the install.
if(WIN32)
  if(TARGET Qt6::windeployqt)
    add_custom_command(TARGET qshadow POST_BUILD
      COMMAND Qt6::windeployqt --no-translations --no-system-d3d-compiler --no-opengl-sw
              --no-compiler-runtime
              --skip-plugin-types iconengines,generic,networkinformation,tls
              "$<TARGET_FILE:qshadow>"
      COMMENT "Deploying Qt runtime next to qshadow.exe")
  else()
    message(WARNING "Qt6::windeployqt not found; qshadow.exe will not find the Qt plugins")
  endif()
endif()

# ---------------------------------------------------------------------------
# Install
# ---------------------------------------------------------------------------
include(GNUInstallDirs)
install(TARGETS qshadow RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
if(WIN32)
  # The post-build step above left every runtime DLL (vcpkg's applocal copies
  # the dependencies, windeployqt the Qt plugins) next to the exe: ship them all.
  # vulkan-1.dll (the loader, copied because we link it) is deliberately left
  # out: it must come from the GPU driver, a bundled one can shadow it.
  # The exe directory is the build directory (Ninja) and also holds vcpkg_installed/
  # (every dependency, debug and release), the test executables and CMake's own
  # files: only ship what sits next to the exe and in the windeployqt plugin folders.
  install(DIRECTORY "$<TARGET_FILE_DIR:qshadow>/" DESTINATION "${CMAKE_INSTALL_BINDIR}"
          FILES_MATCHING
          PATTERN "vulkan-1*.dll" EXCLUDE
          PATTERN "vcpkg_installed" EXCLUDE
          PATTERN "tests" EXCLUDE
          PATTERN "CMakeFiles" EXCLUDE
          PATTERN "_deps" EXCLUDE
          PATTERN "*.dll")
  install(FILES LICENSE THIRD_PARTY_NOTICES.md DESTINATION ".")
  install(FILES shaders/ATTRIBUTION.md DESTINATION "licenses" RENAME "SHADERS.md")
  install(DIRECTORY licenses/ DESTINATION "licenses")
endif()
if(UNIX AND NOT APPLE)
  install(FILES qshadow.desktop DESTINATION "${CMAKE_INSTALL_DATADIR}/applications")
  install(FILES qshadow.svg
          DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps")
  install(FILES LICENSE THIRD_PARTY_NOTICES.md DESTINATION "${CMAKE_INSTALL_DOCDIR}")
  install(FILES shaders/ATTRIBUTION.md DESTINATION "${CMAKE_INSTALL_DOCDIR}/licenses"
          RENAME "SHADERS.md")
  install(DIRECTORY licenses/ DESTINATION "${CMAKE_INSTALL_DOCDIR}/licenses")
endif()

# ---------------------------------------------------------------------------
# Packaging (cpack): portable ZIP on Windows, plain tar.gz on Linux. The Linux
# AppImage is built from the install tree by CI (see .github/workflows).
# ---------------------------------------------------------------------------
set(CPACK_PACKAGE_NAME "qshadow")
set(CPACK_PACKAGE_VENDOR "QShadow")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
    "Low-latency viewer for USB video capture devices")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
if(WIN32)
  set(CPACK_GENERATOR ZIP)
  set(CPACK_PACKAGE_FILE_NAME "qshadow-${PROJECT_VERSION}-windows-x64")
else()
  set(CPACK_GENERATOR TGZ)
  set(CPACK_PACKAGE_FILE_NAME "qshadow-${PROJECT_VERSION}-linux-${CMAKE_SYSTEM_PROCESSOR}")
endif()
include(CPack)

# ---------------------------------------------------------------------------
# Tests (no dongle, GPU or display needed): ctest --test-dir build
# ---------------------------------------------------------------------------
option(QSHADOW_BUILD_TESTS "Build the unit tests (needs Qt6 Test)" ON)
if(QSHADOW_BUILD_TESTS)
  find_package(Qt6 QUIET COMPONENTS Test)
  if(Qt6Test_FOUND)
    enable_testing()
    add_subdirectory(tests)
  else()
    message(STATUS "Qt6 Test not found; skipping unit tests")
  endif()
endif()
