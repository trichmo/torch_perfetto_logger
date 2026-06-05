# Perfetto Logger for Kineto

A standalone `ActivityLogger` implementation that outputs Perfetto protobuf (`.pftrace`) trace files from PyTorch/Kineto profiling sessions. Traces can be viewed in the [Perfetto UI](https://ui.perfetto.dev).

## Dependencies

| Dependency | Purpose | Location |
|---|---|---|
| **Perfetto SDK** (v56.0) | Amalgamated C++ SDK for protobuf trace writing (`perfetto.h`, `perfetto.cc`) | `/root/workspace/code/perfetto_sdk/` |
| **Kineto** | Public headers only (`libkineto/include/`) — defines the `ActivityLogger` interface | `/root/workspace/code/kineto/` |
| **PyTorch** | For building the Python extension that registers the logger at runtime | `/root/workspace/code/pytorch/` |
| **fmt** (>=10.x) | Required by Kineto's `GenericTraceActivity.h` — fetched automatically by CMake | auto |

### PyTorch Patch Requirements

Two changes are required in the PyTorch source tree:

1. **`build_variables.bzl`** — Add `custom_logger_registry.cpp` to `libtorch_profiler_sources`:
   ```
   "torch/csrc/profiler/standalone/custom_logger_registry.cpp",
   ```

2. **`torch/csrc/autograd/profiler_kineto.cpp`** — Wire up `CustomLoggerRegistry::onKinetoInit()` so registered loggers are forwarded to Kineto when profiling starts:
   ```cpp
   #include <torch/csrc/profiler/standalone/custom_logger_registry.h>
   // ... in the profiler init function, before the KINETO_PRIVATEUSE1 block:
   torch::profiler::impl::CustomLoggerRegistry::instance().onKinetoInit();
   ```

Rebuild PyTorch after applying these changes.

## Build Instructions

### Standalone (CMake) — for development and unit testing

```bash
cd vllm_activity_logger
cmake -B build
cmake --build build -j8
./build/test_perfetto_logger   # writes /tmp/test_perfetto_trace.pftrace
```

### Python Extension — for use with PyTorch profiler

```bash
source /root/workspace/code/pytorch/.venv/bin/activate
cd vllm_activity_logger
python setup.py build_ext --inplace
```

This produces `perfetto_logger.cpython-*.so` in the project directory.

Environment variables (optional):
- `KINETO_ROOT` — path to Kineto source (default: `/root/workspace/code/kineto`)
- `PERFETTO_SDK` — path to Perfetto amalgamated SDK (default: `/root/workspace/code/perfetto_sdk`)

### Making the module importable

After building, you need the `.so` file on Python's import path. Several options:

**Option A: pip install** (recommended)
```bash
pip install -e .
```
Note: `uv pip install` may fail because `setup.py` imports `torch` at the top level and `uv` uses an isolated build environment where `torch` is not available. Use `pip install` directly instead.

**Option B: Add to PYTHONPATH**
```bash
export PYTHONPATH=/path/to/vllm_activity_logger:$PYTHONPATH
```

**Option C: Copy the .so to your working directory**
```bash
cp perfetto_logger.cpython-*.so /path/to/your/project/
```

## Usage with PyTorch

```python
import torch

# Import registers the "perfetto" protocol automatically
import perfetto_logger

# Profile normally
with torch.profiler.profile(
    activities=[torch.profiler.ProfilerActivity.CPU],
) as prof:
    x = torch.randn(1000, 1000)
    y = x @ x
    with torch.profiler.record_function("my_operation"):
        z = y.sum()

# Save as Perfetto trace (instead of Chrome JSON)
prof.profiler.kineto_results.save("perfetto:///tmp/trace.pftrace")
```

Open the resulting file at [ui.perfetto.dev](https://ui.perfetto.dev).

### How it works

1. `import perfetto_logger` loads the C++ extension and calls `CustomLoggerRegistry::instance().registerLogger("perfetto", factory)`
2. When `torch.profiler.profile()` starts, PyTorch calls `CustomLoggerRegistry::onKinetoInit()` which forwards the factory to Kineto's internal `ActivityLoggerFactory`
3. `kineto_results.save("perfetto://path")` triggers Kineto to parse the `"perfetto"` protocol prefix, look up our factory, create a `PerfettoLogger` instance, and replay all buffered trace events through it
4. `PerfettoLogger` writes each event as a framed Perfetto `TracePacket` to the output file

## Code Overview

### Core Files

```
src/
  PerfettoLogger.h       # Class declaration (ActivityLogger subclass)
  PerfettoLogger.cpp     # Full implementation
  InterningState.h       # Interning table helper (header-only)
register_perfetto_logger.cpp   # pybind11 module for PyTorch registration
setup.py                       # Python extension build config
CMakeLists.txt                 # Standalone CMake build
test/
  test_perfetto_logger.cpp     # Unit test exercising all handlers
```

### PerfettoLogger Class

Implements all 9 pure-virtual methods of `libkineto::ActivityLogger`:

| Method | What it does |
|---|---|
| `handleTraceStart` | Emits initial state-clear packet with clock defaults |
| `handleDeviceInfo` | Emits `TrackDescriptor` with `ProcessDescriptor` for each device (CPU process, GPU) |
| `handleResourceInfo` | Emits `TrackDescriptor` with `ThreadDescriptor` for each resource (thread, CUDA stream) |
| `handleOverheadInfo` | Emits overhead process track (pid=-1) |
| `handleActivity` | Main dispatch — routes to slice, instant, or counter emission |
| `handleGenericActivity` | Delegates to `handleActivity` |
| `handleTraceSpan` | Emits iteration span slices on a "Spans" process track |
| `finalizeTrace` | Emits end marker, flushes file, atomic rename |
| `finalizeMemoryTrace` | No-op |

### Key Design Themes

**Protozero wire format** — Each event is serialized via `protozero::HeapBuffered<TracePacket>`, then written with manual framing (`0x0A` tag + varint length + payload). A single `HeapBuffered` instance is reused via `Reset()` to minimize allocations.

**Interning** — Event names, category strings, and debug annotation keys are interned via `InternedData` to reduce trace size. Each interning namespace (event_names, event_categories, debug_annotation_names) maintains its own iid counter starting at 1. New interned entries are emitted in the same packet that first references them.

**Track hierarchy** — Kineto's two-level device/resource model maps to Perfetto's `TrackDescriptor` parent/child hierarchy. Track UUIDs are generated deterministically via FNV-1a hashing of (deviceId) or (deviceId, resourceId). Track descriptors are emitted lazily and deduplicated via `emittedTracks_` set.

**Activity mapping** — Most Kineto activity types emit as `TrackEvent` SLICE_BEGIN/END pairs, differentiated by category string (e.g. `"cpu_op"`, `"cuda_runtime"`, `"kernel"`). Special cases:
- `CPU_INSTANT_EVENT` → `TYPE_INSTANT`
- `MTIA_COUNTERS`, `XPU_SCOPE_PROFILER` → `TYPE_COUNTER` on dedicated counter tracks
- `CUDA_SYNC "Stream Sync"` → placed on a virtual thread track offset by 1,000,000 (matching ChromeTraceLogger behavior)

**Flow events** — Kineto flow IDs are namespaced by type (`(flowType << 32) | flowId`) and emitted as `flow_ids` (for flow sources) or `terminating_flow_ids` (for flow destinations).

**Metadata** — `metadataJson()` output is parsed by a lightweight JSON key-value parser and emitted as typed `DebugAnnotation` entries (int, double, string, bool). NCCL collective metadata from linked `record_param_comms` activities is extracted and emitted as named annotations.

**ChromeTraceLogger parity** — The implementation replicates ChromeTraceLogger's external ID exclusion logic, NCCL metadata extraction, tid sanitization, and duration clamping. It intentionally does *not* replicate the GPU_USER_ANNOTATION +/-1ns timestamp hack (Perfetto handles overlapping events natively) or relative time conversion (Perfetto uses absolute nanoseconds).

### Current Scope

This is milestone 1, covering all `TrackEvent`-based activity types. GPU kernel/memcpy/memset activities (`CONCURRENT_KERNEL`, `GPU_MEMCPY`, `GPU_MEMSET`) are emitted as TrackEvent slices. Milestone 2 will emit these as `GpuRenderStageEvent` protos with CUDA-specific fields (grid/block size, shared memory, compute kernel interning).
