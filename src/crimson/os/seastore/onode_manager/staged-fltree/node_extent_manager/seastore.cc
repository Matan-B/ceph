// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:nil -*-
// vim: ts=8 sw=2 sts=2 expandtab

#include "seastore.h"

#include "crimson/os/seastore/onode_manager/staged-fltree/node_extent_accessor.h"
#include "crimson/os/seastore/onode_manager/staged-fltree/stages/node_stage_layout.h"

namespace {
LOG_PREFIX(OTree::Seastore);
}

SET_SUBSYS(seastore_onode);

namespace crimson::os::seastore::onode {

static DeltaRecorderURef create_replay_recorder(
    node_type_t node_type, field_type_t field_type)
{
  if (node_type == node_type_t::LEAF) {
    if (field_type == field_type_t::N0) {
      return DeltaRecorderT<node_fields_0_t, node_type_t::LEAF>::create_for_replay();
    } else if (field_type == field_type_t::N1) {
      return DeltaRecorderT<node_fields_1_t, node_type_t::LEAF>::create_for_replay();
    } else if (field_type == field_type_t::N2) {
      return DeltaRecorderT<node_fields_2_t, node_type_t::LEAF>::create_for_replay();
    } else if (field_type == field_type_t::N3) {
      return DeltaRecorderT<leaf_fields_3_t, node_type_t::LEAF>::create_for_replay();
    } else {
      ceph_abort_msg("impossible path");
    }
  } else if (node_type == node_type_t::INTERNAL) {
    if (field_type == field_type_t::N0) {
      return DeltaRecorderT<node_fields_0_t, node_type_t::INTERNAL>::create_for_replay();
    } else if (field_type == field_type_t::N1) {
      return DeltaRecorderT<node_fields_1_t, node_type_t::INTERNAL>::create_for_replay();
    } else if (field_type == field_type_t::N2) {
      return DeltaRecorderT<node_fields_2_t, node_type_t::INTERNAL>::create_for_replay();
    } else if (field_type == field_type_t::N3) {
      return DeltaRecorderT<internal_fields_3_t, node_type_t::INTERNAL>::create_for_replay();
    } else {
      ceph_abort_msg("impossible path");
    }
  } else {
    ceph_abort_msg("impossible path");
  }
}

NodeExtentRef SeastoreNodeExtent::mutate(
    context_t c, DeltaRecorderURef&& _recorder, mutate_mode_t mode)
{
  DEBUGT("mutate {} mode={} ...", c.t, *this, static_cast<int>(mode));
  auto p_handle = static_cast<TransactionManagerHandle*>(&c.nm);
  auto extent = p_handle->tm.get_mutable_extent(c.t, this, mode);
  auto ret = extent->cast<SeastoreNodeExtent>();
  const bool has_recorded = (recorder && !recorder->is_empty());
  if (ret.get() == this) {
    // mutated in place, see CachedExtent::inplace_writer
    assert(is_inplace_pending());
    if (!has_recorded) {
      // first in-place mutation of this extent by the transaction
      recorder = std::move(_recorder);
    } // else: mutated in place earlier, keep recording into the same recorder
    return ret;
  }
  assert(!ret->recorder || ret->recorder->is_empty());
  if (has_recorded) {
    // This extent was mutated in place earlier in the transaction and is now
    // copied for a layout change: the copy already holds the in-place bytes,
    // move the deltas recorded so far along so the delta of the copy covers
    // both, relative to the committed version.
    ret->recorder = std::move(recorder);
  } else {
    // A replayed extent may already have an empty recorder, we discard it
    // for simplicity.
    ret->recorder = std::move(_recorder);
  }
  return ret;
}

void SeastoreNodeExtent::apply_delta(const ceph::bufferlist& bl)
{
  DEBUG("replay {} ...", *this);
  if (!recorder) {
    auto header = get_header();
    auto field_type = header.get_field_type();
    if (!field_type.has_value()) {
      ERROR("replay got invalid node -- {}", *this);
      ceph_abort_msg("fatal error");
    }
    auto node_type = header.get_node_type();
    recorder = create_replay_recorder(node_type, *field_type);
  } else {
#ifndef NDEBUG
    auto header = get_header();
    assert(recorder->node_type() == header.get_node_type());
    assert(recorder->field_type() == *header.get_field_type());
#endif
  }
  auto mut = do_get_mutable();
  auto p = bl.cbegin();
  while (p != bl.end()) {
    recorder->apply_delta(p, mut, *this);
  }
  DEBUG("relay done!");
}

}
