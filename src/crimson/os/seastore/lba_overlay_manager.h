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
#include "crimson/os/seastore/lba_types.h"
#include "crimson/os/seastore/logical_child_node.h"

#include "crimson/os/seastore/lba_manager.h"

namespace crimson::os::seastore {

/**
 * 1) apply_overlay_op          - save the deferred operation
 * 2) apply_transaction_overlay - create and return LBAOverlayCursor
 * 3) commit_overlay            - apply deferred operations

 * get_cursor returns LBAOverlayCursor
 */
class LBAOverlayManager;
using LBAOverlayManagerRef = std::unique_ptr<LBAOverlayManager>;

class LBAOverlayManager { //  : public LBAManager
//inherit later, avoid overridng all methods..

private:
  explicit LBAOverlayManager(LBAManagerRef base);
  LBAManagerRef lba_manager;

  // ulitmatly, this could be in transaction, but for now keep it here
  std::unordered_map<transaction_id_t, overlay_entry> overlaid_ops;

  //transacito shouldnty care about cursor but entries 
  void apply_transaction_overlay(
    LBAOverlayCursor overlay_cursor,
    Transaction &t);

  void apply_overlay_op(
    Transaction &t,
    overlay_entry entry);

public:
  static LBAOverlayManagerRef create_lba_overlay_manager(Cache &cache);

  void commit_overlay(Transaction &t) {
    // switch case
    // traverse over overlaid_ops
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
    std::vector<LBAOverlayCursor>>;
  alloc_extents_ret alloc_extents(
    Transaction &t,
    LBACursorRef cursor,
    std::vector<LogicalChildNodeRef> ext);

  using alloc_extent_ret = alloc_extent_iertr::future<LBAOverlayCursor>;
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
