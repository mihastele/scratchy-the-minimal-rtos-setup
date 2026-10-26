# Small demos, one per concept. Each is also a port smoke test: the same
# interleave must appear on every chip.
add_executable(blinky_host blinky_host.c)
target_link_libraries(blinky_host PRIVATE mini_rtos_kernel mini_rtos_port)
