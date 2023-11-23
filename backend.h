#pragma once

void glfw_error_callback(int error, const char* description);

#ifdef __cplusplus
extern "C" {
#endif

void main_loop(void (*user_function)());

#ifdef __cplusplus
}
#endif