#include "state.h"

namespace z3lsp {

void DocumentState::BuildLookupMaps() {
  label_map.clear();
  define_map.clear();
  for (const auto& label : labels) {
    label_map[label.name] = &label;
  }
  for (const auto& define : defines) {
    define_map[define.name] = &define;
  }
  address_to_label_map.clear();
  for (const auto& label : labels) {
    if (address_to_label_map.find(label.address) == address_to_label_map.end()) {
      address_to_label_map[label.address] = label.name;
    }
  }
}

}  // namespace z3lsp
