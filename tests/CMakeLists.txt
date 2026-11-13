# Each test is a small Qt Test executable linked against the real code.
function(qshadow_add_test name)
  add_executable(${name} ${name}.cpp)
  target_link_libraries(${name} PRIVATE qshadow_core Qt6::Test)
  add_test(NAME ${name} COMMAND ${name})
  # No display or GPU is needed; make sure Qt never tries to open one.
  set_tests_properties(${name} PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endfunction()

qshadow_add_test(tst_audio)
qshadow_add_test(tst_pipeline)
qshadow_add_test(tst_sizing)
qshadow_add_test(tst_platform)
qshadow_add_test(tst_mainwindow)

# Windows: the test exe sits in tests/, away from the platforms/ plugin dir that
# windeployqt put next to qshadow.exe, and windeployqt never deploys the offscreen
# plugin that the tests select. Without them QApplication cannot start (the test hung
# at startup on CI), so deploy both next to the test.
if(WIN32)
  if(TARGET Qt6::windeployqt)
    add_custom_command(TARGET tst_mainwindow POST_BUILD
      COMMAND Qt6::windeployqt --no-translations --no-system-d3d-compiler --no-opengl-sw
              --no-compiler-runtime "$<TARGET_FILE:tst_mainwindow>")
  endif()
  if(TARGET Qt6::QOffscreenIntegrationPlugin)
    add_custom_command(TARGET tst_mainwindow POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:tst_mainwindow>/platforms"
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
              "$<TARGET_FILE:Qt6::QOffscreenIntegrationPlugin>"
              "$<TARGET_FILE_DIR:tst_mainwindow>/platforms/")
  else()
    message(WARNING "Qt6 offscreen plugin target not found; tst_mainwindow will use the default platform")
    set_tests_properties(tst_mainwindow PROPERTIES ENVIRONMENT "")
  endif()
endif()

# The ALSA capture loop's recovery logic (Linux backend; uses a fake PCM, no ALSA).
if(UNIX AND NOT APPLE)
  qshadow_add_test(tst_alsa_loop)
endif()
