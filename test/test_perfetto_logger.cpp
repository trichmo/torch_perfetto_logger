#include <cstdio>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

// Provide a minimal ActivityBuffers definition so unique_ptr<ActivityBuffers>
// can compile (the type is only forward-declared in output_base.h).
namespace libkineto {
struct ActivityBuffers {};
}

#include "PerfettoLogger.h"

// Provide GenericTraceActivity::log() definition (normally in a Kineto .cpp)
namespace libkineto {
void GenericTraceActivity::log(ActivityLogger& logger) const {
  logger.handleGenericActivity(*this);
}
}

// Stub link-time symbols from Kineto that our headers reference
namespace libkineto {

// ThreadUtil stubs
int32_t systemThreadId(bool) { return 0; }
int32_t threadId() { return 0; }
bool setThreadName(const std::string&) { return false; }
std::string getThreadName() { return ""; }
int32_t pidNamespace(ino_t&) { return 0; }
int32_t processId(bool) { return 0; }
std::string processName(int32_t) { return ""; }
std::vector<std::pair<int32_t, std::string>> pidCommandPairsOfAncestors() { return {}; }

// AbstractConfig stubs
bool AbstractConfig::handleOption(const std::string&, std::string&) { return false; }
void AbstractConfig::printActivityProfilerConfig(std::ostream&) const {}
void AbstractConfig::setActivityDependentConfig() {}

// Config stubs
Config::Config() = default;
bool Config::handleOption(const std::string&, std::string&) { return false; }
void Config::setClientDefaults() {}
void Config::printActivityProfilerConfig(std::ostream&) const {}
void Config::validate(
    const std::chrono::time_point<std::chrono::system_clock>&) {}
void Config::setActivityDependentConfig() {}

// ActivityType non-inline functions
ActivityType toActivityType(const std::string&) {
  return ActivityType::CPU_OP;
}
std::array<ActivityType, activityTypeCount> activityTypes() {
  std::array<ActivityType, activityTypeCount> res{};
  return res;
}
std::array<ActivityType, defaultActivityTypeCount> defaultActivityTypes() {
  std::array<ActivityType, defaultActivityTypeCount> res{};
  return res;
}

}  // namespace libkineto

static const std::string kOutputPath = "/tmp/test_perfetto_trace.pftrace";

int main() {
  std::cout << "Creating PerfettoLogger writing to " << kOutputPath << std::endl;

  auto logger = std::make_unique<perfetto_logger::PerfettoLogger>(kOutputPath);

  // Phase 1: handleTraceStart
  std::unordered_map<std::string, std::string> metadata;
  metadata["schemaVersion"] = "1";
  logger->handleTraceStart(metadata, "[]");

  // Phase 2: handleDeviceInfo
  libkineto::DeviceInfo cpuDevice{
      /*id=*/12345, /*sortIndex=*/0,
      /*name=*/"process 12345", /*label=*/"CPU"};
  logger->handleDeviceInfo(cpuDevice, 1000000000);

  constexpr int64_t kExceedMaxPid = 5000000;
  libkineto::DeviceInfo gpuDevice{
      /*id=*/0, /*sortIndex=*/kExceedMaxPid,
      /*name=*/"process 12345", /*label=*/"GPU 0"};
  logger->handleDeviceInfo(gpuDevice, 1000000000);

  // Phase 3: handleResourceInfo
  libkineto::ResourceInfo cpuThread{
      /*id=*/42, /*sortIndex=*/0,
      /*deviceId=*/12345, /*name=*/"thread 42"};
  logger->handleResourceInfo(cpuThread, 1000000000);

  libkineto::ResourceInfo gpuStream{
      /*id=*/7, /*sortIndex=*/0,
      /*deviceId=*/0, /*name=*/"stream 7"};
  logger->handleResourceInfo(gpuStream, 1000000000);

  // Phase 4: Activities
  libkineto::TraceSpan dummySpan(0, 0, "ProfilerStep");

  // CPU_OP activity
  {
    libkineto::GenericTraceActivity act(
        dummySpan, libkineto::ActivityType::CPU_OP, "aten::linear");
    act.startTime = 1000000000;
    act.endTime = 1000500000;
    act.id = 1;
    act.device = 12345;
    act.resource = 42;
    act.threadId = 42;
    act.addMetadata("Input dims", "[[32, 128]]");
    act.addMetadataQuoted("Input type", "Float");
    act.log(*logger);
  }

  // CUDA_RUNTIME activity
  {
    libkineto::GenericTraceActivity act(
        dummySpan, libkineto::ActivityType::CUDA_RUNTIME, "cudaLaunchKernel");
    act.startTime = 1000100000;
    act.endTime = 1000200000;
    act.id = 100;
    act.device = 12345;
    act.resource = 42;
    act.threadId = 42;
    act.addMetadata("correlation", 100);
    act.addMetadata("cbid", 211);
    act.log(*logger);
  }

  // CONCURRENT_KERNEL (GPU) — emitted as TrackEvent slice for now
  {
    libkineto::GenericTraceActivity act(
        dummySpan, libkineto::ActivityType::CONCURRENT_KERNEL,
        "volta_sgemm_128x64_nn");
    act.startTime = 1000150000;
    act.endTime = 1000400000;
    act.id = 100;
    act.device = 0;
    act.resource = 7;
    act.addMetadata("device", 0);
    act.addMetadata("context", 1);
    act.addMetadata("stream", 7);
    act.addMetadata("correlation", 100);
    act.addMetadata("registers per thread", 32);
    act.addMetadata("shared memory", 8192);
    act.addMetadata("grid", "[128, 1, 1]");
    act.addMetadata("block", "[256, 1, 1]");
    act.log(*logger);
  }

  // CPU_INSTANT_EVENT
  {
    libkineto::GenericTraceActivity act(
        dummySpan, libkineto::ActivityType::CPU_INSTANT_EVENT,
        "RecordFunction callback");
    act.startTime = 1000050000;
    act.endTime = 1000050000;
    act.device = 12345;
    act.resource = 42;
    act.threadId = 42;
    act.log(*logger);
  }

  // USER_ANNOTATION
  {
    libkineto::GenericTraceActivity act(
        dummySpan, libkineto::ActivityType::USER_ANNOTATION,
        "ProfilerStep#5");
    act.startTime = 1000000000;
    act.endTime = 1001000000;
    act.id = 2;
    act.device = 12345;
    act.resource = 42;
    act.threadId = 42;
    act.log(*logger);
  }

  // Activity with flow
  {
    libkineto::GenericTraceActivity cpuAct(
        dummySpan, libkineto::ActivityType::CUDA_RUNTIME, "cudaLaunchKernel");
    cpuAct.startTime = 1000100000;
    cpuAct.endTime = 1000200000;
    cpuAct.id = 200;
    cpuAct.device = 12345;
    cpuAct.resource = 42;
    cpuAct.flow.id = 1;
    cpuAct.flow.type = libkineto::kLinkAsyncCpuGpu;
    cpuAct.flow.start = 1;
    cpuAct.log(*logger);
  }

  // Phase 5: handleTraceSpan
  libkineto::TraceSpan reportSpan(15, 5, "ProfilerStep", "");
  reportSpan.startTime = 1000000000;
  reportSpan.endTime = 1001000000;
  logger->handleTraceSpan(reportSpan);

  // Phase 6: handleOverheadInfo
  libkineto::ActivityLogger::OverheadInfo overhead("CUPTI Overhead");
  logger->handleOverheadInfo(overhead, 1000000000);

  // Phase 7: finalizeTrace
  libkineto::Config config;
  std::unordered_map<std::string, std::vector<std::string>> finalMetadata;
  logger->finalizeTrace(config, nullptr, 1001000000, finalMetadata);

  // Verify output file
  std::ifstream check(kOutputPath, std::ios::binary | std::ios::ate);
  if (!check.is_open()) {
    std::cerr << "FAIL: Output file not created" << std::endl;
    return 1;
  }

  auto size = check.tellg();
  check.seekg(0);
  uint8_t firstByte = 0;
  check.read(reinterpret_cast<char*>(&firstByte), 1);

  if (size <= 0) {
    std::cerr << "FAIL: Output file is empty" << std::endl;
    return 1;
  }

  if (firstByte != 0x0A) {
    std::cerr << "FAIL: First byte is 0x" << std::hex << (int)firstByte
              << ", expected 0x0A" << std::endl;
    return 1;
  }

  std::cout << "OK: Wrote " << size << " bytes to " << kOutputPath << std::endl;
  std::cout << "Open in https://ui.perfetto.dev to inspect the trace." << std::endl;
  return 0;
}
