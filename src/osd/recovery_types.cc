// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "recovery_types.h"

template <typename T>
BackfillInterval<T>::BackfillInterval(hobject_t _begin) :
  begin(_begin), end(_begin) {}

template <typename T>
BackfillInterval<T>::BackfillInterval(hobject_t _begin,
                                   hobject_t _end) :
  begin(_begin),
  end(_end) {}

template <typename T>
BackfillInterval<T>::BackfillInterval(hobject_t _begin,
                                   hobject_t _end,
                                   const T&& _objects,
                                   eversion_t _version) :
  begin(_begin),
  end(_end)
{
  ceph_assert(_begin <= _end);
  populate(std::move(_objects), _version);
}

BackfillInterval<T>::BackfillInterval(hobject_t _begin,
                                   hobject_t _end,
                                   const ceph::buffer::list& data) :
  begin(_begin),
  end(_end)
{
  ceph_assert(_begin <= _end);
  populate(data);
}

template<typename T> std::ostream& operator<<(std::ostream& out,
                                              const BackfillInterval<T>& bi)
{
  out << "BackfillInfo(" << "populated: " << bi.populated 
      << bi.begin << "-" << bi.end << " ";
  if (!bi.objects.empty()) {
    out << bi.objects.size() << " objects " << bi.objects;
  }
  out << ")";
  return out;
}
