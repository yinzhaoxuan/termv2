#pragma once
#include <string>

namespace uicfg::detail {

inline std::string foldEnvironmentName(std::string name) {
  for (char& ch : name) if (ch >= 'A' && ch <= 'Z') ch += 'a' - 'A';
  return name;
}

}  // namespace uicfg::detail
