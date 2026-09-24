## Arg Parsing Utility Plan

### 1. Objectives
- Provide a unified argument parsing facility that replaces DOCA JSON configs while staying compatible with existing LiteFS tooling.
- Maintain clear separation between DPDK/EAL arguments and application arguments without forcing long single-string flags.
- Support YAML (preferred), JSON, and potential future formats via a pluggable loader pipeline.
- Keep the API simple, argparse-like, and extensible for additional binaries and subcommands.

### 2. Target Users & Scope
- All LiteFS binaries currently launching DPDK (`hostfs`, `nicfs`, `nic_tester`, etc.).
- Future utilities that need structured configuration but run without DPDK; the parser should degrade gracefully.
- Initial delivery focuses on C++ consumers; bindings for other languages are out of scope for now but should remain possible.

### 3. High-Level Architecture
1. **Core Module (`common/arg_parser.{hpp,cpp}`)**  
   - Provides `ArgParser`, `ArgSpec`, and `ArgsContext`.
   - Responsible for CLI tokenization, YAML loading, validation, and usage/help text generation.
   - Encodes the convention that option names map to `--<name>` / `-<alias>` on the CLI and to `program_flags.<name>` inside configs unless overridden.
2. **YAML Loader (`common/config_loader.{hpp,cpp}`)**  
   - Thin wrapper over `libyaml` (or `yaml-cpp` if that is already vendored) producing a normalized `ConfigNode`.  
   - Single entry point: `ConfigNode load_yaml(std::string_view path)`.
3. **DPDK Bridge (`common/dpdk_args.{hpp,cpp}`)**  
   - Utilities to capture raw EAL arguments and feed them to `rte_eal_init`.  
   - Holds post-parse metadata (e.g., was `--` used, which config stanza applied).

### 4. DPDK vs Application Argument Strategy
- **Primary Mechanism**: Keep support for the conventional `--` delimiter. When encountered, all tokens before `--` are treated as DPDK/EAL flags (in original order) and everything after as program flags.
- **Context API**: `ArgsContext::dpdk_args()` returns `std::vector<std::string>` ready for `rte_eal_init`.

### 5. Configuration File Support
- **CLI Interface**: Optional single `--config <path>` for the main application configuration; absence means CLI-only operation.
- **Format Handling**: YAML only (`.yaml`/`.yml`). Any other extension results in a clear “unsupported format” error.
- **Schema Mapping**: By default each option binds to `program_flags.<option_name>`; `config_path()` remains available for bespoke layouts. DPDK flags live under `dpdk_args`, expressed as key/value pairs where keys translate into EAL option names and values may be scalars or sequences (for repeated parameters). CLI values override YAML nodes when both are present.
- **Validation**: Types enforced during merge of CLI and config values; detailed error traces include file path and YAML node location.

### 6. ArgParser Interface Sketch
- `ArgSpec` builder pattern, e.g.:
  ```cpp
  ArgParser parser("nicfs", "LiteFS NIC filesystem service");
  parser.add_option("mode")
        .alias("m")
        .required()
        .help("NIC operating mode");
  parser.add_option("log-level")
        .default_value("Info")
        .help("Global doca_log verbosity");
  parser.add_subcommand("daemon", daemon_spec);
  ```
- All option values are stored as strings; conversions happen at retrieval time via helpers like `ArgsContext::as_int` or app-specific adapters.
- Options track two counts: `occurrences` (how many times the flag appeared) and `arguments` (vector of string payloads supplied). `argument_count()` constrains the valid cardinality.
- Option names automatically map to CLI flags (`--mode`, `-m`) and YAML keys (`program_flags.mode`); `config_path()` remains available for advanced overrides.
- `ArgsContext` exposes `const std::string* first_value(const std::string& key)`, `std::vector<std::string> values(const std::string& key)`, and `int occurrences(const std::string& key)` so callers can convert to richer domain types where needed.
- Built-in `--help` and `--version` output with DPDK section explained.
- Logging integration retains existing `doca_log` usage: expose a canonical `log-level` option that falls back to config and applies via a thin bridge (`doca_log_global_level_set`) during parser finalization if the application opts in.

### 6a. Usage Examples
- **Bootstrapping a LiteFS binary**
  ```cpp
  int main(int argc, char** argv) {
    ArgParser parser("nicfs", "LiteFS NIC filesystem service");
    parser.add_option("mode")
          .alias("m")
          .required()
          .help("NIC operating mode");
    parser.add_option("listen")
          .default_value("0.0.0.0:7443");
    parser.add_option("log-level")
          .default_value("Info")
          .help("Controls doca_log global level");
    parser.add_option("replicas")
          .argument_count('+')
          .help("Replicas NICFS will replicate to");

    ArgsContext ctx = parser.parse(argc, argv);

    const std::string* log_level = ctx.first_value("log-level");
    doca_log_global_level_set(to_doca(log_level ? *log_level : "Info"));
    auto dpdk_args = ctx.dpdk_args();
    rte_eal_init(static_cast<int>(dpdk_args.size()),
                 ArgVector(dpdk_args).data());

    NicFsApp app;
    app.configure({
      .mode = *ctx.first_value("mode"),
      .listen_endpoint = parse_address(*ctx.first_value("listen")),
      .replicas = ctx.values("replicas"),
      .config_sources = ctx.loaded_configs(),
    });
    return app.run();
  }
  ```
- **YAML config fragment consumed by the example**
  ```yaml
  # configs/nicfs.yaml
  program_flags:
    mode: passthrough
    listen: 10.10.10.5:7443
    log-level: INFO # DISABLE, CRITICAL, ERROR, WARNING, INFO, DEBUG, TRACE
    replicas:
        - nic_tester
  dpdk_args:
      l: 0-3
      socket-mem: 1024
      a:
        - 03:00.0
        - 03:00.1,dv_flow_en=2
  ```
- **Invocations**
  - CLI only: `nicfs -l 0-1 --socket-mem=512,0 -- --mode loopback --log-level DEBUG --replicas nic_tester`
  - Config-driven: `nicfs --config configs/nicfs.yaml`

### 7. Parsing Workflow
1. **Token Collection**: Capture the raw `argv` into `std::vector<std::string>`.
2. **Early Scan**: Handle `--help`/`--version` before config I/O.
3. **Config Pass**:  
   - Check for a single `--config` token.  
   - Load the YAML file (if provided) into a `ConfigMap`.  
   - Store errors with clear messages referencing file and line.
4. **Argument Pass**:  
   - Iterate tokens until the end.  
   - Apply specs, track unknown options, missing values, or constraint failures.  
5. **DPDK Collection**:  
   - Tokens encountered before `--` form the initial `dpdk_args` vector.  
   - If the YAML config contains a `dpdk_args` section, translate it into tokens and append in declaration order.
6. **Post-Processing**:  
   - Resolve defaults for unspecified options.  
   - Run custom validators.  
   - Emit structured errors with suggestions.

### 8. Error Handling & UX
- Colorized terminal output optional (respect `NO_COLOR`). Otherwise plain text with sections for CLI errors, config errors, and DPDK hints.
- Suggest `--help` on failure. Provide contextual snippet showing conflicting config key/value.
- Distinguish between config read errors (`FileNotFound`, `ParseError`) and validation errors.

### 9. Testing Strategy
- Unit tests under `tests/arg_parser_tests.cpp`:  
  - CLI-only parsing basics.  
  - Config vs CLI precedence (config defaults overridden by CLI).  
  - DPDK delimiter behavior and YAML `dpdk_args` translation.  
  - YAML parsing edge cases (anchors, nested mappings).  
  - Error messaging snapshots.
- Integration test: adapt one binary (e.g., `nicfs`) in a branch to use the new parser and ensure Meson build passes.

### 10. Rollout Steps
1. Implement core parser + config loaders with minimal public API.  
2. Update Meson/CMake to link `libyaml` and ensure dependencies exist in CI.  
3. Create sample configs (`configs/nicfs.yaml`, etc.) and documentation snippet in README.  
4. Port a single executable (`nic_tester` or `hostfs`) using new API; gather feedback.  
5. Iterate on API ergonomics, then migrate remaining binaries.  
6. Deprecate old JSON mechanism once all major apps adopt the new parser.

### 11. Documentation & Developer Guidance
- Add `docs/arg_parser.md` (future) covering quick-start, config format examples, and DPDK argument handling.  
- Include FAQ on migrating from `--` usage and DOCA JSON configs.

### 12. Future Enhancements (Not in Initial Scope)
- Environment variable injection (e.g., `LITEFS_CONFIG`, `LITEFS_DPDK_ARGS_FILE`).  
- Remote config fetch (HTTP, etc.).  
- Dynamic schema validation via JSON Schema or YAML tagging.  
- Autocomplete script generation for `bash`/`zsh`.

