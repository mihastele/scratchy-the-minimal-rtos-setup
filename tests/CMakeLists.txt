# One binary per focused test; each returns nonzero on failure so CTest
# reports red. Plain C, no external framework — the suite must build
# anywhere the kernel builds.
function(mrt_add_test name)
  add_executable(${name} ${name}.c)
  target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}) # test_hooks.h
  target_link_libraries(${name} PRIVATE mini_rtos_kernel mini_rtos_port)
  add_test(NAME ${name} COMMAND ${name})
endfunction()

mrt_add_test(test_task_create)
mrt_add_test(test_sched_order)
mrt_add_test(test_suspend_resume)
mrt_add_test(test_priority)
mrt_add_test(test_delay_until)
mrt_add_test(test_idle_hook)
