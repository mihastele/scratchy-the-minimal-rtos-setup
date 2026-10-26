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
mrt_add_test(test_queue)
mrt_add_test(test_sem)
mrt_add_test(test_mutex)
mrt_add_test(test_event)
mrt_add_test(test_isr_discipline)
mrt_add_test(test_task_delete)
mrt_add_test(test_pool)
mrt_add_test(test_heap)
mrt_add_test(test_stack)

# Static-only gate: kernel sources and the kernel lib must not touch the
# libc heap. See tools/check_no_heap.sh for the reasoning.
add_test(NAME no_heap_in_kernel
  COMMAND sh ${PROJECT_SOURCE_DIR}/tools/check_no_heap.sh "$<TARGET_FILE:mini_rtos_kernel>")
