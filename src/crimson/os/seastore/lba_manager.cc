// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:nil -*-
// vim: ts=8 sw=2 sts=2 expandtab

#include "crimson/os/seastore/lba_manager.h"
#include "crimson/os/seastore/lba/btree_lba_manager.h"

SET_SUBSYS(seastore_lba);

namespace crimson::os::seastore {

LBAManagerRef lba::create_lba_manager(Cache &cache) {
  return LBAManagerRef(new lba::BtreeLBAManager(cache));
}

LBAManager::get_cursor_ret LBAManager::get_cursor_overlay(
  Transaction &t,
  laddr_t offset,
  bool search_containing)
{
  LOG_PREFIX(LBAManager::get_cursor_overlay);
  DEBUGT("{} ... search_containing={}", t, offset, search_containing);
  // Does this offset has any overlay?
  if (t.overlay_map.contains(offset)) {
    std::optional<LBACursorRef> commited_cursor;
    commited_cursor = 
      co_await get_cursor(t, offset, search_containing).handle_error_interruptible(
        crimson::ct_error::enodata::handle([](auto) {
          return std::nullopt;
        }),
        crimson::ct_error::pass_further_all{}
        );

     LBACursorRef overlaied_cursor;
     if (commited_cursor.has_value()) {
       // We've found an entry with the given key and a corespoding cursor
       // Adjust this cursor to behave as an overlay
       overlaied_cursor = commited_cursor.value();
     } else {
       // We could not get a cursor but we do have an overlay
       // This transaction has created this entry but has not yet commited it
       // There's no dummy cursor to use
       // Create one from scrath
     }

     overlaied_cursor->set_overlay();
     // What's the overlaid op?
     Transaction::op_type overlaid_op = t.overlay_map.at(offset).op;
     switch (overlaid_op) {
        case Transaction::op_type::update_refcount:
          // update_refcount on the overlaied_cursor only
          break;
        default:
          break;
     }

     co_return overlaied_cursor;
  }

  // No overlay for this entry
  co_return co_await get_cursor(t, offset, search_containing);
}

LBAManager::ref_iertr::future<LBACursorRef> LBAManager::update_mapping_refcount_overlay(
  Transaction &t,
  LBACursorRef cursor,
  int delta) {
  LOG_PREFIX(LBAManager::update_mapping_refcount_overlay);
  DEBUGT("{} ... delta={}", t, cursor->get_laddr(), delta);
  t.overlay_map[cursor->get_laddr()] = {Transaction::op_type::update_refcount, delta};
  t.overlay_order.push(cursor->get_laddr());

  // return to the user the overlaid cursor
  // we could alteritvly not allow mutation methonds to return cursor
  // and require users to get_cursor_overlay - but this seems nicer
  co_return co_await get_cursor_overlay(t, cursor->get_laddr());
}

/* below is still wip */

LBAManager::init_cached_extent_ret LBAManager::init_cached_extent_overlay(
  Transaction &t,
  CachedExtentRef e) {
  // READER
  // <TODOs> check if this has overlaied changes..
  // if not:
  return init_cached_extent(t,e);
}

LBAManager::scan_mapped_space_ret LBAManager::scan_mapped_space_overlay(
  Transaction &t,
  scan_mapped_space_func_t &&f) {
  // TODO: this one is trickier to implenet due to the visitor param
  return scan_mapped_space(t, std::move(f));
}

LBAManager::get_cursor_ret LBAManager::get_cursor_overlay(
  Transaction &t,
  LogicalChildNode &extent)
{
  // TODO: this might be not allowed anymore. audit users.
  return get_cursor(t, extent);
}

}
