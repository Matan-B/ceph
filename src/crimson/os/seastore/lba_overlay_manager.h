// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:nil -*-
// vim: ts=8 sw=2 sts=2 expandtab

#pragma once

#include <iostream>

#include <boost/intrusive_ptr.hpp>
#include <boost/smart_ptr/intrusive_ref_counter.hpp>

#include <seastar/core/future.hh>

#include "include/ceph_assert.h"
#include "include/buffer_fwd.h"
#include "include/interval_set.h"
#include "common/interval_map.h"

#include "crimson/osd/exceptions.h"

#include "crimson/os/seastore/cache.h"
#include "crimson/os/seastore/seastore_types.h"
#include "crimson/os/seastore/lba_mapping.h"
#include "crimson/os/seastore/logical_child_node.h"

#include "crimson/os/seastore/lba_manager.h"

namespace crimson::os::seastore {

/**
 * <todo>
 */
class LBAOverlayManager;
using LBAOverlayManagerRef = std::unique_ptr<LBAOverlayManager>;

class LBAOverlayManager { //  : public LBAManager
//inherit later, avoid overridng all methods..

private:
  enum class op_type {
    mkfs,
    init_cached_extent,
    alloc_extent,
    alloc_extents,
    clone_mapping,
    reserve_region,
    rewrite_extent,
    get_physical_extent_if_live,
    update_refcount,
    update_mappings
  };

  using overlay_value_t = std::variant<
    std::monostate,
    extent_ref_count_t,
    std::vector<LogicalChildNodeRef>,
    paddr_t
  >;

  template<typename T, typename Variant>
  T& expect_value(Variant& v) {
    auto ptr = std::get_if<T>(&v);
    assert(ptr && "unexpected variant type");
    return *ptr;
  }

  // Overlay consists an operation and a value
  // to be applied to certain cursor.
  struct overlay_entry {
    op_type op;
    overlay_value_t value;
  };

  // we might have 2 entries for the same laadr_t
  // we shouldn't care, overwrite and use the latest ones
  std::unordered_map<laddr_t, overlay_entry> overlay_map;

  // maintain serilized order
  // todo, is this needed
  std::queue<laddr_t> overlay_order;

  explicit LBAOverlayManager(LBAManagerRef base);
  LBAManagerRef lba_manager;
  std::unordered_map<laddr_t, LBAOverlayCursor> overlaid_cursors;

  void apply_overlay(
    LBACursorRef cursor,
    Transaction &t,
    overlay_entry entry);

public:
  static LBAOverlayManagerRef create_lba_overlay_manager(Cache &cache);

  void commit_overlay(Transaction &t) {
    // switch case
    // traverse over txn_overlaid_cursors
    //case Transaction::op_type::update_refcount:
    //lba_manager->update_mapping_refcount();
  }

  /*
  get_cursor returns LBAOverlayCursor even if not overlay exists
  */
  using get_cursor_iertr = base_iertr::extend<
    crimson::ct_error::enoent>;
  using get_cursor_ret = get_cursor_iertr::future<LBAOverlayCursor>;
  get_cursor_ret get_cursor(
    Transaction &t,
    laddr_t offset,
    bool search_containing = false);

  get_cursor_iertr::future<LBAOverlayCursor> update_mapping_refcount(
    Transaction &t,
    LBACursorRef cursor,
    int delta);

  using alloc_extent_iertr = base_iertr;

  using alloc_extents_ret = alloc_extent_iertr::future<
    std::vector<LBACursorRef>>;
  alloc_extents_ret alloc_extents(
    Transaction &t,
    LBACursorRef cursor,
    std::vector<LogicalChildNodeRef> ext);

  using alloc_extent_ret = alloc_extent_iertr::future<LBACursorRef>;
  alloc_extent_ret alloc_extent(
    Transaction &t,
    laddr_t hint,
    LogicalChildNode &nextent,
    extent_ref_count_t refcount);

  using mkfs_iertr = base_iertr;
  using mkfs_ret = mkfs_iertr::future<>;
  mkfs_ret mkfs(
    Transaction &t) {
    co_return co_await lba_manager->mkfs(t);
  }

  using get_cursors_iertr = base_iertr;
  using get_cursors_ret = get_cursors_iertr::future<std::list<LBACursorRef>>;
  get_cursors_ret get_cursors(
    Transaction &t,
    laddr_t offset, extent_len_t length){
    //tmp
    co_return co_await lba_manager->get_cursors(t, offset, length);
    }
};

}
