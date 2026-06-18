#pragma once

#include <cstdint>
#include <fstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "perfetto.h"

#include "InterningState.h"
#include "output_base.h"

namespace perfetto_logger {

using libkineto::DeviceInfo;
using libkineto::ResourceInfo;
using libkineto::TraceSpan;

struct CollapseRule {
  enum Mode { STRICT_PATTERN, PREFIX_SET };
  Mode mode = STRICT_PATTERN;
  std::vector<std::string> pattern;   // for STRICT_PATTERN: ordered event names
  std::vector<std::string> prefixes;  // for PREFIX_SET: any-match substrings
  std::string collapsed_name;
};

class PerfettoLogger : public libkineto::ActivityLogger {
 public:
  explicit PerfettoLogger(const std::string& filename);
  ~PerfettoLogger() override;

  void addCollapseRule(const CollapseRule& rule);

  void handleDeviceInfo(const DeviceInfo& info, int64_t time) override;
  void handleResourceInfo(const ResourceInfo& info, int64_t time) override;
  void handleOverheadInfo(const OverheadInfo& info, int64_t time) override;
  void handleTraceSpan(const TraceSpan& span) override;
  void handleActivity(const libkineto::ITraceActivity& activity) override;
  void handleGenericActivity(
      const libkineto::GenericTraceActivity& activity) override;
  void handleTraceStart(
      const std::unordered_map<std::string, std::string>& metadata,
      const std::string& device_properties) override;
  void finalizeTrace(
      const libkineto::Config& config,
      std::unique_ptr<KINETO_NAMESPACE::ActivityBuffers> buffers,
      int64_t endTime,
      std::unordered_map<std::string, std::vector<std::string>>&
          metadata) override;
  void finalizeMemoryTrace(
      const std::string&, const libkineto::Config&) override;

 private:
  // Track UUID generation (FNV-1a based)
  static uint64_t deviceTrackUuid(int64_t device_id);
  static uint64_t resourceTrackUuid(int64_t device_id, int64_t resource_id);
  static uint64_t counterTrackUuid(
      int64_t device_id, int64_t resource_id, const std::string& counter_name);
  static uint64_t spanProcessTrackUuid();
  static uint64_t spanThreadTrackUuid(const std::string& span_name);
  static uint64_t overheadTrackUuid();

  // Packet writing
  void writePacket();
  void beginPacket(uint64_t timestamp_ns);

  // Interning (returns iid, records new entries for deferred emission)
  struct PendingInterning {
    std::vector<std::pair<uint64_t, std::string>> event_names;
    std::vector<std::pair<uint64_t, std::string>> event_categories;
    std::vector<std::pair<uint64_t, std::string>> debug_annotation_names;
    bool empty() const {
      return event_names.empty() && event_categories.empty() &&
             debug_annotation_names.empty();
    }
    void clear() {
      event_names.clear();
      event_categories.clear();
      debug_annotation_names.clear();
    }
  };

  uint64_t internEventName(const std::string& name);
  uint64_t internEventCategory(const std::string& category);
  uint64_t internDebugAnnotationName(const std::string& name);
  void flushPendingInterning();

  // Activity emission
  void emitSlice(const libkineto::ITraceActivity& activity);
  void emitInstantEvent(const libkineto::ITraceActivity& activity);
  void emitCounterEvent(const libkineto::ITraceActivity& activity);

  // Track descriptor emission (lazy, skips if already emitted)
  void ensureProcessTrack(
      uint64_t uuid, const std::string& name, int64_t pid,
      const std::string& label, int64_t sort_index);
  void ensureThreadTrack(
      uint64_t uuid, uint64_t parent_uuid, const std::string& name,
      int64_t pid, int64_t tid, int64_t sort_index);
  void ensureCounterTrack(
      uint64_t uuid, uint64_t parent_uuid, const std::string& name);

  // Metadata JSON → debug_annotations
  void emitDebugAnnotationsFromJson(
      perfetto::protos::pbzero::TrackEvent* event, const std::string& json);

  static int64_t sanitizeTid(int64_t tid);

  // File
  std::string fileName_;
  std::string tempFileName_;
  std::ofstream traceOf_;

  // Reusable packet buffer
  protozero::HeapBuffered<perfetto::protos::pbzero::TracePacket> packet_;

  // Interning state
  InterningState interning_;
  PendingInterning pending_;

  // Sequence control
  bool firstPacket_ = true;
  static constexpr uint32_t kSequenceId = 1;
  static constexpr int64_t kSyncStreamTidOffset = 1000000;

  // Track dedup
  std::unordered_set<uint64_t> emittedTracks_;
  std::unordered_set<int64_t> syncStreamEmitted_;

  // --- Collapse support ---

  struct CollapseStackEntry {
    std::string name;
    int64_t ts;
    int64_t end_ts;
    int64_t python_id = 0;
    int64_t python_parent_id = 0;
  };

  struct CollapseContext {
    const CollapseRule* rule = nullptr;
    int matched_depth = 0;
    int match_start_depth = 0;
    int64_t collapse_start_ts = 0;
    int64_t collapse_end_ts = 0;
    int64_t collapse_python_id = 0;
    int64_t collapse_python_parent_id = 0;
    bool begin_emitted = false;
  };

  struct CollapseState {
    std::vector<CollapseStackEntry> stack;
    std::vector<CollapseContext> contexts;  // nested collapse stack, innermost last
  };

  // Returns true if the event should be suppressed (part of an active collapse)
  bool processCollapse(
      uint64_t track_key, const libkineto::ITraceActivity& op,
      int64_t ts, int64_t end_ts);
  void emitCollapseBegin(uint64_t track_key, CollapseState& state);
  void flushCollapse(uint64_t track_key, CollapseState& state);

  std::vector<CollapseRule> collapseRules_;
  std::unordered_map<uint64_t, CollapseState> collapseStates_;
  // Maps suppressed Python id → collapsed event's Python id
  std::unordered_map<int64_t, int64_t> pythonIdRemap_;
};

}  // namespace perfetto_logger
