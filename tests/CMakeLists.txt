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

# The ALSA capture loop's recovery logic (Linux backend; uses a fake PCM, no ALSA).
if(UNIX AND NOT APPLE)
  qshadow_add_test(tst_alsa_loop)
endif()
