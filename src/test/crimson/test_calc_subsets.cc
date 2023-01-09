// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "gtest/gtest.h"
#include "crimson/osd/object_metadata_helper.h"

//  struct subsets_t {
//    interval_set<uint64_t> data_subset;
//    std::map<hobject_t, interval_set<uint64_t>> clone_subsets;
//  };

  
  //subsets_t calc_clone_subsets2(
  //  SnapSet& snapset, const hobject_t& soid,
  //  const pg_missing_t& missing,
  //  const hobject_t &last_backfill);

// Data

/*
hobject_t hobj_ms1{object_t{"hobj_ms1"},
			  "keykey",	// key
			  CEPH_NOSNAP,	// snap_id
			  0,		// hash
			  0,		// pool
			  ""s};		// nspace

hobject_t hobj_ms1_snp30{object_t{"hobj_ms1"},
			 "keykey",  // key
			 0x30,	    // snap_id
			 0,	    // hash
			 0,	    // pool
			 ""s};	    // nspace

*/
struct CloneSubsetData {
  // calc_clone_subsets params:
  SnapSet snap_set;
  hobject_t soid;
  pg_missing_t missing;
  hobject_t last_backfill;

  CloneSubsetData() = default;

  CloneSubsetData(SnapSet snap_set,
		  hobject_t soid,
		  pg_missing_t missing,
		  hobject_t last_backfill)
      : snap_set(snap_set)
      , soid(soid)
      , missing(missing)
      , last_backfill(last_backfill)
  {}

  SnapSet make_snapset(snapid_t seq,
		std::vector<snapid_t> snaps,
		std::vector<snapid_t> clones,
		std::map<snapid_t, interval_set<uint64_t>> clone_overlap,
		std::map<snapid_t, uint64_t> clone_size,
		std::map<snapid_t, std::vector<snapid_t>> clone_snaps)
  {
    SnapSet ss;
    ss.seq = seq;
    ss.snaps = snaps;
    ss.clones = clones;
    ss.clone_overlap = clone_overlap;
    ss.clone_size = clone_size;
    ss.clone_snaps = clone_snaps;
    return ss;
  }
};

TEST(subsets, tru)
{
  CloneSubsetData test_data;
  auto result = crimson::osd::calc_clone_subsets2(test_data.snap_set,
                                                  test_data.soid,
                                                  test_data.missing,
                                                  test_data.last_backfill);
  /*
  const auto reference_store = FakeStore{ {
    { "1:00058bcc:::rbd_data.1018ac3e755.00000000000000d5:head", {10, 234} },
    { "1:00ed7f8e:::rbd_data.1018ac3e755.00000000000000af:head", {10, 196} },
    { "1:01483aea:::rbd_data.1018ac3e755.0000000000000095:head", {10, 169} },
  }};
  auto cluster_fixture = BackfillFixtureBuilder::add_source(
    reference_store.objs
  ).add_target(
    reference_store.objs
  ).get_result();

  EXPECT_CALL(cluster_fixture, backfilled);
  */
  EXPECT_TRUE(true);
}

