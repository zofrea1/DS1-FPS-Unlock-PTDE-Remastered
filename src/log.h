#pragma once

void log_init(const wchar_t* dll_path);
void log_write(const char* level, const char* fmt, ...);
void log_close();

#define LOG_INFO(...) log_write("info", __VA_ARGS__)
#define LOG_ERROR(...) log_write("error", __VA_ARGS__)
