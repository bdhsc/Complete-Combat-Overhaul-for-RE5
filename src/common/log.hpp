#pragma once
#include <string>

namespace logx {
// %LOCALAPPDATA%\RE5Trainer, created on first use.
const std::wstring& DataDir();
void Init();
void Write(const char* fmt, ...);
}

#define LOG(...) ::logx::Write(__VA_ARGS__)
