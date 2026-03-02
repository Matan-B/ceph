// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:nil -*-
// vim: ts=8 sw=2 sts=2 expandtab

#include "crimson/os/seastore/lba_manager.h"
#include "crimson/os/seastore/lba/btree_lba_manager.h"

namespace crimson::os::seastore {

LBAManagerRef lba::create_lba_manager(Cache &cache) {
  return LBAManagerRef(new lba::BtreeLBAManager(cache));
}


/*
// TODOS:
//
// overlay currently is possibly exposed to other transactions
// if this proves to be an issue we can move the overlay_map to be
// per txn
//
*/

init_cached_extent_ret LBAManager::init_cached_extent_overlay(
  Transaction &t,
  CachedExtentRef e) {
  // READER
  // <TODOs> check if this has overlaied changes..
  // if not:
  return init_cached_extent(t,e);
}

// this one is trickier to implenet due to the visitor param
scan_mapped_space_ret LBAManager::scan_mapped_space_overlay(
  Transaction &t,
  scan_mapped_space_func_t &&f) {
  return scan_mapped_space_overlay(t,std::move(f));
}

get_cursor_ret LBAManager::get_cursor_overlay(
  Transaction &t,
  LogicalChildNode &extent)
{
  // TODO: this might be not allowed anymore. audit users.
  return get_cursor(t, extent);
}

get_cursor_ret LBAManager::get_cursor_overlay(
  Transaction &t,
  laddr_t offset,
  bool search_containing)
{
  if overlay_map.contains(offset) {
    return overlay_map.at(offset);
  }
  return get_cursor(t, extent, search_containing);
}

}
