#include "PerfettoLogger.h"

#include <algorithm>
#include <cstdio>
#include <set>

#include "GenericTraceActivity.h"
#include "ITraceActivity.h"

using perfetto::protos::pbzero::BuiltinClock;
using perfetto::protos::pbzero::TracePacket;
using perfetto::protos::pbzero::TrackDescriptor;
using perfetto::protos::pbzero::TrackEvent;

namespace perfetto_logger {

// FNV-1a 64-bit hash
static constexpr uint64_t kFnvOffset = 14695981039346656037ULL;
static constexpr uint64_t kFnvPrime = 1099511628211ULL;

static uint64_t fnvHash64(uint64_t a) {
  uint64_t h = kFnvOffset;
  for (int i = 0; i < 8; i++) {
    h ^= (a >> (i * 8)) & 0xFF;
    h *= kFnvPrime;
  }
  return h;
}

static uint64_t fnvHash64(uint64_t a, uint64_t b) {
  uint64_t h = kFnvOffset;
  for (int i = 0; i < 8; i++) {
    h ^= (a >> (i * 8)) & 0xFF;
    h *= kFnvPrime;
  }
  for (int i = 0; i < 8; i++) {
    h ^= (b >> (i * 8)) & 0xFF;
    h *= kFnvPrime;
  }
  return h;
}

static uint64_t fnvHashStr(const std::string& s) {
  uint64_t h = kFnvOffset;
  for (char c : s) {
    h ^= static_cast<uint8_t>(c);
    h *= kFnvPrime;
  }
  return h;
}

static constexpr uint64_t kSpanProcessMagic = 0x5350414E50524F43ULL;
static constexpr uint64_t kOverheadMagic = 0x4F56455248454144ULL;
static constexpr uint64_t kSpanThreadMagic = 0x5350414E54485244ULL;

static const char* kParamCommsCallName = "record_param_comms";

// ---------------------------------------------------------------------------
// Track UUID generation
// ---------------------------------------------------------------------------

uint64_t PerfettoLogger::deviceTrackUuid(int64_t device_id) {
  return fnvHash64(static_cast<uint64_t>(device_id));
}

uint64_t PerfettoLogger::resourceTrackUuid(
    int64_t device_id, int64_t resource_id) {
  return fnvHash64(
      static_cast<uint64_t>(device_id), static_cast<uint64_t>(resource_id));
}

uint64_t PerfettoLogger::counterTrackUuid(
    int64_t device_id, int64_t resource_id, const std::string& counter_name) {
  uint64_t base = resourceTrackUuid(device_id, resource_id);
  return fnvHash64(base, fnvHashStr(counter_name));
}

uint64_t PerfettoLogger::spanProcessTrackUuid() {
  return fnvHash64(kSpanProcessMagic);
}

uint64_t PerfettoLogger::spanThreadTrackUuid(const std::string& span_name) {
  return fnvHash64(kSpanThreadMagic, fnvHashStr(span_name));
}

uint64_t PerfettoLogger::overheadTrackUuid() {
  return fnvHash64(kOverheadMagic);
}

// ---------------------------------------------------------------------------
// Tid sanitization (matches ChromeTraceLogger)
// ---------------------------------------------------------------------------

int64_t PerfettoLogger::sanitizeTid(int64_t tid) {
  if (tid == INT64_MIN) {
    return 0;
  }
  return tid < 0 ? -tid : tid;
}

// ---------------------------------------------------------------------------
// Constructor / Destructor
// ---------------------------------------------------------------------------

PerfettoLogger::PerfettoLogger(const std::string& filename)
    : fileName_(filename), tempFileName_(filename + ".tmp") {
  traceOf_.open(tempFileName_, std::ios::binary | std::ios::trunc);
}

PerfettoLogger::~PerfettoLogger() {
  if (traceOf_.is_open()) {
    traceOf_.close();
  }
}

void PerfettoLogger::addCollapseRule(const CollapseRule& rule) {
  collapseRules_.push_back(rule);
}

// ---------------------------------------------------------------------------
// Collapse support
// ---------------------------------------------------------------------------

static int64_t extractPythonId(const libkineto::ITraceActivity& op,
                                const char* key) {
  std::string val = op.getMetadataValue(key);
  if (val.empty() || val == "null") return 0;
  try { return std::stoll(val); }
  catch (...) { return 0; }
}

static bool nameMatchesPrefixes(
    const std::string& name, const std::vector<std::string>& prefixes) {
  for (auto& p : prefixes) {
    if (name.find(p) != std::string::npos) return true;
  }
  return false;
}

static bool ruleMatchesName(const CollapseRule& rule, const std::string& name,
                            int pattern_index) {
  if (rule.mode == CollapseRule::STRICT_PATTERN) {
    return pattern_index < static_cast<int>(rule.pattern.size()) &&
           name.find(rule.pattern[pattern_index]) != std::string::npos;
  }
  // PREFIX_SET: any prefix in the set matches
  return nameMatchesPrefixes(name, rule.prefixes);
}

static bool ruleCanStartWith(const CollapseRule& rule, const std::string& name) {
  if (rule.mode == CollapseRule::STRICT_PATTERN) {
    return !rule.pattern.empty() &&
           name.find(rule.pattern[0]) != std::string::npos;
  }
  return nameMatchesPrefixes(name, rule.prefixes);
}

static bool ruleFullyMatched(const CollapseRule& rule, int matched_depth) {
  if (rule.mode == CollapseRule::STRICT_PATTERN) {
    return matched_depth >= static_cast<int>(rule.pattern.size());
  }
  // PREFIX_SET: never "fully matched" — continues until a non-match breaks it
  return false;
}

bool PerfettoLogger::processCollapse(
    uint64_t track_key, const libkineto::ITraceActivity& op,
    int64_t ts, int64_t end_ts) {
  if (collapseRules_.empty()) return false;

  auto& state = collapseStates_[track_key];

  // Pop expired events from the stack
  while (!state.stack.empty() && state.stack.back().end_ts <= ts) {
    int current_depth = static_cast<int>(state.stack.size());

    if (!state.contexts.empty() &&
        state.contexts.back().match_start_depth == current_depth) {
      auto& ctx = state.contexts.back();
      if (ctx.rule->mode == CollapseRule::PREFIX_SET) {
        // Defer flush — a sibling at the same depth might extend it
        ctx.collapse_end_ts = std::max(
            ctx.collapse_end_ts, state.stack.back().end_ts);
        state.stack.pop_back();
        ctx.matched_depth = 0;
      } else {
        // STRICT_PATTERN: flush immediately
        flushCollapse(track_key, state);
        state.stack.pop_back();
      }
    } else {
      state.stack.pop_back();
    }

    // Flush contexts whose root can no longer receive siblings
    while (!state.contexts.empty()) {
      int gap = state.contexts.back().match_start_depth -
                static_cast<int>(state.stack.size());
      if (gap <= 1) break;
      flushCollapse(track_key, state);
    }

    // Adjust matched_depth for the innermost context
    if (!state.contexts.empty()) {
      auto& ctx = state.contexts.back();
      int current_relative =
          static_cast<int>(state.stack.size()) - ctx.match_start_depth + 1;
      if (current_relative < ctx.matched_depth) {
        ctx.matched_depth = std::max(current_relative, 0);
        if (ctx.matched_depth <= 0 &&
            ctx.rule->mode != CollapseRule::PREFIX_SET) {
          flushCollapse(track_key, state);
        }
      }
    }
  }

  std::string name = op.name();
  int64_t py_id = extractPythonId(op, "Python id");
  int64_t py_parent = extractPythonId(op, "Python parent id");

  // Push current event
  state.stack.push_back({name, ts, end_ts, py_id, py_parent});
  int depth = static_cast<int>(state.stack.size());

  // Match against active contexts or start new ones.
  // The loop retries with outer contexts after flushing an inner one.
  while (true) {
    if (state.contexts.empty()) {
      // No active context — try to start a new collapse
      for (auto& rule : collapseRules_) {
        if (ruleCanStartWith(rule, name)) {
          CollapseContext ctx;
          ctx.rule = &rule;
          ctx.match_start_depth = depth;
          ctx.matched_depth = 1;
          ctx.collapse_start_ts = ts;
          ctx.collapse_end_ts = end_ts;
          ctx.collapse_python_id = py_id;
          ctx.collapse_python_parent_id = py_parent;
          state.contexts.push_back(ctx);
          if (py_id != 0) {
            pythonIdRemap_[py_id] = py_id;
          }
          emitCollapseBegin(track_key, state);
          return true;
        }
      }
      return false;
    }

    // Save the active rule pointer before any push_back that could
    // reallocate the contexts vector and invalidate references.
    const CollapseRule* active_rule = state.contexts.back().rule;
    int relative_depth =
        depth - state.contexts.back().match_start_depth + 1;

    if (active_rule->mode == CollapseRule::PREFIX_SET) {
      if (relative_depth > 1) {
        if (nameMatchesPrefixes(name, active_rule->prefixes)) {
          auto& ctx = state.contexts.back();
          ctx.matched_depth = std::max(ctx.matched_depth, relative_depth);
          if (py_id != 0) {
            pythonIdRemap_[py_id] = ctx.collapse_python_id;
          }
          return true;  // suppress by active rule
        }
        // Non-matching descendant — try nested collapse with other rules
        for (auto& r : collapseRules_) {
          if (&r == active_rule) continue;
          if (ruleCanStartWith(r, name)) {
            CollapseContext ctx;
            ctx.rule = &r;
            ctx.match_start_depth = depth;
            ctx.matched_depth = 1;
            ctx.collapse_start_ts = ts;
            ctx.collapse_end_ts = end_ts;
            ctx.collapse_python_id = py_id;
            ctx.collapse_python_parent_id = py_parent;
            state.contexts.push_back(ctx);
            if (py_id != 0) {
              pythonIdRemap_[py_id] = py_id;
            }
            emitCollapseBegin(track_key, state);
            return true;  // suppress by nested rule
          }
        }
        return false;  // no match, emit normally
      }
      // relative_depth <= 1: sibling or higher of collapse root
      if (nameMatchesPrefixes(name, active_rule->prefixes)) {
        auto& ctx = state.contexts.back();
        ctx.match_start_depth = depth;
        ctx.matched_depth = 1;
        ctx.collapse_end_ts = std::max(ctx.collapse_end_ts, end_ts);
        if (py_id != 0) {
          pythonIdRemap_[py_id] = ctx.collapse_python_id;
        }
        return true;  // suppress — extends existing collapse
      }
      // Non-matching at root level — flush and retry with outer context
      flushCollapse(track_key, state);
      continue;
    }

    // STRICT_PATTERN
    if (!ruleFullyMatched(*active_rule, state.contexts.back().matched_depth) &&
        relative_depth == state.contexts.back().matched_depth + 1 &&
        ruleMatchesName(*active_rule, name,
                        state.contexts.back().matched_depth)) {
      auto& ctx = state.contexts.back();
      ctx.matched_depth = relative_depth;
      if (py_id != 0) {
        pythonIdRemap_[py_id] = ctx.collapse_python_id;
      }
      return true;  // suppress
    }
    if (ruleFullyMatched(*active_rule,
                         state.contexts.back().matched_depth)) {
      flushCollapse(track_key, state);
      continue;  // retry with outer context or start new
    }
    // Doesn't extend current STRICT_PATTERN — try nested collapse
    for (auto& r : collapseRules_) {
      if (&r == active_rule) continue;
      if (ruleCanStartWith(r, name)) {
        CollapseContext ctx;
        ctx.rule = &r;
        ctx.match_start_depth = depth;
        ctx.matched_depth = 1;
        ctx.collapse_start_ts = ts;
        ctx.collapse_end_ts = end_ts;
        ctx.collapse_python_id = py_id;
        ctx.collapse_python_parent_id = py_parent;
        state.contexts.push_back(ctx);
        if (py_id != 0) {
          pythonIdRemap_[py_id] = py_id;
        }
        emitCollapseBegin(track_key, state);
        return true;  // suppress by nested rule
      }
    }
    return false;
  }
}

void PerfettoLogger::emitCollapseBegin(
    uint64_t track_key, CollapseState& state) {
  auto& ctx = state.contexts.back();

  uint64_t nameIid = internEventName(ctx.rule->collapsed_name);
  uint64_t catIid = internEventCategory("collapsed");

  beginPacket(static_cast<uint64_t>(ctx.collapse_start_ts));
  flushPendingInterning();
  auto* event = packet_->set_track_event();
  event->set_type(TrackEvent::TYPE_SLICE_BEGIN);
  event->set_track_uuid(track_key);
  event->set_name_iid(nameIid);
  event->add_category_iids(catIid);

  if (ctx.collapse_python_id != 0) {
    auto* ann = event->add_debug_annotations();
    ann->set_name("Python id");
    ann->set_int_value(ctx.collapse_python_id);
  }
  if (ctx.collapse_python_parent_id != 0) {
    auto* ann = event->add_debug_annotations();
    ann->set_name("Python parent id");
    ann->set_int_value(ctx.collapse_python_parent_id);
  }

  writePacket();
  ctx.begin_emitted = true;
}

void PerfettoLogger::flushCollapse(uint64_t track_key, CollapseState& state) {
  if (state.contexts.empty()) return;
  auto& ctx = state.contexts.back();

  if (!ctx.begin_emitted) {
    emitCollapseBegin(track_key, state);
  }

  // SLICE_END
  beginPacket(static_cast<uint64_t>(ctx.collapse_end_ts));
  auto* endEvent = packet_->set_track_event();
  endEvent->set_type(TrackEvent::TYPE_SLICE_END);
  endEvent->set_track_uuid(track_key);
  writePacket();

  state.contexts.pop_back();
}

// ---------------------------------------------------------------------------
// Packet writing
// ---------------------------------------------------------------------------

void PerfettoLogger::writePacket() {
  auto bytes = packet_.SerializeAsArray();
  uint8_t preamble[16];
  uint8_t* pos = preamble;
  *pos++ = 0x0A;  // field 1, wire type 2 (length-delimited)
  pos = protozero::proto_utils::WriteVarInt(bytes.size(), pos);
  traceOf_.write(reinterpret_cast<const char*>(preamble), pos - preamble);
  traceOf_.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  packet_.Reset();
}

void PerfettoLogger::beginPacket(uint64_t timestamp_ns) {
  packet_->set_timestamp(timestamp_ns);
  packet_->set_trusted_packet_sequence_id(kSequenceId);
  if (firstPacket_) {
    packet_->set_sequence_flags(TracePacket::SEQ_INCREMENTAL_STATE_CLEARED);
    firstPacket_ = false;
  } else {
    packet_->set_sequence_flags(TracePacket::SEQ_NEEDS_INCREMENTAL_STATE);
  }
}

// ---------------------------------------------------------------------------
// Interning
// ---------------------------------------------------------------------------

uint64_t PerfettoLogger::internEventName(const std::string& name) {
  auto it = interning_.event_names.find(name);
  if (it != interning_.event_names.end()) {
    return it->second;
  }
  uint64_t iid = interning_.next_event_name_iid++;
  interning_.event_names[name] = iid;
  pending_.event_names.emplace_back(iid, name);
  return iid;
}

uint64_t PerfettoLogger::internEventCategory(const std::string& category) {
  auto it = interning_.event_categories.find(category);
  if (it != interning_.event_categories.end()) {
    return it->second;
  }
  uint64_t iid = interning_.next_event_category_iid++;
  interning_.event_categories[category] = iid;
  pending_.event_categories.emplace_back(iid, category);
  return iid;
}

uint64_t PerfettoLogger::internDebugAnnotationName(const std::string& name) {
  auto it = interning_.debug_annotation_names.find(name);
  if (it != interning_.debug_annotation_names.end()) {
    return it->second;
  }
  uint64_t iid = interning_.next_debug_annotation_name_iid++;
  interning_.debug_annotation_names[name] = iid;
  pending_.debug_annotation_names.emplace_back(iid, name);
  return iid;
}

void PerfettoLogger::flushPendingInterning() {
  if (pending_.empty()) {
    return;
  }
  auto* interned = packet_->set_interned_data();
  for (auto& [iid, name] : pending_.event_names) {
    auto* entry = interned->add_event_names();
    entry->set_iid(iid);
    entry->set_name(name.data(), name.size());
  }
  for (auto& [iid, cat] : pending_.event_categories) {
    auto* entry = interned->add_event_categories();
    entry->set_iid(iid);
    entry->set_name(cat.data(), cat.size());
  }
  for (auto& [iid, name] : pending_.debug_annotation_names) {
    auto* entry = interned->add_debug_annotation_names();
    entry->set_iid(iid);
    entry->set_name(name.data(), name.size());
  }
  pending_.clear();
}

// ---------------------------------------------------------------------------
// Track descriptor emission
// ---------------------------------------------------------------------------

void PerfettoLogger::ensureProcessTrack(
    uint64_t uuid, const std::string& name, int64_t pid,
    const std::string& label, int64_t sort_index) {
  if (!emittedTracks_.insert(uuid).second) {
    return;
  }
  packet_.Reset();
  packet_->set_trusted_packet_sequence_id(kSequenceId);
  auto* td = packet_->set_track_descriptor();
  td->set_uuid(uuid);
  td->set_name(name.data(), name.size());
  auto* proc = td->set_process();
  proc->set_pid(static_cast<int32_t>(pid));
  proc->set_process_name(name.data(), name.size());
  if (!label.empty()) {
    proc->add_cmdline(label.data(), label.size());
  }
  td->set_sibling_order_rank(static_cast<int32_t>(sort_index));
  writePacket();
}

void PerfettoLogger::ensureThreadTrack(
    uint64_t uuid, uint64_t parent_uuid, const std::string& name,
    int64_t pid, int64_t tid, int64_t sort_index) {
  if (!emittedTracks_.insert(uuid).second) {
    return;
  }
  packet_.Reset();
  packet_->set_trusted_packet_sequence_id(kSequenceId);
  auto* td = packet_->set_track_descriptor();
  td->set_uuid(uuid);
  td->set_parent_uuid(parent_uuid);
  td->set_name(name.data(), name.size());
  auto* thr = td->set_thread();
  thr->set_pid(static_cast<int32_t>(pid));
  thr->set_tid(tid);
  thr->set_thread_name(name.data(), name.size());
  td->set_sibling_order_rank(static_cast<int32_t>(sort_index));
  writePacket();
}

void PerfettoLogger::ensureCounterTrack(
    uint64_t uuid, uint64_t parent_uuid, const std::string& name) {
  if (!emittedTracks_.insert(uuid).second) {
    return;
  }
  packet_.Reset();
  packet_->set_trusted_packet_sequence_id(kSequenceId);
  auto* td = packet_->set_track_descriptor();
  td->set_uuid(uuid);
  td->set_parent_uuid(parent_uuid);
  td->set_name(name.data(), name.size());
  td->set_counter();
  writePacket();
}

// ---------------------------------------------------------------------------
// handleTraceStart
// ---------------------------------------------------------------------------

void PerfettoLogger::handleTraceStart(
    const std::unordered_map<std::string, std::string>& /*metadata*/,
    const std::string& /*device_properties*/) {
  if (!traceOf_) {
    return;
  }
  // Emit the initial state-clear packet. Use BUILTIN_CLOCK_BOOTTIME (6) which
  // is the default reference clock — no ClockSnapshot needed for offline traces.
  beginPacket(0);
  auto* defaults = packet_->set_trace_packet_defaults();
  defaults->set_timestamp_clock_id(
      static_cast<uint32_t>(BuiltinClock::BUILTIN_CLOCK_BOOTTIME));
  writePacket();
}

// ---------------------------------------------------------------------------
// handleDeviceInfo
// ---------------------------------------------------------------------------

void PerfettoLogger::handleDeviceInfo(const DeviceInfo& info, int64_t /*time*/) {
  if (!traceOf_) {
    return;
  }
  uint64_t uuid = deviceTrackUuid(info.id);
  ensureProcessTrack(uuid, info.name, info.id, info.label, info.sortIndex);
}

// ---------------------------------------------------------------------------
// handleResourceInfo
// ---------------------------------------------------------------------------

void PerfettoLogger::handleResourceInfo(
    const ResourceInfo& info, int64_t /*time*/) {
  if (!traceOf_) {
    return;
  }
  int64_t tid = sanitizeTid(info.id);
  uint64_t parent = deviceTrackUuid(info.deviceId);
  uint64_t uuid = resourceTrackUuid(info.deviceId, tid);
  ensureThreadTrack(uuid, parent, info.name, info.deviceId, tid, info.sortIndex);
}

// ---------------------------------------------------------------------------
// handleOverheadInfo
// ---------------------------------------------------------------------------

void PerfettoLogger::handleOverheadInfo(
    const OverheadInfo& info, int64_t /*time*/) {
  if (!traceOf_) {
    return;
  }
  uint64_t uuid = overheadTrackUuid();
  ensureProcessTrack(uuid, info.name, -1, "", 0x100000A);
}

// ---------------------------------------------------------------------------
// handleTraceSpan
// ---------------------------------------------------------------------------

void PerfettoLogger::handleTraceSpan(const TraceSpan& span) {
  if (!traceOf_) {
    return;
  }

  uint64_t procUuid = spanProcessTrackUuid();
  ensureProcessTrack(procUuid, "Spans", 0x20000000, "", 0x20000000);

  uint64_t threadUuid = spanThreadTrackUuid(span.name);
  ensureThreadTrack(
      threadUuid, procUuid, span.name, 0x20000000, 0, 0);

  int64_t dur = (span.endTime == 0) ? 0 : span.endTime - span.startTime;
  std::string spanName =
      span.prefix + span.name + " (" + std::to_string(span.iteration) + ")";

  // Pre-intern
  uint64_t nameIid = internEventName(spanName);
  uint64_t catIid = internEventCategory("Trace");
  uint64_t opCountKeyIid = internDebugAnnotationName("Op count");

  // SLICE_BEGIN
  beginPacket(static_cast<uint64_t>(span.startTime));
  flushPendingInterning();
  auto* event = packet_->set_track_event();
  event->set_type(TrackEvent::TYPE_SLICE_BEGIN);
  event->set_track_uuid(threadUuid);
  event->set_name_iid(nameIid);
  event->add_category_iids(catIid);
  auto* ann = event->add_debug_annotations();
  ann->set_name_iid(opCountKeyIid);
  ann->set_int_value(span.opCount);
  writePacket();

  // SLICE_END
  int64_t endTs = span.startTime + dur;
  beginPacket(static_cast<uint64_t>(endTs));
  auto* endEvent = packet_->set_track_event();
  endEvent->set_type(TrackEvent::TYPE_SLICE_END);
  endEvent->set_track_uuid(threadUuid);
  writePacket();

  // Iteration marker instant event
  std::string markerName = "Iteration Start: " + span.name;
  uint64_t markerNameIid = internEventName(markerName);

  beginPacket(static_cast<uint64_t>(span.startTime));
  flushPendingInterning();
  auto* marker = packet_->set_track_event();
  marker->set_type(TrackEvent::TYPE_INSTANT);
  marker->set_track_uuid(threadUuid);
  marker->set_name_iid(markerNameIid);
  writePacket();
}

// ---------------------------------------------------------------------------
// handleActivity / handleGenericActivity
// ---------------------------------------------------------------------------

void PerfettoLogger::handleActivity(const libkineto::ITraceActivity& op) {
  if (!traceOf_) {
    return;
  }

  if (op.type() == libkineto::ActivityType::CPU_INSTANT_EVENT) {
    emitInstantEvent(op);
    return;
  }

  switch (op.type()) {
    case libkineto::ActivityType::MTIA_COUNTERS:
    case libkineto::ActivityType::XPU_SCOPE_PROFILER:
      emitCounterEvent(op);
      return;
    default:
      break;
  }

  emitSlice(op);
}

void PerfettoLogger::handleGenericActivity(
    const libkineto::GenericTraceActivity& op) {
  handleActivity(op);
}

// ---------------------------------------------------------------------------
// emitSlice
// ---------------------------------------------------------------------------

void PerfettoLogger::emitSlice(const libkineto::ITraceActivity& op) {
  int64_t ts = op.timestamp();
  int64_t duration = std::max<int64_t>(op.duration(), 0);

  // Unlike ChromeTraceLogger, we do NOT adjust GPU_USER_ANNOTATION timestamps.
  // Perfetto handles overlapping events correctly.

  int64_t device = op.deviceId();
  int64_t resource = op.resourceId();

  // Stream Sync virtual thread
  if (op.type() == libkineto::ActivityType::CUDA_SYNC &&
      op.name() == "Stream Sync") {
    int64_t syncTid = resource + kSyncStreamTidOffset;
    int64_t key = (device << 32) | syncTid;
    if (syncStreamEmitted_.insert(key).second) {
      uint64_t parent = deviceTrackUuid(device);
      uint64_t syncUuid = resourceTrackUuid(device, syncTid);
      std::string syncName =
          "stream " + std::to_string(resource) + " (sync)";
      ensureThreadTrack(
          syncUuid, parent, syncName, device, syncTid, resource);
    }
    resource = syncTid;
  }

  int64_t tid = sanitizeTid(resource);
  uint64_t trackUuid = resourceTrackUuid(device, tid);

  // Ensure the track exists (in case handleResourceInfo wasn't called for it)
  ensureThreadTrack(
      trackUuid, deviceTrackUuid(device), "",
      device, tid, 0);

  // Collapse check — suppress events that are part of a matched pattern
  if (processCollapse(trackUuid, op, ts, ts + duration)) {
    return;
  }

  // External ID (same logic as ChromeTraceLogger)
  int64_t external_id = 0;
  if (op.linkedActivity()) {
    external_id = op.linkedActivity()->correlationId();
  } else {
    static const std::set<libkineto::ActivityType> excludedTypes = {
        libkineto::ActivityType::GPU_MEMCPY,
        libkineto::ActivityType::GPU_MEMSET,
        libkineto::ActivityType::CONCURRENT_KERNEL,
        libkineto::ActivityType::CUDA_RUNTIME,
        libkineto::ActivityType::CUDA_DRIVER,
        libkineto::ActivityType::PRIVATEUSE1_RUNTIME,
        libkineto::ActivityType::PRIVATEUSE1_DRIVER};
    if (excludedTypes.find(op.type()) == excludedTypes.end()) {
      external_id = op.correlationId();
    }
  }

  // Pre-intern everything before building the packet
  std::string opName = op.name();
  if (opName == "kernel") {
    opName = "Kernel";
  }
  uint64_t nameIid = internEventName(opName);
  uint64_t catIid = internEventCategory(libkineto::toString(op.type()));

  // Pre-scan metadata for annotation key interning
  std::string metaJson = op.metadataJson();

  // Pre-intern "External id" if needed
  uint64_t extIdKeyIid = 0;
  if (external_id != 0) {
    extIdKeyIid = internDebugAnnotationName("External id");
  }

  // Pre-intern NCCL metadata keys if applicable
  bool hasNccl = false;
  const libkineto::ITraceActivity* ncclRecord = nullptr;
  if (op.type() == libkineto::ActivityType::CONCURRENT_KERNEL &&
      op.linkedActivity() &&
      op.linkedActivity()->name() == kParamCommsCallName) {
    hasNccl = true;
    ncclRecord = op.linkedActivity();
  }

  // --- SLICE_BEGIN packet ---
  beginPacket(static_cast<uint64_t>(ts));
  flushPendingInterning();
  auto* event = packet_->set_track_event();
  event->set_type(TrackEvent::TYPE_SLICE_BEGIN);
  event->set_track_uuid(trackUuid);
  event->set_name_iid(nameIid);
  event->add_category_iids(catIid);

  if (external_id != 0) {
    event->set_correlation_id(static_cast<uint64_t>(external_id));
    auto* ann = event->add_debug_annotations();
    ann->set_name_iid(extIdKeyIid);
    ann->set_int_value(external_id);
  }

  // Flow IDs
  if (op.flowId() > 0) {
    uint64_t nsFlowId =
        (static_cast<uint64_t>(op.flowType()) << 32) |
        static_cast<uint64_t>(op.flowId());
    if (op.flowStart()) {
      event->add_flow_ids(nsFlowId);
    } else {
      event->add_terminating_flow_ids(nsFlowId);
    }
  }

  // Metadata as debug annotations
  emitDebugAnnotationsFromJson(event, metaJson);

  // NCCL collective metadata
  if (hasNccl && ncclRecord) {
    auto addStr = [&](const char* key, const std::string& val) {
      if (val.empty()) return;
      auto* a = event->add_debug_annotations();
      a->set_name(key);
      a->set_string_value(val.data(), val.size());
    };
    auto addInt = [&](const char* key, const std::string& val) {
      if (val.empty()) return;
      auto* a = event->add_debug_annotations();
      a->set_name(key);
      try {
        a->set_int_value(std::stoll(val));
      } catch (...) {
        a->set_string_value(val.data(), val.size());
      }
    };

    addStr("Collective name", ncclRecord->getMetadataValue("Collective name"));
    addStr("dtype", ncclRecord->getMetadataValue("dtype"));
    addInt("In msg nelems", ncclRecord->getMetadataValue("In msg nelems"));
    addInt("Out msg nelems", ncclRecord->getMetadataValue("Out msg nelems"));
    addInt("Group size", ncclRecord->getMetadataValue("Group size"));
    addStr("Process Group Name",
           ncclRecord->getMetadataValue("Process Group Name"));
    addStr("Process Group Description",
           ncclRecord->getMetadataValue("Process Group Description"));
    addStr("Process Group Ranks",
           ncclRecord->getMetadataValue("Process Group Ranks"));
    addStr("In split size", ncclRecord->getMetadataValue("In split size"));
    addStr("Out split size", ncclRecord->getMetadataValue("Out split size"));
    addInt("Rank", ncclRecord->getMetadataValue("Rank"));
    addInt("Src Rank", ncclRecord->getMetadataValue("Src Rank"));
    addInt("Dst Rank", ncclRecord->getMetadataValue("Dst Rank"));
    addInt("Seq", ncclRecord->getMetadataValue("Seq"));
    addStr("Comms Id", ncclRecord->getMetadataValue("Comms Id"));
  }

  writePacket();

  // --- SLICE_END packet ---
  beginPacket(static_cast<uint64_t>(ts + duration));
  auto* endEvent = packet_->set_track_event();
  endEvent->set_type(TrackEvent::TYPE_SLICE_END);
  endEvent->set_track_uuid(trackUuid);
  writePacket();
}

// ---------------------------------------------------------------------------
// emitInstantEvent
// ---------------------------------------------------------------------------

void PerfettoLogger::emitInstantEvent(const libkineto::ITraceActivity& op) {
  int64_t tid = sanitizeTid(op.resourceId());
  uint64_t trackUuid = resourceTrackUuid(op.deviceId(), tid);
  ensureThreadTrack(
      trackUuid, deviceTrackUuid(op.deviceId()), "",
      op.deviceId(), tid, 0);

  uint64_t nameIid = internEventName(op.name());
  uint64_t catIid = internEventCategory(libkineto::toString(op.type()));

  beginPacket(static_cast<uint64_t>(op.timestamp()));
  flushPendingInterning();
  auto* event = packet_->set_track_event();
  event->set_type(TrackEvent::TYPE_INSTANT);
  event->set_track_uuid(trackUuid);
  event->set_name_iid(nameIid);
  event->add_category_iids(catIid);

  emitDebugAnnotationsFromJson(event, op.metadataJson());
  writePacket();
}

// ---------------------------------------------------------------------------
// emitCounterEvent
// ---------------------------------------------------------------------------

void PerfettoLogger::emitCounterEvent(const libkineto::ITraceActivity& op) {
  int64_t device = op.deviceId();
  int64_t resource = sanitizeTid(op.resourceId());

  for (const auto& [name, value] : op.counterValues()) {
    uint64_t ctrUuid = counterTrackUuid(device, resource, name);
    uint64_t parentUuid = resourceTrackUuid(device, resource);
    ensureCounterTrack(ctrUuid, parentUuid, name);

    beginPacket(static_cast<uint64_t>(op.timestamp()));
    auto* event = packet_->set_track_event();
    event->set_type(TrackEvent::TYPE_COUNTER);
    event->set_track_uuid(ctrUuid);
    event->set_double_counter_value(value);
    writePacket();
  }
}

// ---------------------------------------------------------------------------
// Metadata JSON → debug_annotations
// ---------------------------------------------------------------------------

// Lightweight parser for Kineto's flat metadata JSON format.
// Input is like: "key1": 123, "key2": "str", "key3": true
// No outer braces.
void PerfettoLogger::emitDebugAnnotationsFromJson(
    TrackEvent* event, const std::string& json) {
  if (json.empty()) {
    return;
  }

  // Trim whitespace
  size_t pos = 0;
  size_t len = json.size();

  auto skipWs = [&]() {
    while (pos < len && (json[pos] == ' ' || json[pos] == '\t' ||
                         json[pos] == '\n' || json[pos] == '\r')) {
      pos++;
    }
  };

  while (pos < len) {
    skipWs();
    if (pos >= len) break;

    // Expect "key"
    if (json[pos] != '"') break;
    pos++;  // skip opening quote
    size_t keyStart = pos;
    while (pos < len && json[pos] != '"') {
      if (json[pos] == '\\') pos++;  // skip escaped char
      pos++;
    }
    if (pos >= len) break;
    std::string key = json.substr(keyStart, pos - keyStart);
    pos++;  // skip closing quote

    skipWs();
    if (pos >= len || json[pos] != ':') break;
    pos++;  // skip colon
    skipWs();
    if (pos >= len) break;

    auto* ann = event->add_debug_annotations();
    ann->set_name(key.data(), key.size());

    if (json[pos] == '"') {
      // String value
      pos++;
      size_t valStart = pos;
      while (pos < len && json[pos] != '"') {
        if (json[pos] == '\\') pos++;
        pos++;
      }
      std::string val = json.substr(valStart, pos - valStart);
      if (pos < len) pos++;  // skip closing quote
      ann->set_string_value(val.data(), val.size());
    } else if (json[pos] == 'n') {
      // null — skip the value, emit nothing (annotation has name but no value)
      while (pos < len && json[pos] != ',' && json[pos] != '}') pos++;
    } else if (json[pos] == 't' || json[pos] == 'f') {
      // Boolean
      bool bval = (json[pos] == 't');
      while (pos < len && json[pos] != ',' && json[pos] != '}') pos++;
      ann->set_bool_value(bval);
    } else if (json[pos] == '[') {
      // Array — emit as legacy JSON string for now
      int depth = 0;
      size_t arrStart = pos;
      while (pos < len) {
        if (json[pos] == '[') depth++;
        else if (json[pos] == ']') { depth--; if (depth == 0) { pos++; break; } }
        pos++;
      }
      std::string arrStr = json.substr(arrStart, pos - arrStart);
      ann->set_string_value(arrStr.data(), arrStr.size());
    } else if (json[pos] == '{') {
      // Nested object — emit as legacy JSON string
      int depth = 0;
      size_t objStart = pos;
      while (pos < len) {
        if (json[pos] == '{') depth++;
        else if (json[pos] == '}') { depth--; if (depth == 0) { pos++; break; } }
        pos++;
      }
      std::string objStr = json.substr(objStart, pos - objStart);
      ann->set_string_value(objStr.data(), objStr.size());
    } else {
      // Number (int or float)
      size_t numStart = pos;
      bool isFloat = false;
      if (json[pos] == '-') pos++;
      while (pos < len && (json[pos] >= '0' && json[pos] <= '9')) pos++;
      if (pos < len && (json[pos] == '.' || json[pos] == 'e' || json[pos] == 'E')) {
        isFloat = true;
        pos++;
        while (pos < len && ((json[pos] >= '0' && json[pos] <= '9') ||
                              json[pos] == '+' || json[pos] == '-' ||
                              json[pos] == 'e' || json[pos] == 'E')) {
          pos++;
        }
      }
      std::string numStr = json.substr(numStart, pos - numStart);
      if (isFloat) {
        try { ann->set_double_value(std::stod(numStr)); }
        catch (...) { ann->set_string_value(numStr.data(), numStr.size()); }
      } else {
        try {
          int64_t ival = std::stoll(numStr);
          // Remap Python parent id if it points to a collapsed event
          if (key == "Python parent id" && !pythonIdRemap_.empty()) {
            auto it = pythonIdRemap_.find(ival);
            if (it != pythonIdRemap_.end()) {
              ival = it->second;
            }
          }
          ann->set_int_value(ival);
        }
        catch (...) { ann->set_string_value(numStr.data(), numStr.size()); }
      }
    }

    skipWs();
    if (pos < len && json[pos] == ',') pos++;
  }
}

// ---------------------------------------------------------------------------
// finalizeTrace
// ---------------------------------------------------------------------------

void PerfettoLogger::finalizeTrace(
    const libkineto::Config& /*config*/,
    std::unique_ptr<KINETO_NAMESPACE::ActivityBuffers> /*buffers*/,
    int64_t endTime,
    std::unordered_map<std::string, std::vector<std::string>>& /*metadata*/) {
  if (!traceOf_) {
    return;
  }

  // Flush any pending collapses (innermost first)
  for (auto& [key, state] : collapseStates_) {
    while (!state.contexts.empty()) {
      flushCollapse(key, state);
    }
  }

  // Emit "Record Window End" instant event
  beginPacket(static_cast<uint64_t>(endTime));
  auto* event = packet_->set_track_event();
  event->set_type(TrackEvent::TYPE_INSTANT);
  event->set_name("Record Window End");
  writePacket();

  traceOf_.flush();
  traceOf_.close();

  std::remove(fileName_.c_str());
  std::rename(tempFileName_.c_str(), fileName_.c_str());
}

void PerfettoLogger::finalizeMemoryTrace(
    const std::string& /*unused*/, const libkineto::Config& /*config*/) {
  // No-op for file loggers
}

}  // namespace perfetto_logger
