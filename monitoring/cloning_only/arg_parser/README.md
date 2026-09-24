# Application Argument Parser

This module provides a lightweight argument parser used by the monitoring application:

- **DPDK separation** – tokens that appear before `--` are forwarded directly to
  `rte_eal_init`, while tokens after `--` are parsed as program options.
- **YAML configuration** – a single `--config <path>` flag loads program
  defaults from a YAML file (`program_flags` for application options and
  `dpdk_args` for EAL flags).
- **String-first API** – every option is stored as raw strings. Callers can
  convert values using helpers like `first_value()` and `values()`, keeping the
  parsing core simple and reusable.

To use, add the dependency in Meson:

```meson
deps += arg_parser_dep
sources += ['main.cpp']
```

Then include the header:

```cpp
#include "arg_parser/arg_parser.h"
```

See the [monitoring guide](../../README.md) for configuration and execution examples.

