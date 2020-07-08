// Copyright (c) 2007-2008  INRIA (France).
// All rights reserved.
//
// This file is part of CGAL (www.cgal.org).
// You can redistribute it and/or modify it under the terms of the GNU
// General Public License as published by the Free Software Foundation,
// either version 3 of the License, or (at your option) any later version.
//
// Licensees holding a valid commercial license may use this file in
// accordance with the commercial license agreement provided with the software.
//
// This file is provided AS IS with NO WARRANTY OF ANY KIND, INCLUDING THE
// WARRANTY OF DESIGN, MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE.
//
// $URL$
// $Id$
// SPDX-License-Identifier: GPL-3.0+
//
//
// Author(s)     : Tong Zhao, Cédric Portaneri

#ifndef CGAL_OCTREE_3_H
#define CGAL_OCTREE_3_H

#include <CGAL/Octree/Node.h>
#include <CGAL/Octree/Split_criterion.h>
#include <CGAL/Octree/Walker_criterion.h>
#include <CGAL/Octree/Walker_iterator.h>

#include <CGAL/bounding_box.h>
#include <CGAL/Aff_transformation_3.h>
#include <CGAL/aff_transformation_tags.h>

#include <CGAL/Orthogonal_k_neighbor_search.h>
#include <CGAL/Search_traits_3.h>
#include <CGAL/Search_traits_adapter.h>

#include <boost/function.hpp>
#include <boost/bind.hpp>
#include <boost/iterator/iterator_facade.hpp>
#include <boost/range/iterator_range.hpp>

#include <iostream>
#include <fstream>
#include <ostream>
#include <functional>

#include <stack>
#include <queue>
#include <vector>
#include <math.h>
#include <boost/bind.hpp>

namespace CGAL {

  namespace Octree {

    /*!
     * \ingroup PkgOctreeClasses
     *
     * \brief Class Octree is a data structure for efficient computations in 3D space.
     *
     * \details It builds a heirarchy of nodes which subdivide the space based on a collection of points.
     * Each node represents an axis aligned cubic region of space.
     * A node contains the range of points that are present in the region it defines,
     * and it may contain eight other nodes which further subdivide the region.
     *
     * \tparam PointRange is a range type that provides random access iterators over the indices of a set of points.
     * \tparam PointMap is a type that maps items in the PointRange to Point data
     */
    template<class PointRange, class PointMap>
    class Octree {

    public:

      /// \name Public Types
      /// @{

      /*!
       * \brief The point type is deduced from the type of the property map used
       */
      typedef typename boost::property_traits<PointMap>::value_type Point;

      /*!
       * \brief The Kernel used is deduced from the point type
       */
      typedef typename CGAL::Kernel_traits<Point>::Kernel Kernel;

      /*!
       * \brief The floating point type is decided by the Kernel
       */
      typedef typename Kernel::FT FT;

      /*!
       * \brief
       */
      typedef boost::iterator_range<typename PointRange::iterator> Points_iterator_range;

      /*!
       * \brief The Sub-tree / Octant type
       */
      typedef Node::Node<Points_iterator_range> Node;

      /*!
       * \brief A function that determines whether a node needs to be split when refining a tree
       */
      typedef std::function<bool(const Node &)> Split_criterion;

      /*!
       * \brief A range that provides input-iterator access to the nodes of a tree
       */
      typedef boost::iterator_range<Walker_iterator<const Node>> Node_range;

      /*!
       * \brief A function that determines the next node in a traversal given the current one
       */
      typedef std::function<const Node *(const Node *)> Node_walker;

      /// @}

    private: // Private types

      typedef typename Kernel::Vector_3 Vector;
      typedef typename Kernel::Iso_cuboid_3 Iso_cuboid;
      typedef typename PointRange::iterator Range_iterator;
      typedef typename std::iterator_traits<Range_iterator>::value_type Range_type;

    private: // data members :

      Node m_root;                      /* root node of the octree */
      uint8_t m_max_depth_reached = 0;  /* octree actual highest depth reached */

      PointRange &m_ranges;              /* input point range */
      PointMap m_points_map;          /* property map: `value_type of InputIterator` -> `Point` (Position) */

      Point m_bbox_min;                  /* input bounding box min value */
      FT m_bbox_side;              /* input bounding box side length (cube) */

      std::vector<FT> m_side_per_depth;      /* side length per node's depth */
      std::vector<size_t> m_unit_per_depth; /* number of unit node (smallest) inside one node for each depth for one axis */

    public:

      /// \name Construction, Destruction
      /// @{

      /*!
       * \brief Create an octree from a collection of points
       *
       * \todo
       *
       * \param point_range
       * \param point_map
       * \param enlarge_ratio
       */
      Octree(
              PointRange &point_range,
              PointMap &point_map,
              const FT enlarge_ratio = 1.2) :
              m_ranges(point_range),
              m_points_map(point_map) {


        // compute bounding box that encloses all points
        Iso_cuboid bbox = CGAL::bounding_box(boost::make_transform_iterator
                                                     (m_ranges.begin(),
                                                      CGAL::Property_map_to_unary_function<PointMap>(
                                                              m_points_map)),
                                             boost::make_transform_iterator
                                                     (m_ranges.end(),
                                                      CGAL::Property_map_to_unary_function<PointMap>(
                                                              m_points_map)));

        // Find the center point of the box
        Point bbox_centroid = midpoint(bbox.min(), bbox.max());

        // scale bounding box to add padding
        bbox = bbox.transform(Aff_transformation_3<Kernel>(SCALING, enlarge_ratio));

        // Convert the bounding box into a cube
        FT x_len = bbox.xmax() - bbox.xmin();
        FT y_len = bbox.ymax() - bbox.ymin();
        FT z_len = bbox.zmax() - bbox.zmin();
        FT max_len = std::max({x_len, y_len, z_len});
        bbox = Iso_cuboid(bbox.min(), bbox.min() + max_len * Vector(1.0, 1.0, 1.0));

        // Shift the squared box to make sure it's centered in the original place
        Point bbox_transformed_centroid = midpoint(bbox.min(), bbox.max());
        Vector diff_centroid = bbox_centroid - bbox_transformed_centroid;
        bbox = bbox.transform(Aff_transformation_3<Kernel>(TRANSLATION, diff_centroid));

        // save octree attributes
        m_bbox_min = bbox.min();
        m_bbox_side = bbox.max()[0] - m_bbox_min[0];
        m_root.value() = {point_range.begin(), point_range.end()};
      }

      /// @}

      /// \name Tree Building
      /// @{

      /*!
       * \brief Subdivide an octree's nodes and sub-nodes until it meets the given criteria
       *
       * \todo
       *
       * \param split_criterion
       */
      void refine(const Split_criterion &split_criterion) {

        // create a side length map
        for (int i = 0; i <= (int) 32; i++)
          m_side_per_depth.push_back(m_bbox_side / (FT) (1 << i));

        // Initialize a queue of nodes that need to be refined
        std::queue<Node *> todo;
        todo.push(&m_root);

        // Process items in the queue until it's consumed fully
        while (!todo.empty()) {

          // Get the next element
          auto current = todo.front();
          todo.pop();
          int depth = current->depth();

          // Check if this node needs to be processed
          if (split_criterion(*current)) {

            // Split this node
            current->split();

            // Redistribute its points
            reassign_points((*current));

            // Process each of its children
            for (int i = 0; i < 8; ++i)
              todo.push(&(*current)[i]);

          }
        }
      }

      /*!
       * \brief Refine an octree using a max depth and max number of points in a node as split criterion
       *
       * \todo
       *
       * \param max_depth
       * \param bucket_size
       */
      void refine(size_t max_depth, size_t bucket_size) {
        refine(Split_to_max_depth_or_bucket_size(max_depth, bucket_size));
      }

      /// @}

      /// \name Accessors
      /// @{

      /*!
       * \brief Provides read and write access to the root node, and by extension the rest of the tree
       *
       * \todo
       *
       * \return
       */
      Node &root() { return m_root; }

      /*!
       * \brief Provides read-only access to the root node, and by extension the rest of the tree
       *
       * \todo
       *
       * \return
       */
      const Node &root() const { return m_root; }

      /*!
       * \brief Constructs an input range of nodes from a tree walker function
       *
       * \todo
       *
       * \param first
       * \param tree_walker
       *
       * \return
       */
      Node_range nodes(const Node *first, const Node_walker &tree_walker) const {
        return boost::make_iterator_range(Walker_iterator<const Node>(first, tree_walker),
                                          Walker_iterator<const Node>());
      }

      template<class Walker>
      Node_range walk(Walker walker = Walker()) const {

        const Node *first = walker.first(&m_root);

        Node_walker next =
                [=](const Node *n) -> const Node * {
                  return walker.next(n);
                };

//        Node_walker next = std::bind(&Walker::next, walker, std::placeholders::_1);

        return boost::make_iterator_range(Walker_iterator<const Node>(first, next),
                                          Walker_iterator<const Node>());
      }

      const Point &nearest_neighbor(const Point &point) {

        std::queue<Node *> todo;
        todo.push(&m_root);

        while (!todo.empty()) {


        }
      }

      /// @}

      /// \name Operators
      /// @{

      /*!
       * \brief Compares the topology of a pair of Octrees
       *
       * \todo
       *
       * \param rhs
       * \return
       */
      bool operator==(Octree<PointRange, PointMap> &rhs) {

        // Identical trees should have the same bounding box
        if (rhs.m_bbox_min != m_bbox_min || rhs.m_bbox_side != m_bbox_side)
          return false;

        // Identical trees should have the same depth
        if (rhs.m_max_depth_reached != m_max_depth_reached)
          return false;

        // If all else is equal, recursively compare the trees themselves
        return rhs.m_root == m_root;
      }

      /// @}


    private: // functions :

      Point compute_barycenter_position(Node &node) const {

        // Determine the side length of this node
        FT size = m_side_per_depth[node.depth()];

        // Determine the location this node should be split
        FT bary[3];
        for (int i = 0; i < 3; i++)
          bary[i] = node.location()[i] * size + (size / 2.0) + m_bbox_min[i];

        // Convert that location into a point
        return {bary[0], bary[1], bary[2]};
      }

      void reassign_points(Node &node, Range_iterator begin, Range_iterator end, const Point &center,
                           std::bitset<3> coord = {},
                           std::size_t dimension = 0) {

        // Root case: reached the last dimension
        if (dimension == 3) {

          node[coord.to_ulong()].value() = {begin, end};

          return;
        }

        // Split the point collection around the center point on this dimension
        Range_iterator split_point = std::partition(begin, end,
                                                    [&](const Range_type &a) -> bool {
                                                      return (get(m_points_map, a)[dimension] < center[dimension]);
                                                    });

        // Further subdivide the first side of the split
        std::bitset<3> coord_left = coord;
        coord_left[dimension] = false;
        reassign_points(node, begin, split_point, center, coord_left, dimension + 1);

        // Further subdivide the second side of the split
        std::bitset<3> coord_right = coord;
        coord_right[dimension] = true;
        reassign_points(node, split_point, end, center, coord_right, dimension + 1);

      }

      void reassign_points(Node &node) {

        Point center = compute_barycenter_position(node);
        reassign_points(node, node.value().begin(), node.value().end(), center);
      }

    }; // end class Octree

  } // namespace Octree

} // namespace CGAL

#endif // CGAL_OCTREE_3_H
