#pragma once

#include <cstddef>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace litefs {

class ArgParserError : public std::runtime_error {
public:
  explicit ArgParserError(const std::string& message);
};

struct OptionState {
  int occurrences{0};
  std::vector<std::string> values;
};

class ArgParser;

class ArgsContext {
public:
  ArgsContext() = default;

  const std::string* first_value(const std::string& key) const;
  std::vector<std::string> values(const std::string& key) const;
  int occurrences(const std::string& key) const;
  bool has(const std::string& key) const;
  bool bool_value(const std::string& key, bool default_value) const;
  long long int_value(const std::string& key, long long default_value) const;

  const std::vector<std::string>& dpdk_args() const;
  const std::vector<std::string>& trailing_args() const;
  const std::vector<std::string>& loaded_configs() const;

private:
  friend class ArgParser;

  std::unordered_map<std::string, OptionState> options_;
  std::vector<std::string> dpdk_args_;
  std::vector<std::string> trailing_args_;
  std::vector<std::string> loaded_configs_;
};

struct OptionSpec {
  std::string name;
  std::string alias;
  bool required{false};
  std::size_t min_arguments{1};
  std::size_t max_arguments{1};
  bool has_default{false};
  std::string default_value;
  std::string help;
};

class OptionBuilder {
public:
  explicit OptionBuilder(OptionSpec& spec);

  OptionBuilder& alias(std::string alias);
  OptionBuilder& required(bool value = true);
  OptionBuilder& default_value(std::string value);
  OptionBuilder& argument_count(char mode);
  OptionBuilder& help(std::string text);

private:
  OptionSpec& spec_;
};

class ArgParser {
public:
  ArgParser(std::string program_name, std::string description = {});

  OptionBuilder add_option(std::string name);

  ArgsContext parse(int argc, char** argv) const;
  std::string usage() const;

private:
  ArgsContext parse_tokens(const std::vector<std::string>& dpdk_tokens,
                           const std::vector<std::string>& app_tokens) const;
  bool token_is_known_option(const std::string& token) const;
  void prepare_indexes() const;

  const OptionSpec* find_by_name(const std::string& name) const;
  const OptionSpec* find_by_alias(const std::string& alias) const;

  std::string program_name_;
  std::string description_;
  std::vector<OptionSpec> options_;
  mutable std::unordered_map<std::string, std::size_t> name_index_;
  mutable std::unordered_map<std::string, std::size_t> alias_index_;
};

// Helper conversions
int to_int(const std::string& value);

}  // namespace litefs

