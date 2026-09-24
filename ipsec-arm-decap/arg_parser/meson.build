arg_parser_sources = [
  'arg_parser.cpp',
  'config_loader.cpp',
]

arg_parser_deps = [
  dependency('yaml-0.1', required : true),
]

arg_parser_inc = include_directories('..', '.')

arg_parser_lib = static_library(
  'litefs_arg_parser',
  arg_parser_sources,
  dependencies : arg_parser_deps,
  include_directories : arg_parser_inc,
)

arg_parser_dep = declare_dependency(
  link_with : arg_parser_lib,
  include_directories : arg_parser_inc,
)

