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

  // =====================================================================
  // Test 2: Collapse rules
  // =====================================================================
  static const std::string kCollapseOutput = "/tmp/test_collapse_trace.pftrace";
  std::cout << "\n--- Collapse test ---" << std::endl;

  auto clogger = std::make_unique<perfetto_logger::PerfettoLogger>(kCollapseOutput);
  clogger->addCollapseRule(
      {perfetto_logger::CollapseRule::STRICT_PATTERN,
       {"a_fn", "b_fn", "a_fn1"}, {}, "my_collapsed"});

  std::unordered_map<std::string, std::string> cmeta;
  clogger->handleTraceStart(cmeta, "[]");

  libkineto::DeviceInfo cdev{12345, 0, "process", "CPU"};
  clogger->handleDeviceInfo(cdev, 0);
  libkineto::ResourceInfo cres{42, 0, 12345, "thread 42"};
  clogger->handleResourceInfo(cres, 0);

  libkineto::TraceSpan cspan(0, 0, "test");

  // Nested events matching the collapse pattern: a_fn > b_fn > a_fn1
  // a_fn: t=100..600 (outermost)
  {
    libkineto::GenericTraceActivity act(cspan, libkineto::ActivityType::PYTHON_FUNCTION, "a_fn");
    act.startTime = 100; act.endTime = 600;
    act.device = 12345; act.resource = 42;
    act.addMetadata("Python id", 10);
    act.addMetadataQuoted("Python parent id", "null");
    act.log(*clogger);
  }
  // b_fn: t=100..500 (child of a_fn)
  {
    libkineto::GenericTraceActivity act(cspan, libkineto::ActivityType::PYTHON_FUNCTION, "b_fn");
    act.startTime = 100; act.endTime = 500;
    act.device = 12345; act.resource = 42;
    act.addMetadata("Python id", 11);
    act.addMetadata("Python parent id", 10);
    act.log(*clogger);
  }
  // a_fn1: t=110..400 (child of b_fn — completes the pattern)
  {
    libkineto::GenericTraceActivity act(cspan, libkineto::ActivityType::PYTHON_FUNCTION, "a_fn1");
    act.startTime = 110; act.endTime = 400;
    act.device = 12345; act.resource = 42;
    act.addMetadata("Python id", 12);
    act.addMetadata("Python parent id", 11);
    act.log(*clogger);
  }
  // Sibling event under b_fn (not part of pattern, should be emitted)
  {
    libkineto::GenericTraceActivity act(cspan, libkineto::ActivityType::PYTHON_FUNCTION, "b_fn_sub");
    act.startTime = 120; act.endTime = 350;
    act.device = 12345; act.resource = 42;
    act.addMetadata("Python id", 13);
    act.addMetadata("Python parent id", 12);  // parent is suppressed a_fn1
    act.log(*clogger);
  }

  // Unrelated event after the collapse (should emit normally)
  {
    libkineto::GenericTraceActivity act(cspan, libkineto::ActivityType::CPU_OP, "unrelated_op");
    act.startTime = 700; act.endTime = 800;
    act.device = 12345; act.resource = 42;
    act.log(*clogger);
  }

  libkineto::Config cconfig;
  std::unordered_map<std::string, std::vector<std::string>> cfinalMeta;
  clogger->finalizeTrace(cconfig, nullptr, 1000, cfinalMeta);

  std::ifstream ccheck(kCollapseOutput, std::ios::binary | std::ios::ate);
  if (!ccheck.is_open()) {
    std::cerr << "FAIL: Collapse output not created" << std::endl;
    return 1;
  }
  auto csize = ccheck.tellg();
  std::cout << "OK: Collapse trace wrote " << csize << " bytes to " << kCollapseOutput << std::endl;
  std::cout << "Expected: 'my_collapsed' event (t=100..600), 'b_fn_sub' (t=120..350), 'unrelated_op' (t=700..800)" << std::endl;
  std::cout << "Open in https://ui.perfetto.dev to inspect." << std::endl;

  // =====================================================================
  // Test 3: PREFIX_SET collapse — non-matching descendants stay, matching
  //         descendants at any depth are suppressed
  // =====================================================================
  static const std::string kPrefixOutput = "/tmp/test_prefix_collapse_trace.pftrace";
  std::cout << "\n--- PREFIX_SET collapse test ---" << std::endl;

  auto plogger = std::make_unique<perfetto_logger::PerfettoLogger>(kPrefixOutput);
  plogger->addCollapseRule(
      {perfetto_logger::CollapseRule::PREFIX_SET,
       {}, {"torch_", "autograd_"}, "matched_"});

  std::unordered_map<std::string, std::string> pmeta;
  plogger->handleTraceStart(pmeta, "[]");

  libkineto::DeviceInfo pdev{12345, 0, "process", "CPU"};
  plogger->handleDeviceInfo(pdev, 0);
  libkineto::ResourceInfo pres{42, 0, 12345, "thread 42"};
  plogger->handleResourceInfo(pres, 0);

  libkineto::TraceSpan pspan(0, 0, "test");

  // Tree structure:
  //   torch_          100──────────────────────────1000
  //    autograd_      100────────────────────900
  //      torch_       100──200
  //      other_            210──300
  //      another_               310──400
  //    ignore_ (leaf)                410─450
  //    ignore_                           460──────900
  //      autograd_                       460─────890
  //        torch_                        460────880
  //          autograd_                   460───870
  //            torch_                    460─550
  //            torch_                         560─650

  // torch_ (root)
  {
    libkineto::GenericTraceActivity act(pspan, libkineto::ActivityType::PYTHON_FUNCTION, "torch_root");
    act.startTime = 100; act.endTime = 1000;
    act.device = 12345; act.resource = 42;
    act.addMetadata("Python id", 1);
    act.addMetadataQuoted("Python parent id", "null");
    act.log(*plogger);
  }
  // autograd_ (child of torch_)
  {
    libkineto::GenericTraceActivity act(pspan, libkineto::ActivityType::PYTHON_FUNCTION, "autograd_dispatch");
    act.startTime = 100; act.endTime = 900;
    act.device = 12345; act.resource = 42;
    act.addMetadata("Python id", 2);
    act.addMetadata("Python parent id", 1);
    act.log(*plogger);
  }
  // torch_ (inner, child of autograd_)
  {
    libkineto::GenericTraceActivity act(pspan, libkineto::ActivityType::PYTHON_FUNCTION, "torch_inner");
    act.startTime = 100; act.endTime = 200;
    act.device = 12345; act.resource = 42;
    act.addMetadata("Python id", 3);
    act.addMetadata("Python parent id", 2);
    act.log(*plogger);
  }
  // other_ (sibling of torch_ inner, non-matching — should be EMITTED)
  {
    libkineto::GenericTraceActivity act(pspan, libkineto::ActivityType::PYTHON_FUNCTION, "other_work");
    act.startTime = 210; act.endTime = 300;
    act.device = 12345; act.resource = 42;
    act.addMetadata("Python id", 4);
    act.addMetadata("Python parent id", 2);
    act.log(*plogger);
  }
  // another_ (sibling, non-matching — should be EMITTED)
  {
    libkineto::GenericTraceActivity act(pspan, libkineto::ActivityType::PYTHON_FUNCTION, "another_task");
    act.startTime = 310; act.endTime = 400;
    act.device = 12345; act.resource = 42;
    act.addMetadata("Python id", 5);
    act.addMetadata("Python parent id", 2);
    act.log(*plogger);
  }
  // ignore_ (leaf, child of torch_ root, non-matching — should be EMITTED)
  {
    libkineto::GenericTraceActivity act(pspan, libkineto::ActivityType::PYTHON_FUNCTION, "ignore_leaf");
    act.startTime = 410; act.endTime = 450;
    act.device = 12345; act.resource = 42;
    act.addMetadata("Python id", 6);
    act.addMetadata("Python parent id", 1);
    act.log(*plogger);
  }
  // ignore_ (parent, child of torch_ root, non-matching — should be EMITTED)
  {
    libkineto::GenericTraceActivity act(pspan, libkineto::ActivityType::PYTHON_FUNCTION, "ignore_parent");
    act.startTime = 460; act.endTime = 900;
    act.device = 12345; act.resource = 42;
    act.addMetadata("Python id", 7);
    act.addMetadata("Python parent id", 1);
    act.log(*plogger);
  }
  // autograd_ (under ignore_, matching — should be SUPPRESSED)
  {
    libkineto::GenericTraceActivity act(pspan, libkineto::ActivityType::PYTHON_FUNCTION, "autograd_deep");
    act.startTime = 460; act.endTime = 890;
    act.device = 12345; act.resource = 42;
    act.addMetadata("Python id", 8);
    act.addMetadata("Python parent id", 7);
    act.log(*plogger);
  }
  // torch_ (under autograd_ deep)
  {
    libkineto::GenericTraceActivity act(pspan, libkineto::ActivityType::PYTHON_FUNCTION, "torch_deep1");
    act.startTime = 460; act.endTime = 880;
    act.device = 12345; act.resource = 42;
    act.addMetadata("Python id", 9);
    act.addMetadata("Python parent id", 8);
    act.log(*plogger);
  }
  // autograd_ (under torch_ deep1)
  {
    libkineto::GenericTraceActivity act(pspan, libkineto::ActivityType::PYTHON_FUNCTION, "autograd_deep2");
    act.startTime = 460; act.endTime = 870;
    act.device = 12345; act.resource = 42;
    act.addMetadata("Python id", 10);
    act.addMetadata("Python parent id", 9);
    act.log(*plogger);
  }
  // torch_ (first child of autograd_deep2)
  {
    libkineto::GenericTraceActivity act(pspan, libkineto::ActivityType::PYTHON_FUNCTION, "torch_leaf1");
    act.startTime = 460; act.endTime = 550;
    act.device = 12345; act.resource = 42;
    act.addMetadata("Python id", 11);
    act.addMetadata("Python parent id", 10);
    act.log(*plogger);
  }
  // torch_ (second child of autograd_deep2, sibling of above)
  {
    libkineto::GenericTraceActivity act(pspan, libkineto::ActivityType::PYTHON_FUNCTION, "torch_leaf2");
    act.startTime = 560; act.endTime = 650;
    act.device = 12345; act.resource = 42;
    act.addMetadata("Python id", 12);
    act.addMetadata("Python parent id", 10);
    act.log(*plogger);
  }

  libkineto::Config pconfig;
  std::unordered_map<std::string, std::vector<std::string>> pfinalMeta;
  plogger->finalizeTrace(pconfig, nullptr, 1100, pfinalMeta);

  std::ifstream pcheck(kPrefixOutput, std::ios::binary | std::ios::ate);
  if (!pcheck.is_open()) {
    std::cerr << "FAIL: PREFIX_SET output not created" << std::endl;
    return 1;
  }
  auto psize = pcheck.tellg();
  std::cout << "OK: PREFIX_SET trace wrote " << psize << " bytes to " << kPrefixOutput << std::endl;
  std::cout << "Expected: 'matched_' (100..1000) with children: other_work, another_task, ignore_leaf, ignore_parent" << std::endl;
  std::cout << "All torch_*/autograd_* events should be suppressed." << std::endl;
  std::cout << "Open in https://ui.perfetto.dev to inspect." << std::endl;

  return 0;
}
