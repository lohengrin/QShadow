cmake_minimum_required(VERSION 3.16)
project(qshadow VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets OpenGLWidgets Multimedia)
find_package(ALSA REQUIRED)
find_package(JPEG REQUIRED)
find_package(OpenGL REQUIRED)
find_package(Threads REQUIRED)

qt_add_executable(qshadow
  src/main.cpp
  src/device.h src/device.cpp
  src/capture.h src/capture.cpp
  src/decoder.h src/decoder.cpp
  src/videowidget.h src/videowidget.cpp
  src/audio.h src/audio.cpp
  src/mainwindow.h src/mainwindow.cpp
)

target_link_libraries(qshadow PRIVATE
  Qt6::Core Qt6::Gui Qt6::Widgets Qt6::OpenGLWidgets Qt6::Multimedia
  ALSA::ALSA JPEG::JPEG OpenGL::GL Threads::Threads
)

target_compile_options(qshadow PRIVATE -Wall -Wextra)