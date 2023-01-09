#pragma once

#include "osd/osd_types.h"

namespace crimson::osd {
  struct subsets_t {
    interval_set<uint64_t> data_subset;
    std::map<hobject_t, interval_set<uint64_t>> clone_subsets;
  };

  subsets_t calc_clone_subsets2(
    SnapSet& snapset, const hobject_t& soid,
    const pg_missing_t& missing,
    const hobject_t &last_backfill);
  subsets_t calc_head_subsets2(
    uint64_t size,
    SnapSet& snapset,
    const hobject_t& head,
    const pg_missing_t& missing,
    const hobject_t &last_backfill);
  void set_subsets(
    const subsets_t& subsets,
    ObjectRecoveryInfo& recovery_info);
}
