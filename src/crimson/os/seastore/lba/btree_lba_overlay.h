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

#include "crimson/os/seastore/btree/fixed_kv_btree.h"
#include "crimson/os/seastore/seastore_types.h"
#include "crimson/os/seastore/cache.h"

#include "crimson/os/seastore/lba/lba_btree_node.h"
#include "crimson/os/seastore/btree/btree_types.h"

namespace crimson::os::seastore {
class LogicalCachedExtent;
}

namespace crimson::os::seastore::lba {
class BtreeLBAManager;

using LBABtree = FixedKVBtree<
  laddr_t, lba_map_val_t, LBAInternalNode,
  LBALeafNode, LBACursor, LBA_BLOCK_SIZE>;

  
class Overlay_LBABtree {
private:
  op_context_t opc;
  Cache& cache;

public:
  class iterator;
  using iterator_fut = base_iertr::future<iterator>;

  Overlay_LBABtree(op_context_t opc, Cache &cache)
    : opc(opc), cache (cache)
  {

  }

  using init_cached_extent_iertr = base_iertr;
  using init_cached_extent_ret = init_cached_extent_iertr::future<bool>;
  init_cached_extent_ret init_cached_extent(
    op_context_t c,
    CachedExtentRef e)
  {
    auto btree = co_await get_btree<LBABtree>(cache, opc);
    co_return btree.init_cached_extent(c, e);
  }
  std::vector<int> listings;
};
}