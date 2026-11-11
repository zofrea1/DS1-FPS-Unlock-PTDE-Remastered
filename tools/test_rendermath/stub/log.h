#pragma once
#include <cstdio>
#define LOG_INFO(...) (std::printf(__VA_ARGS__), std::printf("\n"))
#define LOG_ERROR(...) (std::printf(__VA_ARGS__), std::printf("\n"))
