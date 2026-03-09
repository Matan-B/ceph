// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:nil -*-
// vim: ts=8 sw=2 sts=2 expandtab

#pragma once

#include <boost/intrusive/set.hpp>

#include "crimson/common/log.h"

#include "crimson/os/seastore/cached_extent.h"
#include "crimson/os/seastore/seastore_types.h"
#include "crimson/os/seastore/logical_child_node.h"
#include "crimson/os/seastore/lba_mapping.h"

namespace crimson::os::seastore {

struct LBAOverlayCursor {
private:
  // this could be null for newly inserted ones
  LBACursorRef base_cursor;

  // overlaid values
  std::optional<extent_ref_count_t> overlaid_refcount;
  std::optional<std::vector<LogicalChildNodeRef>> alloc_extents;
  std::optional<paddr_t> address;

  friend class LBAOverlayManager;

public:
  LBAOverlayCursor(LBACursorRef base_cursor) : base_cursor(base_cursor) {}
  LBAOverlayCursor() = default;

  template<typename T>
  void set_overlay(std::optional<T> LBAOverlayCursor::*overlay_value, const T& value) {
      this->*overlay_value = value;
  }

  template<typename T>
  const T* get_overlay(std::optional<T> LBAOverlayCursor::*overlay_value) const {
    if ((this->*overlay_value).has_value()) {
      return &((this->*overlay_value).value());
    }
    return nullptr;
  }
};

} // namespace crimson::os::seastore
