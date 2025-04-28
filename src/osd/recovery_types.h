// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#pragma once

#include <map>

#include "osd_types.h"

/**
 * BackfillInterval
 *
 * Represents the objects in a range [begin, end)
 *
 * Possible states:
 * 0) Empty instance - default constructed
 * 1) Unpopulated    - BackfillInterval::begin == BackfillInterval::end
 * 2) Populated      - BackfillInterval::objects contains all existing objects
 *                     in logical range of [BackfillInterval::begin, BackfillInterval::end)
 *
 * ReplicaBackfillInterval
 *
 * Stores a map of hobject_t and eversion to track the version number of
 * the objects being backfilled in an interval for one specific shard
 *
 * PrimaryBackfillInterval
 *
 * Stores a multimap of hobject and pair<shard_id_t, eversion>.
 *
 * Only shards that are backfill targets will be tracked. For replicated and
 * non-optimized EC pools there is one entry per hobject_t and shard_id_t will
 * be NO_SHARD.
 *
 * For optimized EC pools partial writes mean it is possible that different
 * shards have different eversions, hence there may be multiple entries per
 * hobject_t. To conserve memory it is permitted to have an entry for NO_SHARD
 * and additional entries for the same hobject for specific shards. In this
 * case shards that are not specifically listed are expected to be at the
 * eversion for the NO_SHARD entry.
 *
 * Example: EC pool with 4+2 profile
 *
 *   test:head, <NO_SHARD, 1'23>
 *   test:head, <1,        1'20>
 *
 * Shards 0 and 2-5 are expected to be at version 1'23, shard 1 has skipped
 * recent updates and is expected to be at version 1'20
 */

template <typename T>
class BackfillInterval {
public:
  // info about a backfill interval on a peer
  eversion_t version; /// version at which the scan occurred
  hobject_t begin; /// object to start populating the interval from
  hobject_t end;   /// object to start populating the interval to
  bool populated = false;
  T objects;

  virtual ~BackfillInterval() = default;

  // Constructs an unpopulated instance where
  // begin==end. This is used for the
  // initalzation of peer_backfill_info.
  BackfillInterval(hobject_t begin) : begin(begin), end(begin) {}

  // Construct a fully populated instance
  BackfillInterval(hobject_t begin,
                   hobject_t end,
                   const T&& objects,
                   eversion_t version = eversion_t{}) :
  begin(begin), end(end)
  {
    ceph_assert(begin <= end);
    populate(std::move(objects), version);
  }

  BackfillInterval() = default;
  BackfillInterval(const BackfillInterval&) = default;
  BackfillInterval(BackfillInterval&&) = default;
  BackfillInterval& operator=(const BackfillInterval&) = default;
  BackfillInterval& operator=(BackfillInterval&&) = default;

  // populate the objects in the interval and update version
  void populate(const T&& _objects,
                eversion_t _version = eversion_t{}) {
    ceph_assert(objects.empty() && !populated);
    objects = std::move(_objects);
    version = _version;
    populated = true;
  }

  /// true if interval is populated
  bool is_populated() {
    return populated;
  }

  /// clear content
  //virtual void clear() = 0;

  /// true if there are no objects in this interval
  bool empty() const {
    return objects.empty();
  }

  /// true if interval extends to the end of the range
  bool extends_to_end() const {
    return end.is_max();
  }

  /// removes items <= soid and adjusts begin to the first object
  void trim_to(const hobject_t &soid) {
    trim();
    while (!objects.empty() &&
           objects.begin()->first <= soid) {
      pop_front();
    }
  }

  /// Adjusts begin to the first object
  void trim() {
    if (!objects.empty()) {
      begin = objects.begin()->first;
    } else {
      begin = end;
    }
  }

  /// clear content
  void clear() {
    version = eversion_t{};
    begin = hobject_t{};
    end = hobject_t{};
    populated = false;
    objects.clear();
  }

  /// drop first entry, and adjust @begin accordingly
  virtual void pop_front() = 0;

  /// dump
  virtual void dump(ceph::Formatter *f) const = 0;
};

class PrimaryBackfillInterval: public BackfillInterval<std::multimap<hobject_t,
					std::pair<shard_id_t, eversion_t>>> {
public:

  PrimaryBackfillInterval() : BackfillInterval() {}

  PrimaryBackfillInterval(hobject_t begin) : BackfillInterval(begin) {}

  PrimaryBackfillInterval(hobject_t begin,
                          hobject_t end,
                          const std::multimap<hobject_t, std::pair<shard_id_t, eversion_t>>&& objects,
                          eversion_t version = eversion_t{}) :
  BackfillInterval(begin, end, std::move(objects), version) {}

  /// clear content
  //void clear() override {
  //  *this = PrimaryBackfillInterval();
  //}

  /// drop first entry, and adjust @begin accordingly
  void pop_front() override {
    ceph_assert(!objects.empty());
    // Use erase(key) to erase all entries for key
    objects.erase(objects.begin()->first);
    trim();
  }

  /// dump
  void dump(ceph::Formatter *f) const override {
    f->dump_stream("begin") << begin;
    f->dump_stream("end") << end;
    f->open_array_section("objects");
    for (const auto& [hoid, shard_version] : objects) {
      const auto& [shard, version] = shard_version;
      f->open_object_section("object");
      f->dump_stream("object") << hoid;
      f->dump_stream("shard") << shard;
      f->dump_stream("version") << version;
      f->close_section();
    }
    f->close_section();
  }
};

class ReplicaBackfillInterval: public BackfillInterval<std::map<hobject_t,
								eversion_t>> {
public:

  ReplicaBackfillInterval(hobject_t begin) : BackfillInterval(begin) {}

  ReplicaBackfillInterval(hobject_t begin,
                          hobject_t end,
                          const std::map<hobject_t, eversion_t>&& objects,
                          eversion_t version = eversion_t{}) :
  BackfillInterval(begin, end, std::move(objects), version) {}

  // Construct a fully populated instance
  ReplicaBackfillInterval(hobject_t begin,
                          hobject_t end,
                          const ceph::buffer::list& data) {
    begin = begin;
    end = end;
    ceph_assert(begin <= end);
    populate_from_data(data);
  }

  /// clear content
  //void clear() override {
  //  *this = ReplicaBackfillInterval();
 // }

  // populate the objects in the interval
  void populate_from_data(const ceph::buffer::list& data) {
    ceph_assert(objects.empty() && !populated);
    auto p = data.cbegin();
    decode_noclear(objects, p);
    populated = true;
  }

  /// drop first entry, and adjust @begin accordingly
  void pop_front() {
    ceph_assert(!objects.empty());
    objects.erase(objects.begin());
    trim();
  }

  /// dump
  void dump(ceph::Formatter *f) const override {
    f->dump_stream("begin") << begin;
    f->dump_stream("end") << end;
    f->open_array_section("objects");
    for (const auto& [hoid, version] : objects) {
      f->open_object_section("object");
      f->dump_stream("object") << hoid;
      f->dump_stream("version") << version;
      f->close_section();
    }
    f->close_section();
  }
};

template<typename T> std::ostream& operator<<(std::ostream& out,
					      const BackfillInterval<T>& bi)
{
  out << "BackfillInfo(" << "populated: " << bi.populated
      << " " << bi.begin << "-" << bi.end
      << " " << bi.objects.size() << " objects";
  if (!bi.objects.empty())
    out << " " << bi.objects;
  out << ")";
  return out;
}

#if FMT_VERSION >= 90000
template <> struct fmt::formatter<PrimaryBackfillInterval> : fmt::ostream_formatter {};
template <> struct fmt::formatter<ReplicaBackfillInterval> : fmt::ostream_formatter {};
#endif
