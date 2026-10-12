// UTF-8 <-> UTF-16 for Win32 calls. Everything inside the app is UTF-8; only the API boundary is wide.
#pragma once

#include <string>

namespace fph {
namespace pc {

std::wstring widen(const std::string& utf8);
std::string narrow(const std::wstring& utf16);

}  // namespace pc
}  // namespace fph
