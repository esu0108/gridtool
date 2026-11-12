#pragma once

#include <Windows.h>

#include <filesystem>
#include <optional>

// Both require COM to be initialized on the calling thread (CoInitializeEx).
std::optional<std::filesystem::path> PickFolder(HWND owner);
std::optional<std::filesystem::path> PickSaveBmp(HWND owner);
