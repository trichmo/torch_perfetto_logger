#include "PerfettoLogger.h"
#include <torch/extension.h>

#ifdef USE_KINETO
#include <torch/csrc/profiler/standalone/custom_logger_registry.h>
#endif

static std::vector<perfetto_logger::CollapseRule> g_collapse_rules;
static bool g_registered = false;

void register_perfetto_logger() {
  if (g_registered) {
    return;
  }

  auto factory = [](const std::string& url)
      -> std::unique_ptr<libkineto::ActivityLogger> {
    auto logger = std::make_unique<perfetto_logger::PerfettoLogger>(url);
    for (auto& rule : g_collapse_rules) {
      logger->addCollapseRule(rule);
    }
    return logger;
  };

#ifdef USE_KINETO
  torch::profiler::impl::CustomLoggerRegistry::instance().registerLogger(
      "perfetto", factory);
#else
  libkineto::api().registerLoggerFactory("perfetto", factory);
#endif

  g_registered = true;
}

void add_collapse_rule(
    const std::vector<std::string>& pattern,
    const std::string& collapsed_name) {
  g_collapse_rules.push_back(
      {perfetto_logger::CollapseRule::STRICT_PATTERN,
       pattern, {}, collapsed_name});
}

void add_prefix_collapse_rule(
    const std::vector<std::string>& prefixes,
    const std::string& collapsed_name) {
  g_collapse_rules.push_back(
      {perfetto_logger::CollapseRule::PREFIX_SET,
       {}, prefixes, collapsed_name});
}

void clear_collapse_rules() {
  g_collapse_rules.clear();
}

PYBIND11_MODULE(TORCH_EXTENSION_NAME, m) {
  m.doc() = "Perfetto trace logger for PyTorch/Kineto profiler";
  register_perfetto_logger();
  m.def("register", &register_perfetto_logger,
        "Register Perfetto logger with PyTorch/Kineto (called automatically on import)");
  m.def("add_collapse_rule", &add_collapse_rule,
        "Add a strict pattern collapse rule: pattern (list of substrings matching nested event names in order) -> collapsed_name",
        py::arg("pattern"), py::arg("collapsed_name"));
  m.def("add_prefix_collapse_rule", &add_prefix_collapse_rule,
        "Add a prefix-set collapse rule: consecutive nested events whose names contain any prefix are collapsed",
        py::arg("prefixes"), py::arg("collapsed_name"));
  m.def("clear_collapse_rules", &clear_collapse_rules,
        "Remove all collapse rules");
}
