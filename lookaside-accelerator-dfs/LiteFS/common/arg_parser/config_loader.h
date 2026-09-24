#pragma once

#include <string>
#include <unordered_map>
#include <vector>

namespace litefs {

struct ConfigData {
  std::unordered_map<std::string, std::vector<std::string>> program_flags;
  std::vector<std::string> dpdk_tokens;
};

ConfigData load_config_file(const std::string& path);

}  // namespace litefs

