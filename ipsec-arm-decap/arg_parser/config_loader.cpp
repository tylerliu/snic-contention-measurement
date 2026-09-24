#include "config_loader.h"

#include <fstream>
#include <limits>
#include <sstream>

#include <yaml.h>

#include "arg_parser.h"

namespace litefs {
namespace {

[[noreturn]] void throw_yaml_error(const std::string& path,
                                   const std::string& message) {
  std::ostringstream oss;
  oss << "Failed to parse config '" << path << "': " << message;
  throw ArgParserError(oss.str());
}

std::string scalar_value(const yaml_node_t* node) {
  const auto* value = reinterpret_cast<const char*>(node->data.scalar.value);
  return std::string(value, node->data.scalar.length);
}

std::vector<std::string> sequence_to_strings(yaml_document_t* document,
                                             const yaml_node_t* sequence,
                                             const std::string& path) {
  std::vector<std::string> result;
  for (auto item = sequence->data.sequence.items.start;
       item < sequence->data.sequence.items.top; ++item) {
    const yaml_node_t* node = yaml_document_get_node(document, *item);
    if (node->type != YAML_SCALAR_NODE) {
      throw_yaml_error(path, "expected scalar inside sequence");
    }
    result.emplace_back(scalar_value(node));
  }
  return result;
}

std::vector<std::string> dpdk_from_mapping(yaml_document_t* document,
                                           const yaml_node_t* mapping,
                                           const std::string& path) {
  std::vector<std::string> tokens;
  for (auto pair = mapping->data.mapping.pairs.start;
       pair < mapping->data.mapping.pairs.top; ++pair) {
    const yaml_node_t* key_node =
        yaml_document_get_node(document, pair->key);
    const yaml_node_t* value_node =
        yaml_document_get_node(document, pair->value);
    if (key_node->type != YAML_SCALAR_NODE) {
      throw_yaml_error(path, "dpdk_args keys must be scalars");
    }
    std::string key = scalar_value(key_node);

    std::vector<std::string> values;
    switch (value_node->type) {
      case YAML_NO_NODE:
        break;
      case YAML_SCALAR_NODE:
        values.emplace_back(scalar_value(value_node));
        break;
      case YAML_SEQUENCE_NODE:
        values = sequence_to_strings(document, value_node, path);
        break;
      default:
        throw_yaml_error(path, "dpdk_args values must be scalars or sequences");
    }

    if (key.empty()) {
      throw_yaml_error(path, "dpdk_args keys cannot be empty");
    }

    if (key == "device" || key == "devices") {
      for (const std::string& dev : values) {
        tokens.push_back("-a");
        tokens.push_back(dev);
      }
      continue;
    }

    if (key.size() == 1) {
      tokens.push_back("-" + key);
      tokens.insert(tokens.end(), values.begin(), values.end());
    } else {
      if (values.empty()) {
        tokens.push_back("--" + key);
      } else if (values.size() == 1) {
        // Check if the value is a boolean true/false
        const std::string& val = values.front();
        if (val == "true" || val == "True" || val == "TRUE") {
          // Boolean true: just add the flag without value
          tokens.push_back("--" + key);
        } else if (val == "false" || val == "False" || val == "FALSE") {
          // Boolean false: don't add the flag at all
          // (for DPDK, false typically means "don't use this option")
        } else {
          // Non-boolean value: add with =value
          tokens.push_back("--" + key + "=" + val);
        }
      } else {
        tokens.push_back("--" + key);
        tokens.insert(tokens.end(), values.begin(), values.end());
      }
    }
  }
  return tokens;
}

std::unordered_map<std::string, std::vector<std::string>> program_flags_from(
    yaml_document_t* document, const yaml_node_t* mapping,
    const std::string& path) {
  std::unordered_map<std::string, std::vector<std::string>> result;
  for (auto pair = mapping->data.mapping.pairs.start;
       pair < mapping->data.mapping.pairs.top; ++pair) {
    const yaml_node_t* key_node =
        yaml_document_get_node(document, pair->key);
    const yaml_node_t* value_node =
        yaml_document_get_node(document, pair->value);
    if (key_node->type != YAML_SCALAR_NODE) {
      throw_yaml_error(path, "program_flags keys must be scalars");
    }
    std::string key = scalar_value(key_node);
    if (key.empty()) {
      throw_yaml_error(path, "program_flags keys cannot be empty");
    }

    std::vector<std::string> values;
    switch (value_node->type) {
      case YAML_SCALAR_NODE:
        values.emplace_back(scalar_value(value_node));
        break;
      case YAML_SEQUENCE_NODE:
        values = sequence_to_strings(document, value_node, path);
        break;
      default:
        throw_yaml_error(path,
                         "program_flags values must be scalars or sequences");
    }

    result.emplace(std::move(key), std::move(values));
  }
  return result;
}

}  // namespace

ConfigData load_config_file(const std::string& path) {
  std::ifstream file(path);
  if (!file.is_open()) {
    throw ArgParserError("Unable to open config file: " + path);
  }
  std::stringstream buffer;
  buffer << file.rdbuf();
  const std::string content = buffer.str();

  yaml_parser_t parser;
  if (!yaml_parser_initialize(&parser)) {
    throw ArgParserError("Failed to initialize YAML parser");
  }

  yaml_parser_set_input_string(
      &parser, reinterpret_cast<const unsigned char*>(content.data()),
      content.size());

  yaml_document_t document;
  ConfigData config;
  bool document_loaded = false;

  try {
    if (!yaml_parser_load(&parser, &document)) {
      throw_yaml_error(path, "invalid YAML content");
    }
    document_loaded = true;

    const yaml_node_t* root = yaml_document_get_root_node(&document);
    if (root) {
      if (root->type != YAML_MAPPING_NODE) {
        throw_yaml_error(path, "root must be a mapping");
      }

      for (auto pair = root->data.mapping.pairs.start;
           pair < root->data.mapping.pairs.top; ++pair) {
        const yaml_node_t* key_node =
            yaml_document_get_node(&document, pair->key);
        const yaml_node_t* value_node =
            yaml_document_get_node(&document, pair->value);

        if (key_node->type != YAML_SCALAR_NODE) {
          throw_yaml_error(path, "top-level keys must be scalars");
        }
        const std::string key = scalar_value(key_node);

        if (key == "program_flags") {
          if (value_node->type != YAML_MAPPING_NODE) {
            throw_yaml_error(path, "program_flags must be a mapping");
          }
          config.program_flags =
              program_flags_from(&document, value_node, path);
        } else if (key == "dpdk_args") {
          if (value_node->type == YAML_SEQUENCE_NODE) {
            config.dpdk_tokens =
                sequence_to_strings(&document, value_node, path);
          } else if (value_node->type == YAML_MAPPING_NODE) {
            config.dpdk_tokens =
                dpdk_from_mapping(&document, value_node, path);
          } else {
            throw_yaml_error(path, "dpdk_args must be a mapping or sequence");
          }
        }
      }
    }
  } catch (...) {
    if (document_loaded) {
      yaml_document_delete(&document);
    }
    yaml_parser_delete(&parser);
    throw;
  }

  if (document_loaded) {
    yaml_document_delete(&document);
  }
  yaml_parser_delete(&parser);

  return config;
}

}  // namespace litefs

