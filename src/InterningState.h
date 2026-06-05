#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

namespace perfetto_logger {

struct InterningState {
  std::unordered_map<std::string, uint64_t> event_names;
  std::unordered_map<std::string, uint64_t> event_categories;
  std::unordered_map<std::string, uint64_t> debug_annotation_names;

  uint64_t next_event_name_iid = 1;
  uint64_t next_event_category_iid = 1;
  uint64_t next_debug_annotation_name_iid = 1;

  void clear() {
    event_names.clear();
    event_categories.clear();
    debug_annotation_names.clear();
    next_event_name_iid = 1;
    next_event_category_iid = 1;
    next_debug_annotation_name_iid = 1;
  }
};

}  // namespace perfetto_logger
