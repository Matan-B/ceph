// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:nil -*-
// vim: ts=8 sw=2 smarttab expandtab

#include "object_context_loader.h"

#include "crimson/common/exception.h"
#include "crimson/os/futurized_collection.h"
#include "crimson/osd/exceptions.h"
#include "crimson/osd/pg_backend.h"

using std::ostream;
using std::set;
using std::string;
using std::vector;

namespace {
  seastar::logger& logger() {
    return crimson::get_logger(ceph_subsys_osd);
  }
}
namespace crimson::osd {

using crimson::common::local_conf;


ObjectContextLoader::ObjectContextLoader(
  ShardServices &_shard_services,
  PGBackend& _be)
: shard_services{_shard_services},
  backend{_be}
{
}

hobject_t ObjectContextLoader::get_oid(const hobject_t& hobj)
{
  return hobj.snap == CEPH_SNAPDIR ? hobj.get_head() : hobj;
}

RWState::State ObjectContextLoader::get_lock_type(const OpInfo &op_info)
{

  if (op_info.rwordered() && op_info.may_read()) {
    return RWState::RWEXCL;
  } else if (op_info.rwordered()) {
    return RWState::RWWRITE;
  } else {
    ceph_assert(op_info.may_read());
    return RWState::RWREAD;
  }
}

template<RWState::State State>
ObjectContextLoader::load_obc_iertr::future<>
ObjectContextLoader::with_head_obc(ObjectContextRef obc, bool existed, with_obc_func_t&& func)
{
  logger().debug("{} {}", __func__, obc->get_oid());
  assert(obc->is_head());
  obc->append_to(obc_set_accessing);
  return obc->with_lock<State, IOInterruptCondition>(
    [existed=existed, obc=obc, func=std::move(func), this] {
    return get_or_load_obc<State>(obc, existed).safe_then_interruptible(
      [func = std::move(func)](auto obc) {
      return std::move(func)(std::move(obc));
    });
  }).finally([this, pgref=boost::intrusive_ptr<ObjectContextLoader>{this}, obc=std::move(obc)] {
    logger().debug("with_head_obc: released {}", obc->get_oid());
    obc->remove_from(obc_set_accessing);
  });
}

template<RWState::State State>
ObjectContextLoader::load_obc_iertr::future<>
ObjectContextLoader::with_head_obc(hobject_t oid, with_obc_func_t&& func)
{
  auto [obc, existed] =
    shard_services.get_cached_obc(std::move(oid));
  return with_head_obc<State>(std::move(obc), existed, std::move(func));
}

template<RWState::State State>
ObjectContextLoader::interruptible_future<>
ObjectContextLoader::with_existing_head_obc(ObjectContextRef obc, with_obc_func_t&& func)
{
  constexpr bool existed = true;
  return with_head_obc<State>(
    std::move(obc), existed, std::move(func)
  ).handle_error_interruptible(load_obc_ertr::assert_all{"can't happen"});
}

template<RWState::State State>
ObjectContextLoader::load_obc_iertr::future<>
ObjectContextLoader::with_clone_obc(hobject_t oid, with_obc_func_t&& func)
{
  assert(!oid.is_head());
  return with_head_obc<RWState::RWREAD>(oid.get_head(),
    [oid, func=std::move(func), this](auto head) -> load_obc_iertr::future<> {
    if (!head->obs.exists) {
      logger().error("with_clone_obc: {} head doesn't exist", head->obs.oi.soid);
      return load_obc_iertr::future<>{crimson::ct_error::object_corrupted::make()};
    }
    auto coid = PGBackend::resolve_oid(head->get_ro_ss(), oid);
    if (!coid) {
      logger().error("with_clone_obc: {} clone not found", coid);
      return load_obc_iertr::future<>{crimson::ct_error::enoent::make()};
    }
    auto [clone, existed] = shard_services.get_cached_obc(*coid);
    return clone->template with_lock<State, IOInterruptCondition>(
      [existed=existed, head=std::move(head), clone=std::move(clone),
       func=std::move(func), this]() -> load_obc_iertr::future<> {
      auto loaded = get_or_load_obc<State>(clone, existed);
      clone->head = head;
      return loaded.safe_then_interruptible([func = std::move(func)](auto clone) {
        return std::move(func)(std::move(clone));
      });
    });
  });
}

// explicitly instantiate the used instantiations
template ObjectContextLoader::load_obc_iertr::future<>
ObjectContextLoader::with_head_obc<RWState::RWNONE>(hobject_t, with_obc_func_t&&);

template<RWState::State State>
ObjectContextLoader::interruptible_future<>
ObjectContextLoader::with_existing_clone_obc(ObjectContextRef clone, with_obc_func_t&& func)
{
  assert(clone);
  assert(clone->get_head_obc());
  assert(!clone->get_oid().is_head());
  return with_existing_head_obc<RWState::RWREAD>(clone->get_head_obc(),
    [clone=std::move(clone), func=std::move(func)] ([[maybe_unused]] auto head) {
    assert(head == clone->get_head_obc());
    return clone->template with_lock<State>(
      [clone=std::move(clone), func=std::move(func)] {
      return std::move(func)(std::move(clone));
    });
  });
}

ObjectContextLoader::load_obc_iertr::future<crimson::osd::ObjectContextRef>
ObjectContextLoader::load_obc(ObjectContextRef obc)
{
  return backend.load_metadata(obc->get_oid()).safe_then_interruptible(
    [obc=std::move(obc)](auto md)
    -> load_obc_ertr::future<crimson::osd::ObjectContextRef> {
    const hobject_t& oid = md->os.oi.soid;
    logger().debug(
      "load_obc: loaded obs {} for {}", md->os.oi, oid);
    if (oid.is_head()) {
      if (!md->ssc) {
        logger().error(
          "load_obc: oid {} missing snapsetcontext", oid);
        return crimson::ct_error::object_corrupted::make();
      }
      obc->set_head_state(std::move(md->os), std::move(md->ssc));
    } else {
      obc->set_clone_state(std::move(md->os));
    }
    logger().debug(
      "load_obc: returning obc {} for {}",
      obc->obs.oi, obc->obs.oi.soid);
    return load_obc_ertr::make_ready_future<
      crimson::osd::ObjectContextRef>(obc);
  });
}

template<RWState::State State>
ObjectContextLoader::load_obc_iertr::future<crimson::osd::ObjectContextRef>
ObjectContextLoader::get_or_load_obc(
    crimson::osd::ObjectContextRef obc,
    bool existed)
{
  auto loaded = load_obc_iertr::make_ready_future<ObjectContextRef>(obc);
  if (existed) {
    logger().debug("{}: found {} in cache", __func__, obc->get_oid());
  } else {
    logger().debug("{}: cache miss on {}", __func__, obc->get_oid());
    loaded = obc->template with_promoted_lock<State, IOInterruptCondition>(
      [obc, this] {
      return load_obc(obc);
    });
  }
  return loaded;
}

ObjectContextLoader::load_obc_iertr::future<>
ObjectContextLoader::reload_obc(crimson::osd::ObjectContext& obc) const
{
  assert(obc.is_head());
  return backend.load_metadata(obc.get_oid()).safe_then_interruptible<false>([&obc](auto md)
    -> load_obc_ertr::future<> {
    logger().debug(
      "{}: reloaded obs {} for {}",
      __func__,
      md->os.oi,
      obc.get_oid());
    if (!md->ssc) {
      logger().error(
        "{}: oid {} missing snapsetcontext",
        __func__,
        obc.get_oid());
      return crimson::ct_error::object_corrupted::make();
    }
    obc.set_head_state(std::move(md->os), std::move(md->ssc));
    return load_obc_ertr::now();
  });
}

ObjectContextLoader::load_obc_iertr::future<>
ObjectContextLoader::with_locked_obc(const hobject_t &hobj,
                    const OpInfo &op_info,
                    with_obc_func_t &&f)
{
  if (__builtin_expect(stopping, false)) {
    throw crimson::common::system_shutdown_exception();
  }
  const hobject_t oid = get_oid(hobj);
  switch (get_lock_type(op_info)) {
  case RWState::RWREAD:
    if (oid.is_head()) {
      return with_head_obc<RWState::RWREAD>(oid, std::move(f));
    } else {
      return with_clone_obc<RWState::RWREAD>(oid, std::move(f));
    }
  case RWState::RWWRITE:
    if (oid.is_head()) {
      return with_head_obc<RWState::RWWRITE>(oid, std::move(f));
    } else {
      return with_clone_obc<RWState::RWWRITE>(oid, std::move(f));
    }
  case RWState::RWEXCL:
    if (oid.is_head()) {
      return with_head_obc<RWState::RWEXCL>(oid, std::move(f));
    } else {
      return with_clone_obc<RWState::RWEXCL>(oid, std::move(f));
    }
  default:
    ceph_abort();
  };
}

template <RWState::State State>
ObjectContextLoader::interruptible_future<>
ObjectContextLoader::with_locked_obc(ObjectContextRef obc, with_obc_func_t &&f)
{
  // TODO: a question from rebase: do we really need such checks when
  // the interruptible stuff is being used?
  if (__builtin_expect(stopping, false)) {
    throw crimson::common::system_shutdown_exception();
  }
  if (obc->is_head()) {
    return with_existing_head_obc<State>(obc, std::move(f));
  } else {
    return with_existing_clone_obc<State>(obc, std::move(f));
  }
}

// explicitly instantiate the used instantiations
template ObjectContextLoader::interruptible_future<>
ObjectContextLoader::with_locked_obc<RWState::RWEXCL>(ObjectContextRef, with_obc_func_t&&);
}
