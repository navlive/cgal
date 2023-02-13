// Copyright (c) 2023 GeometryFactory Sarl (France).
// All rights reserved.
//
// This file is part of CGAL (www.cgal.org).
//
// $URL$
// $Id$
// SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-Commercial
//
//
// Author(s)     : Sven Oesau, Florent Lafarge, Dmitry Anisimov, Simon Giraudot

#ifndef CGAL_KSR_3_FACEPROPAGATION_H
#define CGAL_KSR_3_FACEPROPAGATION_H

// #include <CGAL/license/Kinetic_shape_reconstruction.h>

// Internal includes.
#include <CGAL/KSR/utils.h>
#include <CGAL/KSR/debug.h>
#include <CGAL/KSR/parameters.h>
#include <CGAL/KSR/debug.h>

#include <CGAL/KSR_3/Data_structure.h>

namespace CGAL {
namespace KSR_3 {

#ifdef DOXYGEN_RUNNING
#else

template<typename GeomTraits, typename IntersectionKernel>
class FacePropagation {

public:
  using Kernel = GeomTraits;
  using Intersection_kernel = IntersectionKernel;

private:
  using FT          = typename Kernel::FT;
  using Point_2     = typename Kernel::Point_2;
  using Vector_2    = typename Kernel::Vector_2;
  using Segment_2   = typename Kernel::Segment_2;
  using Direction_2 = typename Kernel::Direction_2;
  using Line_2      = typename Kernel::Line_2;

  using Data_structure = KSR_3::Data_structure<Kernel, Intersection_kernel>;

  using IVertex = typename Data_structure::IVertex;
  using IEdge   = typename Data_structure::IEdge;
  using IFace   = typename Data_structure::IFace;

  using PVertex = typename Data_structure::PVertex;
  using PEdge   = typename Data_structure::PEdge;
  using PFace   = typename Data_structure::PFace;

  using Bbox_2     = CGAL::Bbox_2;
  using Face_index = typename Data_structure::Face_index;

  using Parameters     = KSR::Parameters_3<FT>;

  using Face_event      = typename Data_structure::Support_plane::Face_event;

  struct Face_event_order {
    bool operator()(const Face_event &a, const Face_event &b) {
      return a.time > b.time;
    }
  };

public:
  FacePropagation(Data_structure& data, const Parameters& parameters) :
  m_data(data), m_parameters(parameters),
  m_min_time(-FT(1)), m_max_time(-FT(1))
  { }

  const std::pair<std::size_t, std::size_t> propagate(std::size_t k) {
    std::size_t num_queue_calls = 0;
    std::size_t num_events = 0;

    m_data.reset_to_initialization();

    for (std::size_t i = 0; i < m_data.number_of_support_planes(); ++i)
      m_data.k(i) = k;

    initialize_queue();

    while (!m_face_queue.empty()) {
      num_events = run(num_events);

      ++num_queue_calls;
    }

    return std::make_pair(num_queue_calls, num_events);
  }

  void clear() {
    m_face_queue.clear();
    m_min_time = -FT(1);
    m_max_time = -FT(1);
  }

private:
  Data_structure& m_data;
  const Parameters& m_parameters;

  FT m_min_time;
  FT m_max_time;

  std::priority_queue<typename Data_structure::Support_plane::Face_event, std::vector<Face_event>, Face_event_order> m_face_queue;

  /*******************************
  **       IDENTIFY EVENTS      **
  ********************************/

  void initialize_queue() {

    if (m_parameters.debug) {
      std::cout << "initializing queue" << std::endl;
    }

    m_data.fill_event_queue(m_face_queue);
  }

  /*******************************
  **          RUNNING           **
  ********************************/

  std::size_t run(
    const std::size_t initial_iteration) {

    if (m_parameters.debug) {
      std::cout << "* unstacking queue, current size: " << m_face_queue.size() << std::endl;
    }

    std::size_t iteration = initial_iteration;
    while (!m_face_queue.empty()) {
      // m_queue.print();

      const Face_event event = m_face_queue.top();
      m_face_queue.pop();

      ++iteration;

      apply(event);
    }
    return iteration;
  }

  /*******************************
  **        HANDLE EVENTS       **
  ********************************/

  void apply(const Face_event& event) {
    //std::cout << "support plane: " << event.support_plane << " edge: " << event.crossed_edge << " t: " << event.time << std::endl;
    if (m_data.igraph().face(event.face).part_of_partition) {
      //std::cout << " face already crossed, skipping event" << std::endl;
      return;
    }

    std::size_t line = m_data.line_idx(event.crossed_edge);
    if (!m_data.support_plane(event.support_plane).has_crossed_line(line)) {
      // Check intersection against kinetic intervals from other support planes
      int crossing = 0;
      auto kis = m_data.igraph().kinetic_intervals(event.crossed_edge);
      for (auto ki = kis.first; ki != kis.second; ki++) {
        if (ki->first == event.support_plane)
          continue;

        for (std::size_t i = 0; i < ki->second.size(); i++) {
          // Exactly on one
          if (ki->second[i].first == event.intersection_bary) {
            if (ki->second[i].second < event.time)
              crossing++;

            break;
          }

          // Within an interval
          if (ki->second[i].first > event.intersection_bary && ki->second[i - 1].first < event.intersection_bary) {
            FT interval_pos = (event.intersection_bary - ki->second[i - 1].first) / (ki->second[i].first - ki->second[i - 1].first);
            FT interval_time = interval_pos * (ki->second[i].second - ki->second[i - 1].second) + ki->second[i - 1].second;

            if (event.time > interval_time)
              crossing++;

            break;
          }
        }
      }

      // Check if the k value is sufficient for crossing the edge.
      int& k = m_data.support_plane(event.support_plane).k();
      if (k <= crossing)
        return;

      // The edge can be crossed.
      // Adjust k value
      k -= crossing;

      m_data.support_plane(event.support_plane).set_crossed_line(line);
    }

    // Associate IFace to mesh.
    PFace f = m_data.add_iface_to_mesh(event.support_plane, event.face);

    // Calculate events for new border edges.
    // Iterate inside of this face, check if each opposite edge is border and on bbox and then calculate intersection times.
    std::vector<IEdge> border;
    m_data.support_plane(event.support_plane).get_border(m_data.igraph(), f.second, border);

    for (IEdge edge : border) {
      Face_event fe;
      FT t = m_data.calculate_edge_intersection_time(event.support_plane, edge, fe);
      if (t > 0)
        m_face_queue.push(fe);
    }
  }
};

#endif //DOXYGEN_RUNNING

} // namespace KSR_3
} // namespace CGAL

#endif // CGAL_KSR_3_FACEPROPAGATION_H
