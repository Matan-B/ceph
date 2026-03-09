// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:nil -*-
// vim: ts=8 sw=2 sts=2 expandtab

#include "crimson/os/seastore/lba_overlay_manager.h"
#include "crimson/os/seastore/lba/btree_lba_manager.h"

SET_SUBSYS(seastore_lba);

namespace crimson::os::seastore {

LBAOverlayManager::LBAOverlayManager(LBAManagerRef lba_manager)
    : lba_manager(std::move(lba_manager)) {}

LBAOverlayManagerRef LBAOverlayManager::create_lba_overlay_manager(Cache &cache) {
    auto lba_manager = LBAManagerRef(new lba::BtreeLBAManager(cache));
    return LBAOverlayManagerRef(new LBAOverlayManager(std::move(lba_manager)));
}

void LBAOverlayManager::apply_overlay(
  LBACursorRef cursor,
  Transaction &t,
  overlay_entry entry) {
  // maintain overlay data and order to be use at commit time
  overlay_map[cursor->get_laddr()] = entry;
  overlay_order.push(cursor->get_laddr());
  // Let the transaction know
  t.txn_overlaid_cursors.insert(cursor->get_laddr());
  // User exposed cursor
  LBAOverlayCursor overlaid_cursor{cursor};
  // What's the overlaid op?
  switch (entry.op) {
      case op_type::update_refcount: {
        auto overlaid_refcount = expect_value<extent_ref_count_t>(entry.value);
        overlaid_cursor.set_overlay(&LBAOverlayCursor::overlaid_refcount, overlaid_refcount);
        break;
      }
      case op_type::alloc_extents: {
        auto alloc_extents = expect_value<std::vector<LogicalChildNodeRef>>(entry.value);
        overlaid_cursor.set_overlay(&LBAOverlayCursor::alloc_extents, alloc_extents);
        break;
      }
      default:
        break;
  }
  overlaid_cursors[cursor->get_laddr()] = overlaid_cursor;
}

LBAOverlayManager::get_cursor_ret LBAOverlayManager::get_cursor(
  Transaction &t,
  laddr_t offset,
  bool search_containing) {

  LOG_PREFIX(LBAOverlayManager::get_cursor);
  DEBUGT("{} ... search_containing={}", t, offset, search_containing);
  if (t.txn_overlaid_cursors.contains(offset)) {
    assert(overlaid_cursors.contains(offset));
    co_return overlaid_cursors.at(offset);
  }
  auto commited_cursor = co_await lba_manager->get_cursor(t, offset, search_containing);
  co_return LBAOverlayCursor{commited_cursor};

}

LBAOverlayManager::get_cursor_iertr::future<LBAOverlayCursor> LBAOverlayManager::update_mapping_refcount(
  Transaction &t,
  LBACursorRef cursor,
  int delta) {
  LOG_PREFIX(LBAOverlayManager::update_mapping_refcount);
  DEBUGT("{} ... delta={}", t, cursor->get_laddr(), delta);
  auto overlaid_refcount = cursor->get_refcount();
  ceph_assert((int)overlaid_refcount + delta >= 0);
  overlaid_refcount += delta;
  apply_overlay(
    cursor,
    t,
    overlay_entry{op_type::update_refcount,
                  overlaid_refcount});
  co_return overlaid_cursors[cursor->get_laddr()];
}

LBAOverlayManager::alloc_extents_ret LBAOverlayManager::alloc_extents(
  Transaction &t,
  LBACursorRef cursor,
  std::vector<LogicalChildNodeRef> ext) {
  LOG_PREFIX(LBAOverlayManager::alloc_extents);
  DEBUGT("{} ...", t, cursor->get_laddr());
  apply_overlay(
    cursor,
    t,
    overlay_entry{op_type::alloc_extents, ext});
  std::vector<LBAOverlayCursor> tmp;
  // create the tmp vector based on the Overlaied cursors
  co_return tmp;
}

LBAOverlayManager::alloc_extent_ret LBAOverlayManager::alloc_extent(
  Transaction &t,
  laddr_t hint,
  LogicalChildNode &nextent,
  extent_ref_count_t refcount) {

  // The idea would be here to change LBAManager alloc_extent into two seperate pieces
  // 1) search read position
  // 2) mutate: insert mappings to tree (overlaid)
  co_return co_await lba_manager->alloc_extent(t, hint, nextent, refcount);
}

}
