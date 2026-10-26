# Host simulator port (POSIX ucontext + virtual tick). Host-only.
add_library(mini_rtos_port STATIC port.c)
target_include_directories(mini_rtos_port PUBLIC ${PROJECT_SOURCE_DIR}/include)
# _XOPEN_SOURCE exposes ucontext (getcontext/makecontext/swapcontext) even
# under strict -std=c11. MCU ports never define this.
target_compile_definitions(mini_rtos_port PRIVATE _XOPEN_SOURCE=700)
