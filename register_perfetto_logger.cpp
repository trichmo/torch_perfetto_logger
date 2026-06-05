#include "PerfettoLogger.h"
#include <torch/extension.h>

#ifdef USE_KINETO
#include <torch/csrc/profiler/standalone/custom_logger_registry.h>
#endif

static bool g_registered = false;

void register_perfetto_logger() {
  if (g_registered) {
    return;
  }

#ifdef USE_KINETO
  torch::profiler::impl::CustomLoggerRegistry::instance().registerLogger(
      "perfetto",
      [](const std::string& url)
          -> std::unique_ptr<libkineto::ActivityLogger> {
        return std::make_unique<perfetto_logger::PerfettoLogger>(url);
      });
#else
  libkineto::api().registerLoggerFactory(
      "perfetto",
      [](const std::string& url)
          -> std::unique_ptr<libkineto::ActivityLogger> {
        return std::make_unique<perfetto_logger::PerfettoLogger>(url);
      });
#endif

  g_registered = true;
}

PYBIND11_MODULE(TORCH_EXTENSION_NAME, m) {
  m.doc() = "Perfetto trace logger for PyTorch/Kineto profiler";
  register_perfetto_logger();
  m.def("register", &register_perfetto_logger,
        "Register Perfetto logger with PyTorch/Kineto (called automatically on import)");
}
