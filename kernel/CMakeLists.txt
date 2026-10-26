# Portable kernel library: no hardware, no heap, no surprises.
add_library(mini_rtos_kernel STATIC task.c sched.c)
target_include_directories(mini_rtos_kernel
  PUBLIC ${PROJECT_SOURCE_DIR}/include  # Public API: mini_rtos/*.h
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})  # kernel_priv.h stays private
# Using the kernel requires a port (it calls mrt_port_*), so propagate the
# link: executables that link the kernel get the port automatically.
target_link_libraries(mini_rtos_kernel PUBLIC mini_rtos_port)
