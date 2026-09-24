#include "arg_parser.h"

#include <algorithm>
#include <cctype>
#include <iterator>
#include <limits>
#include <sstream>
#include <unordered_set>

#include "config_loader.h"

namespace litefs {
namespace {

bool starts_with(const std::string& value, const std::string& prefix) {
  if (value.size() < prefix.size()) {
    return false;
  }
  return std::equal(prefix.begin(), prefix.end(), value.begin());
}

bool parse_bool_literal(const std::string& value, bool* out) {
  std::string lower(value.size(), '\0');
  std::transform(value.begin(), value.end(), lower.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (lower == "1" || lower == "true" || lower == "yes" || lower == "on") {
    *out = true;
    return true;
  }
  if (lower == "0" || lower == "false" || lower == "no" || lower == "off") {
    *out = false;
    return true;
  }
  return false;
}

OptionState& ensure_state(std::unordered_map<std::string, OptionState>& table,
                          const std::string& key) {
  std::unordered_map<std::string, OptionState>::iterator it = table.find(key);
  if (it == table.end()) {
    it = table.insert(std::make_pair(key, OptionState())).first;
  }
  return it->second;
}

}  // namespace

ArgParserError::ArgParserError(const std::string& message)
    : std::runtime_error(message) {}

const std::string* ArgsContext::first_value(const std::string& key) const {
  std::unordered_map<std::string, OptionState>::const_iterator it =
      options_.find(key);
  if (it == options_.end() || it->second.values.empty()) {
    return nullptr;
  }
  return &it->second.values.front();
}

std::vector<std::string> ArgsContext::values(const std::string& key) const {
  std::unordered_map<std::string, OptionState>::const_iterator it =
      options_.find(key);
  if (it == options_.end()) {
    return {};
  }
  return it->second.values;
}

int ArgsContext::occurrences(const std::string& key) const {
  std::unordered_map<std::string, OptionState>::const_iterator it =
      options_.find(key);
  if (it == options_.end()) {
    return 0;
  }
  return it->second.occurrences;
}

bool ArgsContext::has(const std::string& key) const {
  std::unordered_map<std::string, OptionState>::const_iterator it =
      options_.find(key);
  if (it == options_.end()) {
    return false;
  }
  return it->second.occurrences > 0 || !it->second.values.empty();
}

bool ArgsContext::bool_value(const std::string& key, bool default_value) const {
  std::unordered_map<std::string, OptionState>::const_iterator it =
      options_.find(key);
  if (it == options_.end()) {
    return default_value;
  }
  if (it->second.occurrences > 0 && it->second.values.empty()) {
    return true;
  }
  if (!it->second.values.empty()) {
    bool parsed = false;
    if (!parse_bool_literal(it->second.values.back(), &parsed)) {
      throw ArgParserError("Invalid boolean value for option --" + key + ": " + it->second.values.back());
    }
    return parsed;
  }
  return default_value;
}

long long ArgsContext::int_value(const std::string& key, long long default_value) const {
  std::unordered_map<std::string, OptionState>::const_iterator it =
      options_.find(key);
  if (it == options_.end() || it->second.values.empty()) {
    return default_value;
  }
  try {
    return std::stoll(it->second.values.back());
  } catch (const std::exception&) {
    throw ArgParserError("Invalid integer value for option --" + key + ": " + it->second.values.back());
  }
}

const std::vector<std::string>& ArgsContext::dpdk_args() const {
  return dpdk_args_;
}

const std::vector<std::string>& ArgsContext::trailing_args() const {
  return trailing_args_;
}

const std::vector<std::string>& ArgsContext::loaded_configs() const {
  return loaded_configs_;
}

OptionBuilder::OptionBuilder(OptionSpec& spec) : spec_(spec) {}

OptionBuilder& OptionBuilder::alias(std::string alias) {
  if (alias == "c") {
    throw ArgParserError("Alias '-c' is reserved for --config");
  }
  spec_.alias = std::move(alias);
  return *this;
}

OptionBuilder& OptionBuilder::required(bool value) {
  spec_.required = value;
  return *this;
}

OptionBuilder& OptionBuilder::default_value(std::string value) {
  spec_.has_default = true;
  spec_.default_value = std::move(value);
  return *this;
}

OptionBuilder& OptionBuilder::argument_count(char mode) {
  switch (mode) {
    case '0':
      spec_.min_arguments = 0;
      spec_.max_arguments = 0;
      break;
    case '?':
      spec_.min_arguments = 0;
      spec_.max_arguments = 1;
      break;
    case '*':
      spec_.min_arguments = 0;
      spec_.max_arguments = std::numeric_limits<std::size_t>::max();
      break;
    case '+':
      spec_.min_arguments = 1;
      spec_.max_arguments = std::numeric_limits<std::size_t>::max();
      break;
    case '1':
    case ' ':
    default: {
      if (mode >= '0' && mode <= '9') {
        std::size_t count = static_cast<std::size_t>(mode - '0');
        spec_.min_arguments = count;
        spec_.max_arguments = count;
      } else {
        spec_.min_arguments = 1;
        spec_.max_arguments = 1;
      }
      break;
    }
  }
  return *this;
}

OptionBuilder& OptionBuilder::help(std::string text) {
  spec_.help = std::move(text);
  return *this;
}

ArgParser::ArgParser(std::string program_name, std::string description)
    : program_name_(std::move(program_name)),
      description_(std::move(description)) {}

OptionBuilder ArgParser::add_option(std::string name) {
  if (name.empty()) {
    throw ArgParserError("Option name cannot be empty");
  }
  if (name == "config") {
    throw ArgParserError("Option '--config' is reserved");
  }
  if (name_index_.count(name) != 0) {
    throw ArgParserError("Duplicate option name: " + name);
  }
  OptionSpec spec;
  spec.name = name;
  options_.push_back(std::move(spec));
  const std::size_t index = options_.size() - 1;
  name_index_[options_.back().name] = index;
  return OptionBuilder(options_.back());
}

ArgsContext ArgParser::parse(int argc, char** argv) const {
  std::vector<std::string> tokens;
  tokens.reserve(argc > 0 ? argc - 1 : 0);
  for (int i = 1; i < argc; ++i) {
    tokens.emplace_back(argv[i]);
  }

  std::vector<std::string> dpdk_tokens;
  std::vector<std::string> app_tokens;

  auto separator = std::find(tokens.begin(), tokens.end(), "--");
  if (separator != tokens.end()) {
    dpdk_tokens.assign(tokens.begin(), separator);
    app_tokens.assign(std::next(separator), tokens.end());
  } else {
    app_tokens = tokens;
  }

  return parse_tokens(dpdk_tokens, app_tokens);
}

std::string ArgParser::usage() const {
  std::ostringstream oss;
  oss << "Usage: " << program_name_ << " [options]";
  if (!description_.empty()) {
    oss << "\n\n" << description_;
  }
  oss << "\n\nOptions:\n";
  for (const auto& spec : options_) {
    oss << "  --" << spec.name;
    if (!spec.alias.empty()) {
      oss << ", -" << spec.alias;
    }
    oss << "\n";
    if (!spec.help.empty()) {
      oss << "      " << spec.help << "\n";
    }
  }
  oss << "\nDPDK arguments may be supplied before '--' or via config.";
  return oss.str();
}

ArgsContext ArgParser::parse_tokens(
    const std::vector<std::string>& dpdk_tokens,
    const std::vector<std::string>& app_tokens) const {
  prepare_indexes();

  std::string config_path;
  std::vector<std::string> filtered_tokens;
  filtered_tokens.reserve(app_tokens.size());

  for (std::size_t i = 0; i < app_tokens.size(); ++i) {
    const std::string& token = app_tokens[i];

    if (token == "--config" || token == "-c") {
      if (!config_path.empty()) {
        throw ArgParserError("Duplicate --config flag");
      }
      if (i + 1 >= app_tokens.size()) {
        throw ArgParserError("--config requires a path argument");
      }
      config_path = app_tokens[i + 1];
      ++i;
      continue;
    }
    if (starts_with(token, "--config=") || starts_with(token, "-c=")) {
      if (!config_path.empty()) {
        throw ArgParserError("Duplicate --config flag");
      }
      std::size_t offset = starts_with(token, "--config=") ? std::string("--config=").size()
                                                          : std::string("-c=").size();
      config_path = token.substr(offset);
      continue;
    }
    filtered_tokens.push_back(token);
  }

  ConfigData config;
  if (!config_path.empty()) {
    config = load_config_file(config_path);
  }

  std::unordered_map<std::string, OptionState> states;
  states.reserve(options_.size());

  std::vector<std::string> trailing_args;
  trailing_args.reserve(filtered_tokens.size());

  for (std::size_t i = 0; i < filtered_tokens.size(); ++i) {
    const std::string& token = filtered_tokens[i];

    if (token == "--help" || token == "-h") {
      throw ArgParserError(usage());
    }

    if (starts_with(token, "--")) {
      std::string name_token = token.substr(2);
      std::vector<std::string> inline_values;

      const auto equals = name_token.find('=');
      if (equals != std::string::npos) {
        inline_values.emplace_back(name_token.substr(equals + 1));
        name_token = name_token.substr(0, equals);
      }

      const OptionSpec* spec = find_by_name(name_token);
      if (!spec) {
        throw ArgParserError("Unknown option: --" + name_token);
      }

      OptionState& state = ensure_state(states, spec->name);
      state.occurrences += 1;

      std::vector<std::string> values = std::move(inline_values);
      while (values.size() < spec->max_arguments &&
             i + 1 < filtered_tokens.size() &&
             !token_is_known_option(filtered_tokens[i + 1])) {
        values.emplace_back(filtered_tokens[++i]);
      }

      if (values.size() < spec->min_arguments) {
        throw ArgParserError("Option --" + spec->name +
                             " requires more arguments");
      }
      if (values.size() > spec->max_arguments) {
        throw ArgParserError("Option --" + spec->name +
                             " received too many arguments");
      }

      state.values.insert(state.values.end(), values.begin(), values.end());
      continue;
    }

    if (!token.empty() && token[0] == '-' && token.size() > 1 &&
        token[1] != '-') {
      std::string alias_token = token.substr(1);
      std::vector<std::string> inline_values;

      const auto equals = alias_token.find('=');
      if (equals != std::string::npos) {
        inline_values.emplace_back(alias_token.substr(equals + 1));
        alias_token = alias_token.substr(0, equals);
      }

      const OptionSpec* spec = find_by_alias(alias_token);
      if (!spec) {
        trailing_args.push_back(token);
        continue;
      }

      OptionState& state = ensure_state(states, spec->name);
      state.occurrences += 1;

      std::vector<std::string> values = std::move(inline_values);
      while (values.size() < spec->max_arguments &&
             i + 1 < filtered_tokens.size() &&
             !token_is_known_option(filtered_tokens[i + 1])) {
        values.emplace_back(filtered_tokens[++i]);
      }

      if (values.size() < spec->min_arguments) {
        throw ArgParserError("Option -" + spec->alias +
                             " requires more arguments");
      }
      if (values.size() > spec->max_arguments) {
        throw ArgParserError("Option -" + spec->alias +
                             " received too many arguments");
      }

      state.values.insert(state.values.end(), values.begin(), values.end());
      continue;
    }

    trailing_args.push_back(token);
  }

  ArgsContext context;
  context.dpdk_args_ = dpdk_tokens;
  if (!config_path.empty()) {
    context.loaded_configs_.push_back(config_path);
  }
  context.trailing_args_ = std::move(trailing_args);

  for (const auto& spec : options_) {
    OptionState& state = ensure_state(context.options_, spec.name);

    auto found = states.find(spec.name);
    if (found != states.end()) {
      state = found->second;
    }

    if (state.values.empty()) {
      auto cfg_it = config.program_flags.find(spec.name);
      if (cfg_it != config.program_flags.end()) {
        state.values = cfg_it->second;
      }
    }

    if (state.values.empty() && spec.has_default) {
      state.values.push_back(spec.default_value);
    }

    if (spec.required && state.values.empty()) {
      throw ArgParserError("Missing required option: --" + spec.name);
    }

    if (state.values.size() < spec.min_arguments && spec.required) {
      throw ArgParserError("Required option --" + spec.name +
                           " needs at least one argument");
    }
  }

  if (!config.dpdk_tokens.empty()) {
    context.dpdk_args_.insert(context.dpdk_args_.end(),
                              config.dpdk_tokens.begin(),
                              config.dpdk_tokens.end());
  }

  return context;
}

bool ArgParser::token_is_known_option(const std::string& token) const {
  if (starts_with(token, "--")) {
    std::string key = token.substr(2);
    const auto eq = key.find('=');
    if (eq != std::string::npos) {
      key = key.substr(0, eq);
    }
    return name_index_.count(key) > 0 || key == "config";
  }
  if (token.size() > 1 && token[0] == '-') {
    std::string key = token.substr(1);
    const auto eq = key.find('=');
    if (eq != std::string::npos) {
      key = key.substr(0, eq);
    }
    return alias_index_.count(key) > 0 || key == "h" || key == "c";
  }
  return false;
}

const OptionSpec* ArgParser::find_by_name(const std::string& name) const {
  std::unordered_map<std::string, std::size_t>::const_iterator it =
      name_index_.find(name);
  if (it == name_index_.end()) {
    return nullptr;
  }
  return &options_[it->second];
}

const OptionSpec* ArgParser::find_by_alias(const std::string& alias) const {
  std::unordered_map<std::string, std::size_t>::const_iterator it =
      alias_index_.find(alias);
  if (it == alias_index_.end()) {
    return nullptr;
  }
  return &options_[it->second];
}

void ArgParser::prepare_indexes() const {
  name_index_.clear();
  alias_index_.clear();
  std::unordered_set<std::string> seen_names;
  std::unordered_set<std::string> seen_aliases;
  for (std::size_t i = 0; i < options_.size(); ++i) {
    const auto& spec = options_[i];
    if (!seen_names.insert(spec.name).second) {
      throw ArgParserError("Duplicate option name detected: " + spec.name);
    }
    name_index_.emplace(spec.name, i);
    if (!spec.alias.empty()) {
      if (!seen_aliases.insert(spec.alias).second) {
        throw ArgParserError("Duplicate alias detected: -" + spec.alias);
      }
      alias_index_.emplace(spec.alias, i);
    }
  }
}

int to_int(const std::string& value) {
  return std::stoi(value);
}

}  // namespace litefs

