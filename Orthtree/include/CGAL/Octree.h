// Copyright (c) 2020  GeometryFactory (France).
// All rights reserved.
//
// This file is part of CGAL (www.cgal.org).
//
// $URL$
// $Id$
// SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-Commercial
//
// Author(s)     : Simon Giraudot

#ifndef CGAL_OCTREE_H
#define CGAL_OCTREE_H

#include <CGAL/license/Orthtree.h>

#include <CGAL/Orthtree.h>
#include <CGAL/Orthtree_traits_point.h>

namespace CGAL {

/*!
  \ingroup PkgOrthtreeRef

  \brief Alias that specializes the `Orthtree` class to a 3D octree.

  These two types are exactly equivalent:
  - `Octree<GeomTraits, PointRange, PointMap>`
  - `Orthtree<Orthtree_traits_point<GeomTraits, PointRange, PointMap, Dimension_tag<3>>>`.

  \tparam GeomTraits must be a model of `Kernel`
  \tparam PointRange must be a model of `Range` whose value type is the key type of `PointMap`
  \tparam PointMap must be a model of `ReadablePropertyMap` whose value type is `GeomTraits::Point_3`
 */
template <
  typename GeomTraits,
  typename PointRange,
  typename PointMap = Identity_property_map<typename std::iterator_traits<typename PointRange::iterator>::value_type>
>
using Octree = Orthtree<Orthtree_traits_point<GeomTraits, PointRange, PointMap, Dimension_tag<3>>>;

} // namespace CGAL


#endif // CGAL_OCTREE_H
