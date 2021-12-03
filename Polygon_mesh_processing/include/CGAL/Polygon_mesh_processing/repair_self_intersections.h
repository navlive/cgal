// Copyright (c) 2015-2020 GeometryFactory (France).
// All rights reserved.
//
// This file is part of CGAL (www.cgal.org).
//
// $URL$
// $Id$
// SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-Commercial
//
// Author(s)     : Sebastien Loriot,
//                 Mael Rouxel-Labbé
//
#ifndef CGAL_POLYGON_MESH_PROCESSING_REPAIR_SELF_INTERSECTIONS_H
#define CGAL_POLYGON_MESH_PROCESSING_REPAIR_SELF_INTERSECTIONS_H

#include <CGAL/license/Polygon_mesh_processing/repair.h>

#include <CGAL/Polygon_mesh_processing/border.h>
#include <CGAL/Polygon_mesh_processing/connected_components.h>
#include <CGAL/Polygon_mesh_processing/manifoldness.h>
#include <CGAL/Polygon_mesh_processing/orient_polygon_soup.h>
#include <CGAL/Polygon_mesh_processing/polygon_soup_to_polygon_mesh.h>
#include <CGAL/Polygon_mesh_processing/remesh.h>
#include <CGAL/Polygon_mesh_processing/self_intersections.h>
#include <CGAL/Polygon_mesh_processing/smooth_mesh.h>
#include <CGAL/Polygon_mesh_processing/triangulate_hole.h>
#ifndef CGAL_PMP_REMOVE_SELF_INTERSECTION_NO_POLYHEDRAL_ENVELOPE_CHECK
#include <CGAL/Polyhedral_envelope.h>
#endif

#include <CGAL/AABB_tree.h>
#include <CGAL/AABB_traits.h>
#include <CGAL/AABB_face_graph_triangle_primitive.h>
#include <CGAL/assertions.h>
#include <CGAL/boost/graph/copy_face_graph.h>
#include <CGAL/boost/graph/Face_filtered_graph.h>
#include <CGAL/boost/graph/Named_function_parameters.h>
#include <CGAL/boost/graph/named_params_helper.h>
#include <CGAL/boost/graph/selection.h>
#include <CGAL/box_intersection_d.h>
#include <CGAL/utility.h>

#include <array>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <set>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

// #define CGAL_PMP_REMOVE_SELF_INTERSECTIONS_NO_SMOOTHING
// #define CGAL_PMP_REMOVE_SELF_INTERSECTIONS_NO_CONSTRAINTS_IN_HOLE_FILLING
// #define CGAL_PMP_REMOVE_SELF_INTERSECTION_NO_POLYHEDRAL_ENVELOPE_CHECK

// Self-intersection removal is done by making a big-enough hole and filling it
//
// Local self-intersection removal is more subtle and only considers self-intersections
// within a connected component. It then tries to fix those by trying successively:
// - smoothing with the sharp edges in the area being constrained
// - smoothing without the sharp edges in the area being constrained
// - hole-filling with the sharp edges in the area being constrained
// - hole-filling without the sharp edges in the area being constrained
//
// The working area grows as long as we haven't been able to fix the self-intersection,
// up to a user-defined number of times.

namespace CGAL {
namespace Polygon_mesh_processing {
namespace internal {

#ifdef CGAL_PMP_REMOVE_SELF_INTERSECTION_DEBUG
static int unsolved_self_intersections = 0;
static int self_intersections_solved_by_constrained_smoothing = 0;
static int self_intersections_solved_by_unconstrained_smoothing = 0;
static int self_intersections_solved_by_constrained_hole_filling = 0;
static int self_intersections_solved_by_unconstrained_hole_filling = 0;
#endif

// -*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-

// @todo these could be extracted to somewhere else, it's useful in itself
template <typename PolygonMesh, typename VPM, typename Point, typename FaceOutputIterator>
FaceOutputIterator replace_faces_with_patch(const std::vector<typename boost::graph_traits<PolygonMesh>::vertex_descriptor>& border_vertices,
                                            const std::set<typename boost::graph_traits<PolygonMesh>::vertex_descriptor>& interior_vertices,
                                            const std::vector<typename boost::graph_traits<PolygonMesh>::halfedge_descriptor>& border_hedges,
                                            const std::set<typename boost::graph_traits<PolygonMesh>::edge_descriptor>& interior_edges,
                                            const std::set<typename boost::graph_traits<PolygonMesh>::face_descriptor>& faces,
                                            const std::vector<std::vector<Point> >& patch,
                                            PolygonMesh& pmesh,
                                            VPM& vpm,
                                            FaceOutputIterator out)
{
  CGAL_static_assertion((std::is_same<typename boost::property_traits<VPM>::value_type, Point>::value));

  typedef typename boost::graph_traits<PolygonMesh>::vertex_descriptor      vertex_descriptor;
  typedef typename boost::graph_traits<PolygonMesh>::halfedge_descriptor    halfedge_descriptor;
  typedef typename boost::graph_traits<PolygonMesh>::edge_descriptor        edge_descriptor;
  typedef typename boost::graph_traits<PolygonMesh>::face_descriptor        face_descriptor;

  typedef std::vector<Point>                                                Point_face;
  typedef std::vector<vertex_descriptor>                                    Vertex_face;

  CGAL_precondition(is_valid_polygon_mesh(pmesh));

  // To be used to create new elements
  std::vector<vertex_descriptor> vertex_stack(interior_vertices.begin(), interior_vertices.end());
  std::vector<edge_descriptor> edge_stack(interior_edges.begin(), interior_edges.end());
  std::vector<face_descriptor> face_stack(faces.begin(), faces.end());

  // Introduce new vertices, convert the patch in vertex patches
  std::vector<Vertex_face> patch_with_vertices;
  patch_with_vertices.reserve(patch.size());

  std::map<Point, vertex_descriptor> point_to_vs;

  // first, add those for which the vertex will not change
  for(const vertex_descriptor v : border_vertices)
    point_to_vs[get(vpm, v)] = v;

  // now build a correspondence map and the faces with vertices
  const vertex_descriptor null_v = boost::graph_traits<PolygonMesh>::null_vertex();
  for(const Point_face& face : patch)
  {
    Vertex_face vface;
    vface.reserve(face.size());

    for(const Point& p : face)
    {
      bool success;
      typename std::map<Point, vertex_descriptor>::iterator it;
      std::tie(it, success) = point_to_vs.insert(std::make_pair(p, null_v));
      vertex_descriptor& v = it->second;

      if(success) // first time we meet that point, means it`s an interior point and we need to make a new vertex
      {
        if(vertex_stack.empty())
        {
          v = add_vertex(pmesh);
        }
        else
        {
          v = vertex_stack.back();
          vertex_stack.pop_back();
        }

        put(vpm, v, p);
      }

      vface.push_back(v);
    }

    patch_with_vertices.push_back(vface);
  }

  typedef std::pair<vertex_descriptor, vertex_descriptor>                        Vertex_pair;
  typedef std::map<Vertex_pair, halfedge_descriptor>                             Vertex_pair_halfedge_map;

  Vertex_pair_halfedge_map halfedge_map;

  // register border halfedges
  int i = 0;
  for(halfedge_descriptor h : border_hedges)
  {
    const vertex_descriptor vs = source(h, pmesh);
    const vertex_descriptor vt = target(h, pmesh);
    halfedge_map.insert(std::make_pair(std::make_pair(vs, vt), h));

    set_halfedge(target(h, pmesh), h, pmesh); // update vertex halfedge pointer
    ++i;
  }

  face_descriptor f = boost::graph_traits<PolygonMesh>::null_face();
#ifdef CGAL_PMP_REMOVE_SELF_INTERSECTION_DEBUG
  std::vector<face_descriptor> new_faces;
#endif

  for(const Vertex_face& vface : patch_with_vertices)
  {
    if(face_stack.empty())
    {
      f = add_face(pmesh);
    }
    else
    {
      f = face_stack.back();
      face_stack.pop_back();
    }

    *out++ = f;
#ifdef CGAL_PMP_REMOVE_SELF_INTERSECTION_DEBUG
    new_faces.push_back(f);
#endif

    std::vector<halfedge_descriptor> hedges;
    hedges.reserve(vface.size());

    for(std::size_t i=0, n=vface.size(); i<n; ++i)
    {
      const vertex_descriptor vi = vface[i];
      const vertex_descriptor vj = vface[(i+1)%n];

      // get the corresponding halfedge (either a new one or an already created)
      bool success;
      typename Vertex_pair_halfedge_map::iterator it;
      std::tie(it, success) = halfedge_map.insert(std::make_pair(std::make_pair(vi, vj),
                                                  boost::graph_traits<PolygonMesh>::null_halfedge()));
      halfedge_descriptor& h = it->second;

      if(success) // this halfedge is an interior halfedge
      {
        if(edge_stack.empty())
        {
          h = halfedge(add_edge(pmesh), pmesh);
        }
        else
        {
          h = halfedge(edge_stack.back(), pmesh);
          edge_stack.pop_back();
        }

        halfedge_map[std::make_pair(vj, vi)] = opposite(h, pmesh);
      }

      hedges.push_back(h);
    }

    CGAL_assertion(vface.size() == hedges.size());

    // update halfedge connections + face pointers
    for(std::size_t i=0, n=vface.size(); i<n; ++i)
    {
      set_next(hedges[i], hedges[(i+1)%n], pmesh);
      set_face(hedges[i], f, pmesh);

      set_target(hedges[i], vface[(i+1)%n], pmesh);
      set_halfedge(vface[(i+1)%n], hedges[i], pmesh);
    }

    set_halfedge(f, hedges[0], pmesh);
  }

  // now remove the remaining superfluous vertices, edges, faces
  for(vertex_descriptor v : vertex_stack)
    remove_vertex(v, pmesh);
  for(edge_descriptor e : edge_stack)
    remove_edge(e, pmesh);
  for(face_descriptor f : face_stack)
    remove_face(f, pmesh);

#ifdef CGAL_PMP_REMOVE_SELF_INTERSECTION_OUTPUT
  CGAL::IO::write_polygon_mesh("results/last_patch_replacement.off", pmesh, CGAL::parameters::stream_precision(17));
#endif

#ifdef CGAL_PMP_REMOVE_SELF_INTERSECTION_DEBUG
  std::cout << "  DEBUG: Replacing range with patch: ";
  std::cout << faces.size() << " triangles removed, " << patch.size() << " created\n";
#endif

  CGAL_postcondition(is_valid_polygon_mesh(pmesh, true));

#ifdef CGAL_PMP_REMOVE_SELF_INTERSECTION_DEBUG
  CGAL_postcondition(!does_self_intersect(new_faces, pmesh));
#endif

  return out;
}

template <typename PolygonMesh, typename VPM, typename Point, typename FaceOutputIterator>
FaceOutputIterator replace_faces_with_patch(const std::set<typename boost::graph_traits<PolygonMesh>::face_descriptor>& face_range,
                                            const std::vector<std::vector<Point> >& patch,
                                            PolygonMesh& pmesh,
                                            VPM& vpm,
                                            FaceOutputIterator out)
{
  typedef typename boost::graph_traits<PolygonMesh>::vertex_descriptor     vertex_descriptor;
  typedef typename boost::graph_traits<PolygonMesh>::halfedge_descriptor   halfedge_descriptor;
  typedef typename boost::graph_traits<PolygonMesh>::edge_descriptor       edge_descriptor;
  typedef typename boost::graph_traits<PolygonMesh>::face_descriptor       face_descriptor;

  std::vector<vertex_descriptor> border_vertices;
  std::set<vertex_descriptor> interior_vertices;
  std::vector<halfedge_descriptor> border_hedges;
  std::set<edge_descriptor> interior_edges;

  for(face_descriptor fh : face_range)
  {
    for(halfedge_descriptor h : halfedges_around_face(halfedge(fh, pmesh), pmesh))
    {
      if(halfedge(target(h, pmesh), pmesh) == h) // limit the number of insertions
        interior_vertices.insert(target(h, pmesh));
    }
  }

  for(face_descriptor fh : face_range)
  {
    for(halfedge_descriptor h : halfedges_around_face(halfedge(fh, pmesh), pmesh))
    {
      CGAL_assertion(!is_border(h, pmesh));

      const edge_descriptor e = edge(h, pmesh);
      const halfedge_descriptor opp_h = opposite(h, pmesh);
      const face_descriptor opp_f = face(opp_h, pmesh);

      if(is_border(opp_h, pmesh) || face_range.count(opp_f) == 0)
      {
        vertex_descriptor v = target(h, pmesh);
        interior_vertices.erase(v);
        border_hedges.push_back(h);
        border_vertices.push_back(v);
      }
      else
      {
        interior_edges.insert(e);
      }
    }
  }

  return replace_faces_with_patch(border_vertices, interior_vertices,
                                  border_hedges, interior_edges, face_range, patch,
                                  pmesh, vpm, out);
}

template <typename PolygonMesh, typename VPM, typename Point>
void replace_faces_with_patch(const std::set<typename boost::graph_traits<PolygonMesh>::face_descriptor>& faces,
                              const std::vector<std::vector<Point> >& patch,
                              PolygonMesh& pmesh,
                              VPM& vpm)
{
  CGAL::Emptyset_iterator out;
  replace_faces_with_patch(faces, patch, pmesh, vpm, out);
}

// -*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-

template <typename Point, typename FaceRange, typename PolygonMesh, typename VertexPointMap>
void back_up_face_range_as_point_patch(std::vector<std::vector<Point> >& point_patch,
                                       const FaceRange& face_range,
                                       const PolygonMesh& tmesh,
                                       const VertexPointMap vpm)
{
  typedef typename boost::graph_traits<PolygonMesh>::halfedge_descriptor    halfedge_descriptor;
  typedef typename boost::graph_traits<PolygonMesh>::face_descriptor        face_descriptor;

  point_patch.reserve(face_range.size());

  for(const face_descriptor f : face_range)
  {
    std::vector<Point> face_points;
    for(const halfedge_descriptor h : CGAL::halfedges_around_face(halfedge(f, tmesh), tmesh))
      face_points.push_back(get(vpm, target(h, tmesh)));

    point_patch.push_back(face_points);
  }
}

// -*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-

template <typename FaceRange, typename EdgeConstrainMap,
          typename TriangleMesh, typename VertexPointMap, typename GeomTraits>
void constrain_edges(const FaceRange& faces,
                     TriangleMesh& tmesh,
                     const bool constrain_border_edges,
                     const bool constrain_sharp_edges,
                     const double dihedral_angle,
                     const double /*weak_DA*/,
                     EdgeConstrainMap& eif,
                     VertexPointMap vpm,
                     const GeomTraits& gt)
{
  typedef typename boost::graph_traits<TriangleMesh>::halfedge_descriptor   halfedge_descriptor;
  typedef typename boost::graph_traits<TriangleMesh>::edge_descriptor       edge_descriptor;
  typedef typename boost::graph_traits<TriangleMesh>::face_descriptor       face_descriptor;

  typedef typename GeomTraits::FT                                           FT;
  typedef typename GeomTraits::Vector_3                                     Vector;

  std::unordered_map<edge_descriptor, bool> is_border_of_selection;
  for(face_descriptor f : faces)
  {
    for(halfedge_descriptor h : CGAL::halfedges_around_face(halfedge(f, tmesh), tmesh))
    {
      // Default initialization is guaranteed to be `false`. Thus, meet it once will switch
      // the value to `true` and meeting it twice will switch back to `false`.
      const edge_descriptor e = edge(h, tmesh);
      if(constrain_sharp_edges)
        is_border_of_selection[e] = !(is_border_of_selection[e]);
      else
        is_border_of_selection[e] = false;
    }
  }

#if 0 // Until detect_features++ is integrated
  CGAL::Polygon_mesh_processing::experimental::detect_sharp_edges_pp(faces, tmesh, dihedral_angle, eif,
                                                                     parameters::weak_dihedral_angle(weak_DA));

  // ...
#else
  // this is basically the code that is in detect_features (at the very bottom)
  // but we do not want a folding to be marked as a sharp feature so the dihedral angle is also
  // bounded from above
  const double bound = dihedral_angle;
  const double cos_angle = std::cos(bound * CGAL_PI / 180.);

  for(const auto& ep : is_border_of_selection)
  {
    bool flag = ep.second;
    if(!constrain_border_edges)
      flag = false;

    if(constrain_sharp_edges && !flag)
    {
      const halfedge_descriptor h = halfedge(ep.first, tmesh);
      CGAL_assertion(!is_border(edge(h, tmesh), tmesh));

      const face_descriptor f1 = face(h, tmesh);
      const face_descriptor f2 = face(opposite(h, tmesh), tmesh);

      // @speed cache normals
      const Vector n1 = compute_face_normal(f1, tmesh, parameters::vertex_point_map(vpm).geom_traits(gt));
      const Vector n2 = compute_face_normal(f2, tmesh, parameters::vertex_point_map(vpm).geom_traits(gt));
      const FT c = gt.compute_scalar_product_3_object()(n1, n2);

      // Do not mark as sharp edges with a dihedral angle that is almost `pi` because this is likely
      // due to a foldness on the mesh rather than a sharp edge that we wish to preserve
      // (Ideally this would be pre-treated as part of the flatness treatment)
      flag = (c <= cos_angle && c >= -cos_angle);
    }

    is_border_of_selection[ep.first] = flag; // Only needed for output, really
    put(eif, ep.first, flag);
  }
#endif

#ifdef CGAL_PMP_REMOVE_SELF_INTERSECTION_OUTPUT
  std::ofstream out("results/constrained_edges.polylines.txt");
  out << std::setprecision(17);
  for(edge_descriptor e : edges(tmesh))
    if(get(eif, e))
       out << "2 " << tmesh.point(source(e, tmesh)) << " " << tmesh.point(target(e, tmesh)) << std::endl;
  out.close();
#endif
}

// -*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-

template <typename TriangleMesh, typename VertexPointMap, typename GeomTraits, typename PolyhedralEnvelope>
bool remove_self_intersections_with_smoothing(std::set<typename boost::graph_traits<TriangleMesh>::face_descriptor>& face_range,
                                              TriangleMesh& tmesh,
                                              const bool constrain_sharp_edges,
                                              const double dihedral_angle,
                                              const double weak_DA,
                                              const PolyhedralEnvelope& cc_envelope,
                                              VertexPointMap vpm,
                                              const GeomTraits& gt)
{
  namespace CP = CGAL::parameters;

  typedef typename boost::graph_traits<TriangleMesh>::halfedge_descriptor   halfedge_descriptor;
  typedef typename boost::graph_traits<TriangleMesh>::face_descriptor       face_descriptor;

  typedef typename boost::property_traits<VertexPointMap>::value_type       Point;

#ifdef CGAL_PMP_REMOVE_SELF_INTERSECTION_DEBUG
  std::cout << "  DEBUG: repair with smoothing... (constraining sharp edges: ";
  std::cout << std::boolalpha << constrain_sharp_edges << ")" << std::endl;
#endif

  CGAL_precondition(does_self_intersect(face_range, tmesh));

  // Rather than working directly on the mesh, copy a range and work on this instead
  const CGAL::Face_filtered_graph<TriangleMesh> ffg(tmesh, face_range);
  TriangleMesh local_mesh;
  CGAL::copy_face_graph(ffg, local_mesh, CP::vertex_point_map(vpm));

#ifdef CGAL_PMP_REMOVE_SELF_INTERSECTION_OUTPUT
  CGAL::IO::write_polygon_mesh("results/local_mesh.off", local_mesh, CGAL::parameters::stream_precision(17));
#endif

  // Constrain sharp and border edges
  typedef CGAL::dynamic_edge_property_t<bool>                                 Edge_property_tag;
  typedef typename boost::property_map<TriangleMesh, Edge_property_tag>::type EIFMap;
  EIFMap eif = get(Edge_property_tag(), local_mesh);

  VertexPointMap local_vpm = get_property_map(vertex_point, local_mesh);

  constrain_edges(faces(local_mesh), local_mesh, true /*constrain_borders*/,
                  constrain_sharp_edges, dihedral_angle, weak_DA, eif, local_vpm, gt);

  // @todo choice of number of iterations? Till convergence && max of 100?
  Polygon_mesh_processing::smooth_mesh(faces(local_mesh), local_mesh, CP::edge_is_constrained_map(eif)
                                                                         .number_of_iterations(100)
                                                                         .use_safety_constraints(false));

#ifdef CGAL_PMP_REMOVE_SELF_INTERSECTION_OUTPUT
  CGAL::IO::write_polygon_mesh("results/post_smoothing_local_mesh.off", local_mesh, CGAL::parameters::stream_precision(17));
#endif

  if(does_self_intersect(local_mesh))
  {
#ifdef CGAL_PMP_REMOVE_SELF_INTERSECTION_DEBUG
    std::cout << "  DEBUG: patch still self-intersecting after smoothing\n";
#endif
    return false;
  }
  if (!cc_envelope.is_empty() && !cc_envelope(local_mesh))
  {
#ifdef CGAL_PMP_REMOVE_SELF_INTERSECTION_DEBUG
    std::cout << "  DEBUG: patch is not in the input polyhedral envelope\n";
#endif
    return false;
  }

  // Patch is acceptable, swap it in
  std::vector<std::vector<Point> > patch;
  for(const face_descriptor f : faces(local_mesh))
  {
    halfedge_descriptor h = halfedge(f, local_mesh);
    patch.emplace_back(std::initializer_list<Point>{get(local_vpm, target(h, local_mesh)),
                                                    get(local_vpm, target(next(h, local_mesh), local_mesh)),
                                                    get(local_vpm, target(prev(h, local_mesh), local_mesh))});
  }

  std::set<face_descriptor> new_faces;
  replace_faces_with_patch(face_range, patch, tmesh, vpm, std::inserter(new_faces, new_faces.end()));

  CGAL_assertion(!does_self_intersect(new_faces, tmesh, parameters::vertex_point_map(vpm)));

#ifdef CGAL_PMP_REMOVE_SELF_INTERSECTION_DEBUG
  if(constrain_sharp_edges)
    ++self_intersections_solved_by_constrained_smoothing;
  else
    ++self_intersections_solved_by_unconstrained_smoothing;
#endif

  return true;
}

// -*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-

template <typename TriangleMesh>
bool order_border_halfedge_range(std::vector<typename boost::graph_traits<TriangleMesh>::halfedge_descriptor>& hrange,
                                 const TriangleMesh& tmesh)
{
  typedef typename boost::graph_traits<TriangleMesh>::vertex_descriptor     vertex_descriptor;

  CGAL_precondition(hrange.size() > 2);

  for(std::size_t i=0; i<hrange.size()-2; ++i)
  {
    const vertex_descriptor tgt = target(hrange[i], tmesh);
    for(std::size_t j=i+1; j<hrange.size(); ++j)
    {
      if(tgt == source(hrange[j], tmesh))
      {
        std::swap(hrange[i+1], hrange[j]);
        break;
      }

      // something went wrong while ordering halfedge (e.g. hole has more than one boundary cycle)
      if(j == hrange.size() - 1)
        return false;
    }
  }

  CGAL_postcondition(source(hrange.front(), tmesh) == target(hrange.back(), tmesh));
  return true;
}

// -*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-

#ifdef CGAL_PMP_REMOVE_SELF_INTERSECTION_OUTPUT

template <typename FaceContainer, typename TriangleMesh, typename VertexPointMap>
void dump_cc(const std::string filename,
             const FaceContainer& cc_faces,
             const TriangleMesh& mesh,
             const VertexPointMap vpm)
{
  typedef typename boost::graph_traits<TriangleMesh>::face_descriptor      face_descriptor;

#ifdef CGAL_PMP_REMOVE_SELF_INTERSECTION_DEBUG
  std::cout << "  DEBUG: Writing " << cc_faces.size() << " face(s) into " << filename << std::endl;
#endif

  std::ofstream out(filename);
  out.precision(17);

  out << "OFF\n";
  out << 3*cc_faces.size() << " " << cc_faces.size() << " 0\n";

  for(const face_descriptor f : cc_faces)
  {
    out << get(vpm, source(halfedge(f, mesh), mesh)) << "\n";
    out << get(vpm, target(halfedge(f, mesh), mesh)) << "\n";
    out << get(vpm, target(next(halfedge(f, mesh), mesh), mesh)) << "\n";
  }

  int id = 0;
  for(const face_descriptor f : cc_faces)
  {
    CGAL_USE(f);
    out << "3 " << id << " " << id+1 << " " << id+2 << "\n";
    id += 3;
  }

  out.close();
}

template <typename Point>
void dump_tentative_patch(std::vector<std::vector<Point> >& point_patch,
                         const std::string filename)
{
  std::ofstream out(filename);
  out << std::setprecision(17);

  std::map<Point, int> unique_points_with_id;
  for(const std::vector<Point>& face : point_patch)
    for(const Point& p : face)
      unique_points_with_id.insert(std::make_pair(p, 0));

  out << "OFF\n";
  out << unique_points_with_id.size() << " " << point_patch.size() << " 0\n";

  int unique_id = 0;
  for(auto& pp : unique_points_with_id)
  {
    out << pp.first << "\n";
    pp.second = unique_id++;
  }

  for(const std::vector<Point>& face : point_patch)
  {
    out << face.size();
    for(const Point& p : face)
      out << " " << unique_points_with_id.at(p);
    out << "\n";
  }

  out << std::endl;
  out.close();
}

#endif // CGAL_PMP_REMOVE_SELF_INTERSECTION_OUTPUT

// -*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-

// Hole filling can be influenced by setting a third point associated to an edge on the border of the hole.
// This third point is supposed to represent how the mesh continues on the other side of the hole.
// If that edge is a border edge, there is no third point (since the opposite face is the null face).
// Similarly if the edge is an internal sharp edge, we don't really want to use the opposite face because
// there is by definition a strong discontinuity and it might thus mislead the hole filling algorithm.
//
// Rather, we construct an artifical third point that is in the same plane as the face incident to `h`,
// defined as the third point of the imaginary equilateral triangle incident to opp(h, tmesh)
template <typename TriangleMesh, typename VertexPointMap, typename GeomTraits>
typename boost::property_traits<VertexPointMap>::value_type
construct_artificial_third_point(const typename boost::graph_traits<TriangleMesh>::halfedge_descriptor h,
                                 const TriangleMesh& tmesh,
                                 const VertexPointMap vpm,
                                 const GeomTraits& gt)
{
  typedef typename GeomTraits::FT                                           FT;
  typedef typename boost::property_traits<VertexPointMap>::value_type       Point;
  typedef typename boost::property_traits<VertexPointMap>::reference        Point_ref;
  typedef typename GeomTraits::Vector_3                                     Vector;

  const Point_ref p1 = get(vpm, source(h, tmesh));
  const Point_ref p2 = get(vpm, target(h, tmesh));
  const Point_ref opp_p = get(vpm, target(next(h, tmesh), tmesh));

  // sqrt(3)/2 to have an equilateral triangle with p1, p2, and third_point
  const FT dist = 0.5 * CGAL::sqrt(3.) * CGAL::approximate_sqrt(gt.compute_squared_distance_3_object()(p1, p2));

  const Vector ve1 = gt.construct_vector_3_object()(p1, p2);
  const Vector ve2 = gt.construct_vector_3_object()(p1, opp_p);

  // gram schmidt
  const FT e1e2_sp = gt.compute_scalar_product_3_object()(ve1, ve2);
  Vector orthogonalized_ve2 = gt.construct_sum_of_vectors_3_object()(
                                ve2, gt.construct_scaled_vector_3_object()(ve1, - e1e2_sp));
  Polygon_mesh_processing::internal::normalize(orthogonalized_ve2, gt);

  const Point mid_p1p2 = gt.construct_midpoint_3_object()(p1, p2);
  const Point third_p = gt.construct_translated_point_3_object()(
                          mid_p1p2, gt.construct_scaled_vector_3_object()(orthogonalized_ve2, -dist));

  return third_p;
}

// Patch is not valid if:
// - we insert the same face more than once
// - insert (geometric) non-manifold edges
template <typename TriangleMesh, typename Point>
bool check_patch_sanity(const std::vector<std::vector<Point> >& patch)
{
  std::set<std::set<Point> > unique_faces;
  std::map<std::set<Point>, int> unique_edges;

  for(const std::vector<Point>& face : patch)
  {
    if(!unique_faces.emplace(face.begin(), face.end()).second) // this face had already been found
      return false;

    int i = (unique_edges.insert(std::make_pair(std::set<Point> { face[0], face[1] }, 0)).first->second)++;
    if(i == 2) // non-manifold edge
      return false;

    i = (unique_edges.insert(std::make_pair(std::set<Point> { face[1], face[2] }, 0)).first->second)++;
    if(i == 2) // non-manifold edge
      return false;

    i = (unique_edges.insert(std::make_pair(std::set<Point> { face[2], face[0] }, 0)).first->second)++;
    if(i == 2) // non-manifold edge
      return false;
  }

  // Check for self-intersections within the patch
  // @todo something better than just making a mesh out of the soup?
  std::vector<Point> points;
  std::vector<std::vector<std::size_t> > faces;
  std::map<Point, std::size_t> ids;

  std::size_t c = 0;
  for(const std::vector<Point>& face : patch)
  {
    std::vector<std::size_t> ps_f;
    for(const Point& pt : face)
    {
      std::size_t id = c;
      std::pair<typename std::map<Point, std::size_t>::iterator, bool> is_insert_successful =
        ids.insert(std::make_pair(pt, c));
      if(is_insert_successful.second) // first time we've seen that point
      {
        ++c;
        points.push_back(pt);
      }
      else // already seen that point
      {
        id = is_insert_successful.first->second;
      }

      CGAL_assertion(id < points.size());
      ps_f.push_back(id);
    }

    faces.push_back(ps_f);
  }

  TriangleMesh patch_mesh;
  if(is_polygon_soup_a_polygon_mesh(faces))
    polygon_soup_to_polygon_mesh(points, faces, patch_mesh);
  else
    return false;

  if(does_self_intersect(patch_mesh))
  {
#ifdef CGAL_PMP_REMOVE_SELF_INTERSECTION_DEBUG
    std::cout << "  DEBUG: Tentative patch has self-intersections." << std::endl;
#endif

    return false;
  }

  return true;
}

template <typename TriangleMesh>
bool check_patch_compatibility(const std::vector<CGAL::Triple<int, int, int> >& hole_faces,
                               const std::vector<typename boost::graph_traits<TriangleMesh>::vertex_descriptor>& cc_border_vertices,
                               const std::set<typename boost::graph_traits<TriangleMesh>::edge_descriptor>& cc_interior_edges,
                               const TriangleMesh& tmesh)
{
  typedef typename boost::graph_traits<TriangleMesh>::halfedge_descriptor      halfedge_descriptor;

  typedef CGAL::Triple<int, int, int>                                          Face_indices;

  // make sure that the hole filling is valid: check that no edge
  // already in the mesh is present in hole_faces.
  bool non_manifold_edge_found = false;
  for(const Face_indices& triangle : hole_faces)
  {
    std::array<int, 6> edges = make_array(triangle.first, triangle.second,
                                          triangle.second, triangle.third,
                                          triangle.third, triangle.first);
    for(int k=0; k<3; ++k)
    {
      const int vi = edges[2*k], vj = edges[2*k+1];

      // ignore boundary edges
      if(vi+1 == vj || (vj == 0 && static_cast<std::size_t>(vi) == cc_border_vertices.size()-1))
        continue;

      halfedge_descriptor h = halfedge(cc_border_vertices[vi], cc_border_vertices[vj], tmesh).first;
      if(h != boost::graph_traits<TriangleMesh>::null_halfedge() &&
         cc_interior_edges.count(edge(h, tmesh)) == 0)
      {
        non_manifold_edge_found = true;
        break;
      }
    }

    if(non_manifold_edge_found)
      break;
  }

  if(non_manifold_edge_found)
  {
#ifdef CGAL_PMP_REMOVE_SELF_INTERSECTION_DEBUG
    std::cout << "  DEBUG: Triangulation produced is non-manifold when plugged into the mesh.\n";
#endif

    return false;
  }

  return true;
}

template <typename Point, typename GeomTraits>
bool construct_hole_patch(std::vector<CGAL::Triple<int, int, int> >& hole_faces,
                          const std::vector<Point>& hole_points,
                          const std::vector<Point>& third_points,
                          const GeomTraits& gt)
{
  if(hole_points.size() > 3)
  {
    triangulate_hole_polyline(hole_points, third_points, std::back_inserter(hole_faces),
                              parameters::geom_traits(gt));
  }
  else
  {
    hole_faces.emplace_back(0, 1, 2); // trivial hole filling
  }

  if(hole_faces.empty())
  {
#ifndef CGAL_HOLE_FILLING_DO_NOT_USE_DT3
 #ifdef CGAL_PMP_REMOVE_SELF_INTERSECTION_DEBUG
    std::cout << "  DEBUG: Failed to fill a hole using Delaunay search space.\n";
 #endif

    triangulate_hole_polyline(hole_points, third_points, std::back_inserter(hole_faces),
                              parameters::use_delaunay_triangulation(false).geom_traits(gt));
#endif
    if(hole_faces.empty())
    {
#ifdef CGAL_PMP_REMOVE_SELF_INTERSECTION_DEBUG
      std::cout << "  DEBUG: Failed to fill a hole using the whole search space.\n";
#endif
      return false;
    }
  }

#ifdef CGAL_PMP_REMOVE_SELF_INTERSECTION_OUTPUT
  std::cout << "  DEBUG: " << hole_faces.size() << " faces in the patch" << std::endl;
  std::vector<std::vector<Point> > to_dump;
  for(const auto& face : hole_faces)
  {
    to_dump.emplace_back(std::initializer_list<Point>{hole_points[face.first],
                                                      hole_points[face.second],
                                                      hole_points[face.third]});
  }

  CGAL_assertion(to_dump.size() == hole_faces.size());

  static int patch_id = 0;
  std::stringstream oss;
  oss << "results/tentative_patch_" << patch_id++ << ".off" << std::ends;
  const std::string filename = oss.str().c_str();

  dump_tentative_patch(to_dump, filename);
#endif

  return true;
}

template <typename TriangleMesh, typename GeomTraits>
struct Mesh_projection_functor
{
  typedef typename boost::graph_traits<TriangleMesh>::vertex_descriptor vertex_descriptor;

  typedef CGAL::AABB_face_graph_triangle_primitive<
            TriangleMesh, CGAL::Default /*VPM*/, CGAL::Tag_true /*single mesh*/, CGAL::Tag_true /*cache*/> Primitive;
  typedef CGAL::AABB_traits<GeomTraits, Primitive> Traits;
  typedef CGAL::AABB_tree<Traits> Tree;

  typedef typename GeomTraits::Point_3 Point_3;

  Mesh_projection_functor(const TriangleMesh& mesh)
    : mesh(mesh)
  {
    // mesh will be modified, but the tree stores the geometry
    tree = Tree(faces(mesh).begin(), faces(mesh).end(), mesh);
  }

  Point_3 operator()(const vertex_descriptor vd) const
  {
    return tree.closest_point(get(CGAL::vertex_point, mesh, vd));
  }

private:
  const TriangleMesh& mesh;
  Tree tree;
};

// Really rough and absurdly high complexity (at least factorize the AABB tree...)
template <typename TriangleMesh, typename Point, typename GeomTraits>
bool adapt_patch(std::vector<std::vector<Point> >& point_patch,
                 const TriangleMesh& tmesh,
                 const GeomTraits& gt)
{
  typedef typename boost::graph_traits<TriangleMesh>::vertex_descriptor        vertex_descriptor;
  typedef typename boost::graph_traits<TriangleMesh>::halfedge_descriptor      halfedge_descriptor;
  typedef typename boost::graph_traits<TriangleMesh>::edge_descriptor          edge_descriptor;
  typedef typename boost::graph_traits<TriangleMesh>::face_descriptor          face_descriptor;

  typedef typename GeomTraits::FT FT;

  std::vector<Point> soup_points;
  std::vector<std::array<FT, 3> > soup_faces;

  FT avg_edge_length = 0;
  int pid = 0;
  std::map<Point, std::size_t> point_ids;
  for(const auto& fp : point_patch)
  {
    CGAL_assertion(fp.size() == 3);
    std::array<FT, 3> f;
    for(std::size_t i=0; i<3; ++i)
    {
      avg_edge_length += CGAL::approximate_sqrt(squared_distance(fp[i], fp[(i+1)%3]));
      auto res = point_ids.emplace(fp[i], pid);
      if(res.second)
      {
        soup_points.push_back(fp[i]);
        ++pid;
      }
      f[i] = res.first->second;
    }
    soup_faces.push_back(f);
  }

  avg_edge_length /= FT(3 * soup_faces.size());
  FT target_edge_length = 0.7 * avg_edge_length;

  TriangleMesh local_mesh;
  polygon_soup_to_polygon_mesh(soup_points, soup_faces, local_mesh);
  bool has_SI = does_self_intersect(local_mesh);

  std::vector<halfedge_descriptor> border_hedges;
  border_halfedges(faces(local_mesh), local_mesh, std::back_inserter(border_hedges));
  typename TriangleMesh::template Property_map<edge_descriptor, bool> selected_edge =
    local_mesh.template add_property_map<edge_descriptor, bool>("e:selected",false).first;

  for(halfedge_descriptor h : border_hedges)
    selected_edge[edge(h, local_mesh)] = true;

  std::vector<vertex_descriptor> new_vertices;
  refine(local_mesh, faces(local_mesh), CGAL::Emptyset_iterator(), std::back_insert_iterator(new_vertices));

  Mesh_projection_functor<TriangleMesh, GeomTraits> projector(local_mesh);
  for(vertex_descriptor v : new_vertices)
    local_mesh.point(v) = projector(v);

#ifdef CGAL_PMP_REMOVE_SELF_INTERSECTION_OUTPUT
  std::cout << "  DEBUG: " << point_patch.size() << " faces in the adapted patch" << std::endl;
  static int adapted_patch_id = 0;
  std::stringstream oss;
  oss << "results/adapted_patch_" << adapted_patch_id++ << ".off" << std::ends;
  const std::string filename = oss.str().c_str();
  IO::write_polygon_mesh(filename, local_mesh);
#endif

  std::cout << "tentative patch self intersects? " << has_SI << std::endl;
  std::cout << "does self intersect = " << does_self_intersect(local_mesh) << std::endl;

  // If the adapted tentative patch has SI, revert back to the base patch
  if(does_self_intersect(local_mesh))
    return has_SI; // if the base patch also has self-intersections, we are done

  // Replace the tentative patch with the new, adapted patch
  point_patch.clear();
  point_patch.reserve(num_faces(local_mesh));

  auto local_vpm = get(CGAL::vertex_point, local_mesh);
  for(face_descriptor f : faces(local_mesh))
  {
    std::vector<Point> fp { get(local_vpm, target(halfedge(f, local_mesh), local_mesh)),
                            get(local_vpm, target(next(halfedge(f, local_mesh), local_mesh), local_mesh)),
                            get(local_vpm, source(halfedge(f, local_mesh), local_mesh)) };
    point_patch.push_back(fp);
  }

  return true;
}

// This overload uses hole filling to construct a patch and tests the manifoldness of the patch
template <typename TriangleMesh, typename Point, typename GeomTraits>
bool construct_manifold_hole_patch(std::vector<std::vector<Point> >& point_patch,
                                   const std::vector<Point>& hole_points,
                                   const std::vector<Point>& third_points,
                                   const std::vector<typename boost::graph_traits<TriangleMesh>::vertex_descriptor>& cc_border_vertices,
                                   const std::set<typename boost::graph_traits<TriangleMesh>::edge_descriptor>& cc_interior_edges,
                                   const TriangleMesh& tmesh,
                                   const GeomTraits& gt)
{
  typedef typename boost::graph_traits<TriangleMesh>::halfedge_descriptor      halfedge_descriptor;

  typedef CGAL::Triple<int, int, int>                                          Face_indices;

  // Try to triangulate the hole using default parameters
  // (using Delaunay search space if CGAL_HOLE_FILLING_DO_NOT_USE_DT3 is not defined)
  std::vector<Face_indices> hole_faces;
  construct_hole_patch(hole_faces, hole_points, third_points, gt);

  // Check manifoldness compatibility
  if(!check_patch_compatibility(hole_faces, cc_border_vertices, cc_interior_edges, tmesh))
  {
#ifdef CGAL_PMP_REMOVE_SELF_INTERSECTION_DEBUG
    std::cout << "  DEBUG: Incompatible patch" << std::endl;
#endif
    return false;
  }

  std::vector<std::vector<Point> > local_point_patch;
  local_point_patch.reserve(hole_faces.size());
  for(const Face_indices& face : hole_faces)
  {
    local_point_patch.emplace_back(std::initializer_list<Point>{hole_points[face.first],
                                                                hole_points[face.second],
                                                                hole_points[face.third]});
  }

  if(!adapt_patch(local_point_patch, tmesh, gt))
    return false;

  point_patch.reserve(point_patch.size() + local_point_patch.size());
  std::move(std::begin(local_point_patch), std::end(local_point_patch), std::back_inserter(point_patch));

  bool is_sane = check_patch_sanity<TriangleMesh>(point_patch);
#ifdef CGAL_PMP_REMOVE_SELF_INTERSECTION_DEBUG
  if(is_sane)
    std::cout << "  DEBUG: Found acceptable hole-filling patch\n";
  else
    std::cout << "  DEBUG: Insane hole-filling patch\n";
#endif

  return is_sane;
}

// This overloads fill the containers `cc_interior_vertices` and `cc_interior_edges`
template <typename TriangleMesh, typename Point, typename GeomTraits>
bool construct_tentative_hole_patch_with_border(std::vector<std::vector<Point> >& point_patch,
                                                const std::vector<Point>& hole_points,
                                                const std::vector<Point>& third_points,
                                                const std::vector<typename boost::graph_traits<TriangleMesh>::vertex_descriptor>& cc_border_vertices,
                                                const std::vector<typename boost::graph_traits<TriangleMesh>::halfedge_descriptor>& cc_border_hedges,
                                                std::set<typename boost::graph_traits<TriangleMesh>::vertex_descriptor>& cc_interior_vertices,
                                                std::set<typename boost::graph_traits<TriangleMesh>::edge_descriptor>& cc_interior_edges,
                                                const std::set<typename boost::graph_traits<TriangleMesh>::face_descriptor>& cc_faces,
                                                const TriangleMesh& tmesh,
                                                const GeomTraits& gt)
{
  typedef typename boost::graph_traits<TriangleMesh>::halfedge_descriptor   halfedge_descriptor;
  typedef typename boost::graph_traits<TriangleMesh>::face_descriptor       face_descriptor;

  CGAL_assertion(hole_points.size() == third_points.size());

  // Collect vertices and edges inside the current selection cc: first collect all vertices and
  // edges incident to the faces to remove...
  for(const face_descriptor f : cc_faces)
  {
    for(halfedge_descriptor h : halfedges_around_face(halfedge(f, tmesh), tmesh))
    {
      if(halfedge(target(h, tmesh), tmesh) == h) // to limit the number of insertions
        cc_interior_vertices.insert(target(h, tmesh));

      cc_interior_edges.insert(edge(h, tmesh));
    }
  }

  // ... and then remove those on the boundary
  for(halfedge_descriptor h : cc_border_hedges)
  {
    cc_interior_vertices.erase(target(h, tmesh));
    cc_interior_edges.erase(edge(h, tmesh));
  }

  return construct_manifold_hole_patch(point_patch, hole_points, third_points,
                                       cc_border_vertices, cc_interior_edges, tmesh, gt);
}

// This function constructs the ranges `hole_points` and `third_points`. Note that for a sub-hole,
// these two ranges are constructed in another function because we don't want to set 'third_points'
// for edges that are on the border of the sub-hole but not on the border of the (full) hole.
template <typename TriangleMesh, typename VertexPointMap, typename GeomTraits>
bool construct_tentative_hole_patch(std::vector<std::vector<typename boost::property_traits<VertexPointMap>::value_type> >& patch,
                                    std::vector<typename boost::graph_traits<TriangleMesh>::vertex_descriptor>& cc_border_vertices,
                                    std::set<typename boost::graph_traits<TriangleMesh>::vertex_descriptor>& cc_interior_vertices,
                                    std::set<typename boost::graph_traits<TriangleMesh>::edge_descriptor>& cc_interior_edges,
                                    const std::vector<typename boost::graph_traits<TriangleMesh>::halfedge_descriptor>& cc_border_hedges,
                                    const std::set<typename boost::graph_traits<TriangleMesh>::face_descriptor>& cc_faces,
                                    const TriangleMesh& tmesh,
                                    const VertexPointMap vpm,
                                    const GeomTraits& gt)
{
  typedef typename boost::graph_traits<TriangleMesh>::vertex_descriptor     vertex_descriptor;
  typedef typename boost::graph_traits<TriangleMesh>::halfedge_descriptor   halfedge_descriptor;

  typedef typename boost::property_traits<VertexPointMap>::value_type       Point;

  cc_border_vertices.reserve(cc_border_hedges.size());

  std::vector<Point> hole_points, third_points;
  hole_points.reserve(cc_border_hedges.size());
  third_points.reserve(cc_border_hedges.size());

  for(const halfedge_descriptor h : cc_border_hedges)
  {
    const vertex_descriptor v = source(h, tmesh);
    hole_points.push_back(get(vpm, v));
    cc_border_vertices.push_back(v);

    CGAL_assertion(!is_border(h, tmesh));

    if(is_border_edge(h, tmesh))
      third_points.push_back(construct_artificial_third_point(h, tmesh, vpm, gt));
    else
      third_points.push_back(get(vpm, target(next(opposite(h, tmesh), tmesh), tmesh)));
  }

  CGAL_postcondition(hole_points.size() >= 3);

  return construct_tentative_hole_patch_with_border(patch, hole_points, third_points,
                                                    cc_border_vertices, cc_border_hedges,
                                                    cc_interior_vertices, cc_interior_edges,
                                                    cc_faces, tmesh, gt);
}

// In this overload, we don't know the border of the patch because the face range is a sub-region
// of the hole. We also construct `hole_points` and `third_points`, but with no third point for internal
// sharp edges because a local self-intersection is usually caused by folding and thus we do not want
// a third point resulting from folding to wrongly influence the hole filling process.
template <typename TriangleMesh, typename VertexPointMap, typename GeomTraits>
bool construct_tentative_sub_hole_patch(std::vector<std::vector<typename boost::property_traits<VertexPointMap>::value_type> >& patch,
                                        const std::set<typename boost::graph_traits<TriangleMesh>::face_descriptor>& sub_cc_faces,
                                        const std::set<typename boost::graph_traits<TriangleMesh>::face_descriptor>& cc_faces,
                                        TriangleMesh& tmesh,
                                        VertexPointMap vpm,
                                        const GeomTraits& gt)
{
  typedef typename boost::graph_traits<TriangleMesh>::vertex_descriptor     vertex_descriptor;
  typedef typename boost::graph_traits<TriangleMesh>::halfedge_descriptor   halfedge_descriptor;
  typedef typename boost::graph_traits<TriangleMesh>::edge_descriptor       edge_descriptor;
  typedef typename boost::graph_traits<TriangleMesh>::face_descriptor       face_descriptor;

  typedef typename boost::property_traits<VertexPointMap>::value_type       Point;

  // Collect halfedges on the boundary of the region to be selected
  // (pointing inside the domain to be remeshed)
  std::set<halfedge_descriptor> internal_hedges;
  std::vector<halfedge_descriptor> cc_border_hedges;
  for(const face_descriptor fd : sub_cc_faces)
  {
    halfedge_descriptor h = halfedge(fd, tmesh);
    for(int i=0; i<3;++i)
    {
      if(is_border(opposite(h, tmesh), tmesh))
      {
         cc_border_hedges.push_back(h);
      }
      else
      {
        const face_descriptor opp_f = face(opposite(h, tmesh), tmesh);
        if(sub_cc_faces.count(opp_f) == 0)
        {
          cc_border_hedges.push_back(h);
          if(cc_faces.count(opp_f) != 0)
            internal_hedges.insert(h);
        }
      }

      h = next(h, tmesh);
    }
  }

  // Sort halfedges so that they describe the sequence of halfedges of the hole to be made
  if(!order_border_halfedge_range(cc_border_hedges, tmesh))
  {
#ifdef CGAL_PMP_REMOVE_SELF_INTERSECTION_DEBUG
    std::cout << "  DEBUG: More than one border in sub-hole. Not currently handled." << std::endl;
#endif

    return false;
  }

  // @todo we don't care about these sets, so instead there could be a system of output iterators
  // in construct_tentative_hole_patch() instead (and here would be emptyset iterators).
  std::set<vertex_descriptor> cc_interior_vertices;
  std::set<edge_descriptor> cc_interior_edges;

  std::vector<vertex_descriptor> cc_border_vertices;
  cc_border_vertices.reserve(cc_border_hedges.size());

  std::vector<Point> hole_points, third_points;
  hole_points.reserve(cc_border_hedges.size());
  third_points.reserve(cc_border_hedges.size());

  for(const halfedge_descriptor h : cc_border_hedges)
  {
    const vertex_descriptor v = source(h, tmesh);
    hole_points.push_back(get(vpm, v));
    cc_border_vertices.push_back(v);

    CGAL_assertion(!is_border(h, tmesh));

    if(internal_hedges.count(h) == 0 && // `h` is on the border of the full CC
       !is_border_edge(h, tmesh))
    {
      third_points.push_back(get(vpm, target(next(opposite(h, tmesh), tmesh), tmesh)));
    }
    else // `h` is on the border of the sub CC but not on the border of the full CC
    {
      const Point tp = construct_artificial_third_point(h, tmesh, vpm, gt);
      third_points.push_back(tp);
    }
  }

  return construct_tentative_hole_patch_with_border(patch, hole_points, third_points,
                                                    cc_border_vertices, cc_border_hedges,
                                                    cc_interior_vertices, cc_interior_edges,
                                                    sub_cc_faces, tmesh, gt);
}

// -*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-*-

// This function is only called when the hole is NOT subdivided into smaller holes
template <typename TriangleMesh, typename VertexPointMap, typename GeomTraits, typename PolyhedralEnvelope>
bool fill_hole(std::vector<typename boost::graph_traits<TriangleMesh>::halfedge_descriptor>& cc_border_hedges,
               const std::set<typename boost::graph_traits<TriangleMesh>::face_descriptor>& cc_faces,
               std::set<typename boost::graph_traits<TriangleMesh>::face_descriptor>& working_face_range,
               TriangleMesh& tmesh,
               const PolyhedralEnvelope& cc_envelope,
               VertexPointMap vpm,
               const GeomTraits& gt)
{
  typedef typename boost::graph_traits<TriangleMesh>::vertex_descriptor     vertex_descriptor;
  typedef typename boost::graph_traits<TriangleMesh>::edge_descriptor       edge_descriptor;
  typedef typename boost::graph_traits<TriangleMesh>::face_descriptor       face_descriptor;

  typedef typename boost::property_traits<VertexPointMap>::value_type       Point;

#ifdef CGAL_PMP_REMOVE_SELF_INTERSECTION_DEBUG
  std::cout << "  DEBUG: Attempting hole-filling (no constraints), " << cc_faces.size() << " faces\n";
#endif

  if(!order_border_halfedge_range(cc_border_hedges, tmesh))
  {
    CGAL_assertion(false); // we shouldn't fail to orient the boundary cycle of the complete hole
    return false;
  }

  std::set<vertex_descriptor> cc_interior_vertices;
  std::set<edge_descriptor> cc_interior_edges;

  std::vector<vertex_descriptor> cc_border_vertices;
  cc_border_vertices.reserve(cc_border_hedges.size());

  std::vector<std::vector<Point> > patch;
  if(!construct_tentative_hole_patch(patch, cc_border_vertices, cc_interior_vertices, cc_interior_edges,
                                     cc_border_hedges, cc_faces, tmesh, vpm, gt))
  {
#ifdef CGAL_PMP_REMOVE_SELF_INTERSECTION_DEBUG
    std::cout << "  DEBUG: Failed to find acceptable hole patch\n";
#endif

    return false;
  }

  if(!cc_envelope.is_empty() && !cc_envelope(patch))
  {
#ifdef CGAL_PMP_REMOVE_SELF_INTERSECTION_DEBUG
    std::cout << "  DEBUG: Patch is not inside the input polyhedral envelope\n";
#endif
    return false;
  }

  for(const face_descriptor f : cc_faces)
    working_face_range.erase(f);

  // Plug the new triangles in the mesh, reusing previous edges and faces
  replace_faces_with_patch(cc_border_vertices, cc_interior_vertices,
                           cc_border_hedges, cc_interior_edges,
                           cc_faces, patch, tmesh, vpm,
                           std::inserter(working_face_range, working_face_range.end()));

#ifdef CGAL_PMP_REMOVE_SELF_INTERSECTION_OUTPUT
  static int filed_hole_id = 0;
  std::stringstream oss;
  oss << "results/filled_basic_" << filed_hole_id++ << ".off" << std::ends;
  std::ofstream(oss.str().c_str()) << std::setprecision(17) << tmesh;
#endif

  CGAL_postcondition(is_valid_polygon_mesh(tmesh));

  return true;
}

// Same function as above but border of the hole is not known
template <typename TriangleMesh, typename VertexPointMap, typename GeomTraits, typename PolyhedralEnvelope>
bool fill_hole(const std::set<typename boost::graph_traits<TriangleMesh>::face_descriptor>& cc_faces,
               std::set<typename boost::graph_traits<TriangleMesh>::face_descriptor>& working_face_range,
               TriangleMesh& tmesh,
               const PolyhedralEnvelope& cc_envelope,
               VertexPointMap vpm,
               const GeomTraits& gt)
{
  typedef typename boost::graph_traits<TriangleMesh>::halfedge_descriptor   halfedge_descriptor;
  typedef typename boost::graph_traits<TriangleMesh>::face_descriptor       face_descriptor;

  std::vector<halfedge_descriptor> cc_border_hedges;
  for(face_descriptor fd : cc_faces)
  {
    halfedge_descriptor h = halfedge(fd, tmesh);
    for(int i=0; i<3; ++i)
    {
      if(is_border(opposite(h, tmesh), tmesh) || cc_faces.count(face(opposite(h, tmesh), tmesh)) == 0)
        cc_border_hedges.push_back(h);

      h = next(h, tmesh);
    }
  }

  if(order_border_halfedge_range(cc_border_hedges, tmesh))
    return fill_hole(cc_border_hedges, cc_faces, working_face_range, tmesh, cc_envelope,vpm, gt);
  else
    return false;
}

template <typename TriangleMesh, typename VertexPointMap, typename GeomTraits, typename PolyhedralEnvelope>
bool fill_hole_with_constraints(std::vector<typename boost::graph_traits<TriangleMesh>::halfedge_descriptor>& cc_border_hedges,
                                const std::set<typename boost::graph_traits<TriangleMesh>::face_descriptor>& cc_faces,
                                std::set<typename boost::graph_traits<TriangleMesh>::face_descriptor>& working_face_range,
                                TriangleMesh& tmesh,
                                const double dihedral_angle,
                                const double weak_DA,
                                const PolyhedralEnvelope& cc_envelope,
                                VertexPointMap vpm,
                                const GeomTraits& gt)
{
  typedef typename boost::graph_traits<TriangleMesh>::face_descriptor       face_descriptor;

  typedef typename boost::property_traits<VertexPointMap>::value_type       Point;

#ifdef CGAL_PMP_REMOVE_SELF_INTERSECTION_DEBUG
  std::cout << "  DEBUG: Attempting local hole-filling with constrained sharp edges..." << std::endl;
#endif

  // If we are treating self intersections locally, first try to constrain sharp edges in the hole
  typedef CGAL::dynamic_edge_property_t<bool>                                 Edge_property_tag;
  typedef typename boost::property_map<TriangleMesh, Edge_property_tag>::type EIFMap;
  EIFMap eif = get(Edge_property_tag(), tmesh);

  constrain_edges(cc_faces, tmesh, true /*constrain_border_edges*/, true /*constrain_sharp_edges*/,
                  dihedral_angle, weak_DA, eif, vpm, gt);

  // Partition the hole using these constrained edges
  std::set<face_descriptor> visited_faces;
  std::vector<std::vector<Point> > patch;

  int cc_counter = 0;
  for(face_descriptor f : cc_faces)
  {
    if(!visited_faces.insert(f).second) // already visited that face
      continue;

    // gather the faces making a sub-hole
    std::set<face_descriptor> sub_cc;
    Polygon_mesh_processing::connected_component(f, tmesh, std::inserter(sub_cc, sub_cc.end()),
                                                 CGAL::parameters::edge_is_constrained_map(eif));

    visited_faces.insert(sub_cc.begin(), sub_cc.end());
#ifdef CGAL_PMP_REMOVE_SELF_INTERSECTION_DEBUG
    std::cout << "CC of size " << sub_cc.size() << " (total: " << cc_faces.size() << ")" << std::endl;
#endif
    ++cc_counter;

#ifdef CGAL_PMP_REMOVE_SELF_INTERSECTION_OUTPUT
    dump_cc("results/current_cc.off", sub_cc, tmesh, vpm);
#endif

    // The mesh is not modified, but 'patch' gets filled
    if(!construct_tentative_sub_hole_patch(patch, sub_cc, cc_faces, tmesh, vpm, gt))
    {
      // Something went wrong while finding a potential cover for the a sub-hole --> use basic hole-filling
      return fill_hole(cc_border_hedges, cc_faces, working_face_range, tmesh, cc_envelope,vpm, gt);
    }
  }

#ifdef CGAL_PMP_REMOVE_SELF_INTERSECTION_DEBUG
  std::cout << cc_counter << " independent sub holes" << std::endl;
#endif

#ifdef CGAL_PMP_REMOVE_SELF_INTERSECTION_OUTPUT
  std::ofstream out("results/hole_fillers.off");
  out.precision(17);

  out << "OFF\n";
  out << 3*patch.size() << " " << patch.size() << " 0\n";

  for(const auto& f : patch)
  {
    for(const auto& pt : f)
      out << pt << "\n";
  }

  int id = 0;
  for(std::size_t i=0; i<patch.size(); ++i)
  {
    out << "3 " << id << " " << id+1 << " " << id+2 << "\n";
    id += 3;
  }
  out.close();
#endif

  // We're assembling multiple patches so we could have the same face appearing multiple times...
  if(!check_patch_sanity<TriangleMesh>(patch))
  {
#ifdef CGAL_PMP_REMOVE_SELF_INTERSECTION_DEBUG
    std::cout << "  DEBUG: Unhealthy patch, defaulting to basic fill_hole()" << std::endl;
#endif
    return fill_hole(cc_border_hedges, cc_faces, working_face_range, tmesh, cc_envelope, vpm, gt);
  }

  // check if the patch is inside the input polyhedral envelope
  if(!cc_envelope.is_empty() && !cc_envelope(patch))
  {
#ifdef CGAL_PMP_REMOVE_SELF_INTERSECTION_DEBUG
    std::cout << "  DEBUG: Patch is not entirely inside the input polyhedral envelope, defaulting to basic fill_hole()" << std::endl;
#endif
    return fill_hole(cc_border_hedges, cc_faces, working_face_range, tmesh, cc_envelope, vpm, gt);
  }

  // Plug the hole-filling patch in the mesh
  std::set<face_descriptor> new_faces;
  replace_faces_with_patch(cc_faces, patch, tmesh, vpm, std::inserter(new_faces, new_faces.end()));

  // Otherwise it should have failed the sanity check
  CGAL_assertion(!does_self_intersect(new_faces, tmesh, parameters::vertex_point_map(vpm)));

  // Update working range with the new faces
  for(const face_descriptor f : cc_faces)
    working_face_range.erase(f);

  working_face_range.insert(new_faces.begin(), new_faces.end());

  return true;
}

template <class Box, class TM, class VPM, class GT, class OutputIterator>
struct Strict_intersect_edges // "strict" as in "not sharing a vertex"
{
  typedef typename boost::graph_traits<TM>::halfedge_descriptor               halfedge_descriptor;
  typedef typename GT::Segment_3                                              Segment;

  mutable OutputIterator m_iterator;
  const TM& m_tmesh;
  const VPM m_vpmap;

  typename GT::Construct_segment_3 m_construct_segment;
  typename GT::Do_intersect_3 m_do_intersect;

  Strict_intersect_edges(const TM& tmesh, VPM vpmap, const GT& gt, OutputIterator it)
    :
      m_iterator(it),
      m_tmesh(tmesh),
      m_vpmap(vpmap),
      m_construct_segment(gt.construct_segment_3_object()),
      m_do_intersect(gt.do_intersect_3_object())
  {}

  void operator()(const Box* b, const Box* c) const
  {
    const halfedge_descriptor h = b->info();
    const halfedge_descriptor g = c->info();

    if(source(h, m_tmesh) == target(g, m_tmesh) || target(h, m_tmesh) == source(g, m_tmesh))
      return;

    const Segment s1 = m_construct_segment(get(m_vpmap, source(h, m_tmesh)), get(m_vpmap, target(h, m_tmesh)));
    const Segment s2 = m_construct_segment(get(m_vpmap, source(g, m_tmesh)), get(m_vpmap, target(g, m_tmesh)));

    if(m_do_intersect(s1, s2))
      *m_iterator++ = std::make_pair(b->info(), c->info());
  }
};

template <typename TriangleMesh, typename VertexPointMap, typename GeomTraits>
bool is_simple_3(const std::vector<typename boost::graph_traits<TriangleMesh>::halfedge_descriptor>& cc_border_hedges,
                 const TriangleMesh& tmesh,
                 VertexPointMap vpm,
                 const GeomTraits& gt)
{
  typedef typename boost::graph_traits<TriangleMesh>::halfedge_descriptor                       halfedge_descriptor;

  typedef typename boost::property_traits<VertexPointMap>::reference                            Point_ref;

  typedef CGAL::Box_intersection_d::ID_FROM_BOX_ADDRESS                                         Box_policy;
  typedef CGAL::Box_intersection_d::Box_with_info_d<double, 3, halfedge_descriptor, Box_policy> Box;

  std::vector<Box> boxes;
  boxes.reserve(cc_border_hedges.size());

  for(halfedge_descriptor h : cc_border_hedges)
  {
    const Point_ref p = get(vpm, source(h, tmesh));
    const Point_ref q = get(vpm, target(h, tmesh));
    CGAL_assertion(!gt.equal_3_object()(p, q));

    boxes.emplace_back(p.bbox() + q.bbox(), h);
  }

  // generate box pointers
  std::vector<const Box*> box_ptr;
  box_ptr.reserve(boxes.size());

  for(Box& b : boxes)
    box_ptr.push_back(&b);

  typedef boost::function_output_iterator<CGAL::internal::Throw_at_output>          Throwing_output_iterator;
  typedef internal::Strict_intersect_edges<Box, TriangleMesh, VertexPointMap,
                                           GeomTraits, Throwing_output_iterator>    Throwing_filter;
  Throwing_filter throwing_filter(tmesh, vpm, gt, Throwing_output_iterator());

  try
  {
    const std::ptrdiff_t cutoff = 2000;
    CGAL::box_self_intersection_d<Parallel_if_available_tag>(box_ptr.begin(), box_ptr.end(), throwing_filter, cutoff);
  }
  catch(CGAL::internal::Throw_at_output_exception&)
  {
    return false;
  }

  return true;
}

template <typename TriangleMesh, typename VertexPointMap, typename GeomTraits, typename PolyhedralEnvelope>
bool remove_self_intersections_with_hole_filling(std::vector<typename boost::graph_traits<TriangleMesh>::halfedge_descriptor>& cc_border_hedges,
                                                 const std::set<typename boost::graph_traits<TriangleMesh>::face_descriptor>& cc_faces,
                                                 std::set<typename boost::graph_traits<TriangleMesh>::face_descriptor>& working_face_range,
                                                 TriangleMesh& tmesh,
                                                 bool local_self_intersection_removal,
                                                 const double strong_dihedral_angle,
                                                 const double weak_dihedral_angle,
                                                 const PolyhedralEnvelope& cc_envelope,
                                                 VertexPointMap vpm,
                                                 const GeomTraits& gt)
{
#ifdef CGAL_PMP_REMOVE_SELF_INTERSECTION_OUTPUT
  std::ofstream out("results/zone_border.polylines.txt");
  out << std::setprecision(17);
  for(const auto& h : cc_border_hedges)
    out << "2 " << tmesh.point(source(h, tmesh)) << " " << tmesh.point(target(h, tmesh)) << std::endl;
  out.close();
#endif

  if(!is_simple_3(cc_border_hedges, tmesh, vpm, gt))
  {
#ifdef CGAL_PMP_REMOVE_SELF_INTERSECTION_DEBUG
    std::cout << "Hole filling cannot handle non-simple border" << std::endl;
#endif
    return false;
  }

#ifdef CGAL_PMP_REMOVE_SELF_INTERSECTIONS_NO_CONSTRAINTS_IN_HOLE_FILLING
  // Do not try to impose sharp edge constraints if we are not doing local-only self intersections removal
  local_self_intersection_removal = false;
#endif

  bool success = false;
  if(local_self_intersection_removal)
  {
    success = fill_hole_with_constraints(cc_border_hedges, cc_faces, working_face_range, tmesh,
                                         strong_dihedral_angle, weak_dihedral_angle,
                                         cc_envelope, vpm, gt);
  }
  else
  {
    success = fill_hole(cc_border_hedges, cc_faces, working_face_range, tmesh, cc_envelope, vpm, gt);
  }

#ifdef CGAL_PMP_REMOVE_SELF_INTERSECTION_DEBUG
  if(success)
  {
    if(local_self_intersection_removal)
      ++self_intersections_solved_by_constrained_hole_filling;
    else
      ++self_intersections_solved_by_unconstrained_hole_filling;
  }
#endif

  return success;
}

template <typename TriangleMesh, typename PolyhedralEnvelope, typename VertexPointMap, typename GeomTraits>
bool handle_CC_with_complex_topology(std::vector<typename boost::graph_traits<TriangleMesh>::halfedge_descriptor>& cc_border_hedges,
                                     const std::set<typename boost::graph_traits<TriangleMesh>::face_descriptor>& cc_faces,
                                     std::set<typename boost::graph_traits<TriangleMesh>::face_descriptor>& working_face_range,
                                     TriangleMesh& tmesh,
                                     bool local_self_intersection_removal,
                                     const double strong_dihedral_angle,
                                     const double weak_dihedral_angle,
                                     const bool preserve_genus,
                                     const PolyhedralEnvelope& cc_envelope,
                                     VertexPointMap vpm,
                                     const GeomTraits& gt)
{
  typedef typename boost::graph_traits<TriangleMesh>::vertex_descriptor        vertex_descriptor;
  typedef typename boost::graph_traits<TriangleMesh>::halfedge_descriptor      halfedge_descriptor;
  typedef typename boost::graph_traits<TriangleMesh>::edge_descriptor          edge_descriptor;
  typedef typename boost::graph_traits<TriangleMesh>::face_descriptor          face_descriptor;

  typedef typename GeomTraits::FT                                              FT;
  typedef typename boost::property_traits<VertexPointMap>::value_type          Point;

#ifdef CGAL_PMP_REMOVE_SELF_INTERSECTION_DEBUG
    std::cout << "  DEBUG: CC with Euler_chi != 1" << std::endl;
#endif

  if(preserve_genus)
  {
#ifdef CGAL_PMP_REMOVE_SELF_INTERSECTION_DEBUG
    std::cout << "  DEBUG: CC not handled, selection is not a topological disk (preserve_genus=true)\n";
#endif
    return false;
  }

  const CGAL::Face_filtered_graph<TriangleMesh> ccmesh(tmesh, cc_faces);
  if(!ccmesh.is_selection_valid())
    return false;

  std::vector<halfedge_descriptor> boundary_reps;
  extract_boundary_cycles(ccmesh, std::back_inserter(boundary_reps));

#ifdef CGAL_PMP_REMOVE_SELF_INTERSECTION_DEBUG
  std::cout << "  DEBUG: " << boundary_reps.size() << " borders in the CC\n";
#endif

  if(boundary_reps.size() == 1)
  {
#ifdef CGAL_PMP_REMOVE_SELF_INTERSECTION_DEBUG
    std::cout << "  DEBUG: Complex topology but single border --> standard hole filling\n";
#endif

    // If there is a single border, fill the hole as if it were a topological disk.
    // This will lose some information since chi != -1, but preserve_genus = false here
    return remove_self_intersections_with_hole_filling(cc_border_hedges, cc_faces, working_face_range,
                                                       tmesh, local_self_intersection_removal,
                                                       strong_dihedral_angle, weak_dihedral_angle,
                                                       cc_envelope, vpm, gt);
  }

  std::vector<bool> is_hole_incident_to_patch(boundary_reps.size());
  std::vector<FT> hole_lengths(boundary_reps.size());

  int holes_incident_to_patches_n = 0;
  for(std::size_t hole_id = 0; hole_id<boundary_reps.size(); ++hole_id)
  {
    FT border_length = 0;
    bool is_incident_to_patch = false;
    halfedge_descriptor bh = boundary_reps[hole_id], end = bh;
    do
    {
      border_length += edge_length(edge(bh, tmesh), tmesh, CGAL::parameters::vertex_point_map(vpm)
                                                                            .geom_traits(gt));
      if(!is_border(bh, tmesh)) // note the 'tmesh'
      {
        is_incident_to_patch = true;
        ++holes_incident_to_patches_n;
      }

      bh = next(bh, ccmesh);
    }
    while(bh != end);

    is_hole_incident_to_patch[hole_id] = is_incident_to_patch;
    hole_lengths[hole_id] = border_length;
  }

  // If all border halfedges are "real" border halfedges (i.e., they are border halfedges
  // when looked at in tmesh), then fill only the longest hole
  // @todo when islands can be handled, something better could be attempted
  if(holes_incident_to_patches_n == 0)
  {
#ifdef CGAL_PMP_REMOVE_SELF_INTERSECTION_DEBUG
    std::cout << "  DEBUG: Complex topology, multiple borders, hole filling the longest border\n";
#endif

    int longest_border_id = *(std::max_element(hole_lengths.begin(), hole_lengths.end()));

    std::vector<halfedge_descriptor> longest_border_hedges;
    halfedge_descriptor bh = boundary_reps[longest_border_id], end = bh;
    do
    {
      longest_border_hedges.push_back(opposite(bh, tmesh));
      bh = prev(bh, ccmesh); // prev because we insert the opposite
    }
    while(bh != end);

    // @todo this currently doesn't attempt to constrain sharp edges
    return fill_hole(longest_border_hedges, cc_faces, working_face_range, tmesh, cc_envelope, vpm, gt);
  }

  // If there exists some boundary cycles with "fake" border halfedges, fill those
#ifdef CGAL_PMP_REMOVE_SELF_INTERSECTION_DEBUG
  std::cout << "  DEBUG: Complex topology, some fake borders @todo\n";
#endif

  std::vector<std::vector<Point> > patch;

  for(std::size_t hole_id=0; hole_id<boundary_reps.size(); ++hole_id)
  {
    if(!is_hole_incident_to_patch[hole_id])
      continue;

    std::vector<halfedge_descriptor> border_hedges;
    halfedge_descriptor bh = boundary_reps[hole_id], end = bh;
    do
    {
      border_hedges.push_back(opposite(bh, tmesh));
      bh = prev(bh, ccmesh); // prev because we insert the opposite
    }
    while(bh != end);

    std::vector<vertex_descriptor> border_vertices;
    border_vertices.reserve(border_hedges.size());

    std::vector<Point> hole_points, third_points;
    hole_points.reserve(border_hedges.size());
    third_points.reserve(border_hedges.size());

    for(const halfedge_descriptor h : border_hedges)
    {
      const vertex_descriptor v = source(h, tmesh);
      hole_points.push_back(get(vpm, v));
      border_vertices.push_back(v);

      CGAL_assertion(!is_border(h, tmesh));

      if(is_border_edge(h, tmesh))
        third_points.push_back(construct_artificial_third_point(h, tmesh, vpm, gt));
      else
        third_points.push_back(get(vpm, target(next(opposite(h, tmesh), tmesh), tmesh)));
    }

    std::set<vertex_descriptor> interior_vertices;
    std::set<edge_descriptor> interior_edges;

    if(!construct_tentative_hole_patch_with_border(patch, hole_points, third_points,
                                                   border_vertices, border_hedges,
                                                   interior_vertices, interior_edges,
                                                   cc_faces, tmesh, gt))
      return false;
  }

  // Built the patch from all the boundary cycles, put it in
#ifdef CGAL_PMP_REMOVE_SELF_INTERSECTION_OUTPUT
  std::ofstream out("results/multiple_real_borders.off");
  out.precision(17);

  out << "OFF\n";
  out << 3*patch.size() << " " << patch.size() << " 0\n";

  for(const auto& f : patch)
  {
    for(const auto& pt : f)
      out << pt << "\n";
  }

  int id = 0;
  for(std::size_t i=0; i<patch.size(); ++i)
  {
    out << "3 " << id << " " << id+1 << " " << id+2 << "\n";
    id += 3;
  }
  out.close();
#endif

  // We're assembling multiple patches so we could have the same face appearing multiple times...
  if(!check_patch_sanity<TriangleMesh>(patch))
  {
#ifdef CGAL_PMP_REMOVE_SELF_INTERSECTION_DEBUG
    std::cout << "  DEBUG: Unhealthy patch, defaulting to basic fill_hole()" << std::endl;
#endif
    return false;
  }

  // check if the patch is inside the input polyhedral envelope
  if(!cc_envelope.is_empty() && !cc_envelope(patch))
  {
#ifdef CGAL_PMP_REMOVE_SELF_INTERSECTION_DEBUG
    std::cout << "  DEBUG: Patch is not entirely inside the input polyhedral envelope, defaulting to basic fill_hole()" << std::endl;
#endif
    return false;
  }

  // Plug the hole-filling patch in the mesh
  std::set<face_descriptor> new_faces;
  replace_faces_with_patch(cc_faces, patch, tmesh, vpm, std::inserter(new_faces, new_faces.end()));

  // Otherwise it should have failed the sanity check
  CGAL_assertion(!does_self_intersect(new_faces, tmesh, parameters::vertex_point_map(vpm)));

  // Update working range with the new faces
  for(const face_descriptor f : cc_faces)
    working_face_range.erase(f);

  working_face_range.insert(new_faces.begin(), new_faces.end());

  return true;
}

// the parameter `step` controls how many extra layers of faces we take around the range `faces_to_remove`
template <typename TriangleMesh, typename VertexPointMap, typename GeomTraits, typename Visitor>
std::pair<bool, bool>
remove_self_intersections_one_step(std::set<typename boost::graph_traits<TriangleMesh>::face_descriptor>& faces_to_remove,
                                   std::set<typename boost::graph_traits<TriangleMesh>::face_descriptor>& working_face_range,
                                   TriangleMesh& tmesh,
                                   const int step,
                                   const bool preserve_genus,
                                   const bool only_treat_self_intersections_locally,
                                   const double strong_dihedral_angle,
                                   const double weak_dihedral_angle,
                                   const double containment_epsilon,
                                   VertexPointMap vpm,
                                   const GeomTraits& gt,
                                   Visitor& visitor)
{
  typedef boost::graph_traits<TriangleMesh>                               graph_traits;
  typedef typename graph_traits::vertex_descriptor                        vertex_descriptor;
  typedef typename graph_traits::halfedge_descriptor                      halfedge_descriptor;
  typedef typename graph_traits::face_descriptor                          face_descriptor;

  std::set<face_descriptor> faces_to_remove_copy = faces_to_remove;

#if defined(CGAL_PMP_REMOVE_SELF_INTERSECTION_DEBUG) || defined(CGAL_PMP_REMOVE_SELF_INTERSECTION_OUTPUT)
  static int call_id = -1;
  ++call_id;
#endif

#ifdef CGAL_PMP_REMOVE_SELF_INTERSECTION_DEBUG
  std::cout << "##### running remove_self_intersections_one_step (#" << call_id << "), step " << step
            << " with " << faces_to_remove.size() << " intersecting faces\n";
#endif

  bool something_was_done = false; // indicates if a region was successfully remeshed
  bool all_fixed = true; // indicates if all removal went well
  // indicates if a removal was not possible because the region handle has
  // some boundary cycle of halfedges
  bool topology_issue = false;

#ifdef CGAL_PMP_REMOVE_SELF_INTERSECTION_DEBUG
  std::cout << "  DEBUG: is_valid in one_step(tmesh)? " << is_valid_polygon_mesh(tmesh) << "\n";
  std::cout.flush();

  unsolved_self_intersections = 0;
#endif

  CGAL_precondition(is_valid_polygon_mesh(tmesh));
#if defined(CGAL_PMP_REMOVE_SELF_INTERSECTION_DEBUG) || defined(CGAL_PMP_REMOVE_SELF_INTERSECTION_OUTPUT)
  int cc_id = -1;
#endif

  while(!faces_to_remove.empty())
  {
    if(visitor.stop())
      return std::make_pair(false, false);

    visitor.start_component_handling();
    visitor.status_update(faces_to_remove);
#if defined(CGAL_PMP_REMOVE_SELF_INTERSECTION_DEBUG) || defined(CGAL_PMP_REMOVE_SELF_INTERSECTION_OUTPUT)
    ++cc_id;
#endif

#ifdef CGAL_PMP_REMOVE_SELF_INTERSECTION_DEBUG
    std::cout << "  DEBUG: --------------- Considering CC #" << cc_id << " remaining faces to remove: " << faces_to_remove.size() << "\n";
#endif

    // Process a connected component of faces to remove.
    // collect all the faces from the connected component
    std::set<face_descriptor> cc_faces;
    std::vector<face_descriptor> queue(1, *faces_to_remove.begin()); // temporary queue
    cc_faces.insert(queue.back());
    while(!queue.empty())
    {
      face_descriptor top = queue.back();
      queue.pop_back();
      halfedge_descriptor h = halfedge(top, tmesh);
      for(int i=0; i<3; ++i)
      {
        face_descriptor adjacent_face = face(opposite(h, tmesh), tmesh);
        if(adjacent_face!=boost::graph_traits<TriangleMesh>::null_face())
        {
          if(faces_to_remove.count(adjacent_face) != 0 && cc_faces.insert(adjacent_face).second)
            queue.push_back(adjacent_face);
        }

        h = next(h, tmesh);
      }
    }

#ifdef CGAL_PMP_REMOVE_SELF_INTERSECTION_DEBUG
    std::cout << "  DEBUG: " << cc_faces.size() << " faces in the current CC\n";
    std::cout << "  DEBUG: first face: " << get(vpm, source(halfedge(*(cc_faces.begin()), tmesh), tmesh)) << " "
              << get(vpm, target(halfedge(*(cc_faces.begin()), tmesh), tmesh)) << " "
              << get(vpm, target(next(halfedge(*(cc_faces.begin()), tmesh), tmesh), tmesh)) << "\n";
#endif

#ifdef CGAL_PMP_REMOVE_SELF_INTERSECTION_OUTPUT
    std::string fname = "results/initial_r_"+std::to_string(call_id)+"_CC_" + std::to_string(cc_id)+"_step_"+std::to_string(step)+".off";
    dump_cc(fname, cc_faces, tmesh, vpm);

    fname = "results/mesh_at_r_"+std::to_string(call_id)+"_CC_"+std::to_string(cc_id)+"_step_"+std::to_string(step)+".off";
    std::ofstream mout(fname);
    mout << std::setprecision(17) << tmesh;
    mout.close();
#endif

    // expand the region to be filled
    if(step > 0)
    {
      expand_face_selection(cc_faces, tmesh, step,
                            make_boolean_property_map(cc_faces),
                            Emptyset_iterator());
    }

#ifdef CGAL_PMP_REMOVE_SELF_INTERSECTION_OUTPUT
    fname = "results/expanded_r_"+std::to_string(call_id)+"_CC_"+std::to_string(cc_id)+"_step_"+std::to_string(step)+".off";
    dump_cc(fname, cc_faces, tmesh, vpm);
#endif

    // @todo keep this?
    // try to compactify the selection region by also selecting all the faces included
    // in the bounding box of the initial selection
    std::vector<halfedge_descriptor> stack_for_expension;
    Bbox_3 bb;
    for(face_descriptor fd : cc_faces)
    {
      for(halfedge_descriptor h : halfedges_around_face(halfedge(fd, tmesh), tmesh))
      {
        bb += get(vpm, target(h, tmesh)).bbox();
        face_descriptor nf = face(opposite(h, tmesh), tmesh);
        if(nf != boost::graph_traits<TriangleMesh>::null_face() && cc_faces.count(nf) == 0)
        {
          stack_for_expension.push_back(opposite(h, tmesh));
        }
      }
    }

    while(!stack_for_expension.empty())
    {
      halfedge_descriptor h = stack_for_expension.back();
      stack_for_expension.pop_back();
      if(cc_faces.count(face(h, tmesh)) == 1)
        continue;

      if(do_overlap(bb, get(vpm, target(next(h, tmesh), tmesh)).bbox()))
      {
        cc_faces.insert(face(h, tmesh));
        halfedge_descriptor candidate = opposite(next(h, tmesh), tmesh);
        if(face(candidate, tmesh) != boost::graph_traits<TriangleMesh>::null_face())
          stack_for_expension.push_back(candidate);

        candidate = opposite(prev(h, tmesh), tmesh);
        if(face(candidate, tmesh) != boost::graph_traits<TriangleMesh>::null_face())
          stack_for_expension.push_back(candidate);
      }
    }

    Boolean_property_map<std::set<face_descriptor> > is_selected(cc_faces);
    expand_face_selection_for_removal(cc_faces, tmesh, is_selected);

#ifdef CGAL_PMP_REMOVE_SELF_INTERSECTION_DEBUG
    std::cout << "  DEBUG: " << cc_faces.size() << " faces in expanded and compactified CC\n";
#endif

#ifdef CGAL_PMP_REMOVE_SELF_INTERSECTION_OUTPUT
    fname = "results/expanded_compactified_r_"+std::to_string(call_id)+"_CC_"+std::to_string(cc_id)+"_step_"+std::to_string(step)+".off";
    dump_cc(fname, cc_faces, tmesh, vpm);
#endif

    if(only_treat_self_intersections_locally)
    {
      if(!does_self_intersect(cc_faces, tmesh, parameters::vertex_point_map(vpm).geom_traits(gt)))
      {
#ifdef CGAL_PMP_REMOVE_SELF_INTERSECTION_DEBUG
        std::cout << "  DEBUG: No self-intersection in CC\n";
#endif

        for(const face_descriptor f : cc_faces)
          faces_to_remove.erase(f);

        continue;
      }
    }

    // remove faces from the set to process
    for(const face_descriptor f : cc_faces)
      faces_to_remove.erase(f);

    // Collect halfedges on the boundary of the region to be selected
    // (incident to faces that are part of the CC)
    std::vector<halfedge_descriptor> cc_border_hedges;
    for(face_descriptor fd : cc_faces)
    {
      for(halfedge_descriptor h : halfedges_around_face(halfedge(fd, tmesh), tmesh))
      {
        if(is_border(opposite(h, tmesh), tmesh) || cc_faces.count(face(opposite(h, tmesh), tmesh)) == 0)
          cc_border_hedges.push_back(h);
      }
    }

    if(cc_faces.size() == 1) // it is a triangle, thus nothing better can be done
    {
#ifdef CGAL_PMP_REMOVE_SELF_INTERSECTION_DEBUG
     ++unsolved_self_intersections;
#endif
      visitor.end_component_handling();
      continue;
    }

    working_face_range.insert(cc_faces.begin(), cc_faces.end());

    // Now, we have a proper selection that we can work on.

#ifndef CGAL_PMP_REMOVE_SELF_INTERSECTION_NO_POLYHEDRAL_ENVELOPE_CHECK
    Polyhedral_envelope<GeomTraits> cc_envelope;
    if (containment_epsilon!=0)
      cc_envelope = Polyhedral_envelope<GeomTraits>(cc_faces, tmesh, containment_epsilon);
#else
    struct Return_true {
      bool is_empty() const { return true; }
      bool operator()(const std::vector<std::vector<typename GeomTraits::Point_3> >&) const { return true; }
      bool operator()(const TriangleMesh&) const { return true; }
    };

    Return_true cc_envelope;
    CGAL_USE(containment_epsilon);
#endif

#ifndef CGAL_PMP_REMOVE_SELF_INTERSECTIONS_NO_SMOOTHING
    // First, try to smooth if we only care about local self-intersections
    // Two different approaches:
    // - First, try to constrain edges that are in the zone to smooth and whose dihedral angle is large,
    //   but not too large (we don't want to constrain edges that are foldings);
    // - If that fails, try to smooth without any constraints, but make sure that the deviation from
    //   the first zone is small.
    //
    // If smoothing fails, the face patch is restored to its pre-smoothing state.
    //
    // Note that there is no need to update the working range because smoothing doesn`t change
    // the number of faces (and old faces are re-used).
    bool fixed_by_smoothing = false;

    if(only_treat_self_intersections_locally)
    {
      fixed_by_smoothing = remove_self_intersections_with_smoothing(cc_faces, tmesh, true /*constrain_sharp_edges*/,
                                                                    strong_dihedral_angle, weak_dihedral_angle,
                                                                    cc_envelope, vpm, gt);

      if(!fixed_by_smoothing) // try again, but without constraining sharp edges
      {
 #ifdef CGAL_PMP_REMOVE_SELF_INTERSECTION_DEBUG
        std::cout << "  DEBUG: Could not be solved via smoothing with constraints\n";
 #endif

        fixed_by_smoothing = remove_self_intersections_with_smoothing(cc_faces, tmesh, false /*constrain_sharp_edges*/,
                                                                      strong_dihedral_angle, weak_dihedral_angle,
                                                                      cc_envelope, vpm, gt);
      }
    }

    if(fixed_by_smoothing)
    {
 #ifdef CGAL_PMP_REMOVE_SELF_INTERSECTION_DEBUG
      std::cout << "  DEBUG: Solved with smoothing!\n";
 #endif

      something_was_done = true;
      visitor.end_component_handling();
      continue;
    }
 #ifdef CGAL_PMP_REMOVE_SELF_INTERSECTION_DEBUG
    else
    {
      std::cout << "  DEBUG: Could not be solved via smoothing\n";
    }
 #endif
#endif // ndef CGAL_PMP_REMOVE_SELF_INTERSECTIONS_NO_SMOOTHING

#ifdef CGAL_PMP_REMOVE_SELF_INTERSECTION_DEBUG
    std::cout << "  DEBUG: Trying hole-filling based approach...\n";
#endif

    int selection_chi = euler_characteristic_of_selection(cc_faces, tmesh);
    if(selection_chi != 1) // not a topological disk
    {
      if(!handle_CC_with_complex_topology(cc_border_hedges, cc_faces, working_face_range,
                                          tmesh, only_treat_self_intersections_locally,
                                          strong_dihedral_angle, weak_dihedral_angle,
                                          preserve_genus, cc_envelope, vpm, gt))
      {
#ifdef CGAL_PMP_REMOVE_SELF_INTERSECTION_DEBUG
        ++unsolved_self_intersections;
#endif
        topology_issue = true;
        all_fixed = false;
      }
      else
      {
        something_was_done = true;
      }

      visitor.end_component_handling();
      continue;
    }

    // From here on, the CC is a topological disk
    if(!remove_self_intersections_with_hole_filling(cc_border_hedges, cc_faces, working_face_range,
                                                    tmesh, only_treat_self_intersections_locally,
                                                    strong_dihedral_angle, weak_dihedral_angle,
                                                    cc_envelope, vpm, gt))
    {
#ifdef CGAL_PMP_REMOVE_SELF_INTERSECTION_DEBUG
      std::cout << "  DEBUG: Failed to fill hole\n";
      ++unsolved_self_intersections;
#endif

      all_fixed = false;
    }
    else
    {
      something_was_done = true;
    }
    visitor.end_component_handling();
  }

  if(!something_was_done)
  {
    faces_to_remove.swap(faces_to_remove_copy);
#ifdef CGAL_PMP_REMOVE_SELF_INTERSECTION_DEBUG
    std::cout << "  DEBUG: Nothing was changed during this step, self-intersections won`t be recomputed." << std::endl;
#endif
  }

#ifdef CGAL_PMP_REMOVE_SELF_INTERSECTION_OUTPUT
  std::stringstream oss;
  oss << "results/after_step_" << step << ".off" << std::ends;
  std::ofstream(oss.str().c_str()) << std::setprecision(17) << tmesh;
#endif

  return std::make_pair(all_fixed, topology_issue);
}

} // namespace internal

namespace experimental {

template <class TriangleMesh>
struct Remove_self_intersection_default_visitor
{
  bool stop() const { return false; }
  template <class FaceContainer>
  void status_update(const FaceContainer&) {}
  void start_main_loop() {}
  void end_main_loop() {}
  void start_iteration() {}
  void end_iteration() {}
  void start_component_handling() {}
  void end_component_handling() {}
  void parameters_used( bool /* parameters_used(preserve_genus */,
                        bool /* only_treat_self_intersections_locally */,
                        int /* max_steps */,
                        double /* strong_dihedral_angle */,
                        double /* weak_dihedral_angle */,
                        double /* containment_epsilon */ ) {}
};

template <typename FaceRange, typename TriangleMesh, typename NamedParameters>
bool remove_self_intersections(const FaceRange& face_range,
                               TriangleMesh& tmesh,
                               const NamedParameters& np)
{
  using parameters::choose_parameter;
  using parameters::get_parameter;

  typedef boost::graph_traits<TriangleMesh>                                 graph_traits;
  typedef typename graph_traits::face_descriptor                            face_descriptor;

  // named parameter extraction
  typedef typename GetVertexPointMap<TriangleMesh, NamedParameters>::type   VertexPointMap;
  VertexPointMap vpm = choose_parameter(get_parameter(np, internal_np::vertex_point),
                                        get_property_map(vertex_point, tmesh));

  typedef typename GetGeomTraits<TriangleMesh, NamedParameters>::type       GeomTraits;
  GeomTraits gt = choose_parameter<GeomTraits>(get_parameter(np, internal_np::geom_traits));

  bool preserve_genus = choose_parameter(get_parameter(np, internal_np::preserve_genus), true);
  const bool only_treat_self_intersections_locally = choose_parameter(get_parameter(np, internal_np::apply_per_connected_component), false);

  // When treating intersections locally, we don't want to grow the working range too much as
  // either the solution is found fast, or it's too difficult and neither local smoothing or local
  // hole filling are going to provide nice results.
  const int default_max_step = only_treat_self_intersections_locally ? 2 : 7;
  const int max_steps = choose_parameter(get_parameter(np, internal_np::number_of_iterations), default_max_step);

  // @fixme give it its own named parameter rather than abusing 'with_dihedral_angle'?
  const double strong_dihedral_angle = choose_parameter(get_parameter(np, internal_np::with_dihedral_angle), 60.);

  // detect_feature_pp NP (unused for now)
  const double weak_dihedral_angle = 0.; // choose_parameter(get_parameter(np, internal_np::weak_dihedral_angle), 20.);

  struct Return_false {
    bool operator()(std::pair<face_descriptor, face_descriptor>) const { return false; }
  };

  typedef typename internal_np::Lookup_named_param_def <
    internal_np::filter_t,
    NamedParameters,
    Return_false//default
  > ::type  Output_iterator_predicate;
  Output_iterator_predicate out_it_predicates
    = choose_parameter<Return_false>(get_parameter(np, internal_np::filter));

  // use containment check
  const double containment_epsilon = choose_parameter(get_parameter(np, internal_np::polyhedral_envelope_epsilon), 0.);

#ifdef CGAL_PMP_REMOVE_SELF_INTERSECTION_DEBUG
  std::cout << "DEBUG: Starting remove_self_intersections, is_valid(tmesh)? " << is_valid_polygon_mesh(tmesh) << "\n";
  std::cout << "\tpreserve_genus: " << preserve_genus << std::endl;
  std::cout << "\tonly_treat_self_intersections_locally: " << only_treat_self_intersections_locally << std::endl;
  std::cout << "\tmax_steps: " << max_steps << std::endl;
  std::cout << "\tstrong_dihedral_angle: " << strong_dihedral_angle << std::endl;
  std::cout << "\tweak_dihedral_angle: " << weak_dihedral_angle << std::endl;
  std::cout << "\tcontainment_epsilon: " << containment_epsilon << std::endl;
#endif

  typedef typename internal_np::Lookup_named_param_def <
    internal_np::visitor_t,
    NamedParameters,
    Remove_self_intersection_default_visitor<TriangleMesh>//default
  > ::type Visitor;
  Visitor visitor = choose_parameter<Visitor>(get_parameter(np, internal_np::visitor));

  visitor.parameters_used(preserve_genus,
                          only_treat_self_intersections_locally,
                          max_steps,
                          strong_dihedral_angle,
                          weak_dihedral_angle,
                          containment_epsilon);

  if(!preserve_genus)
    duplicate_non_manifold_vertices(tmesh, np);

  // Look for self-intersections in the mesh and remove them
  int step = -1;
  bool all_fixed = true; // indicates if the filling of all created holes went fine
  bool topology_issue = false; // indicates if some boundary cycles of edges are blocking the fixing
  std::set<face_descriptor> faces_to_remove;
  std::set<face_descriptor> working_face_range(face_range.begin(), face_range.end());

  visitor.start_main_loop();
  while(++step < max_steps)
  {
    if (visitor.stop()) break;
    visitor.start_iteration();

    if(faces_to_remove.empty()) // the previous round might have been blocked due to topological constraints
    {
      typedef std::pair<face_descriptor, face_descriptor> Face_pair;
      std::vector<Face_pair> self_inter;

      // TODO : possible optimization to reduce the range to check with the bbox
      // of the previous patches or something.
      self_intersections(working_face_range, tmesh,
                         CGAL::filter_output_iterator(std::back_inserter(self_inter), out_it_predicates));
#ifdef CGAL_PMP_REMOVE_SELF_INTERSECTION_DEBUG
      std::cout << "  DEBUG: " << self_inter.size() << " intersecting pairs" << std::endl;
#endif
      for(const Face_pair& fp : self_inter)
      {
        faces_to_remove.insert(fp.first);
        faces_to_remove.insert(fp.second);
      }
    }

    if(faces_to_remove.empty() && all_fixed)
    {
#ifdef CGAL_PMP_REMOVE_SELF_INTERSECTION_DEBUG
      std::cout << "DEBUG: There are no more faces to remove." << std::endl;
#endif
      break;
    }

    visitor.status_update(faces_to_remove);

    std::tie(all_fixed, topology_issue) =
      internal::remove_self_intersections_one_step(
          faces_to_remove, working_face_range, tmesh,
          step, preserve_genus, only_treat_self_intersections_locally,
          strong_dihedral_angle, weak_dihedral_angle, containment_epsilon, vpm, gt, visitor);

#ifdef CGAL_PMP_REMOVE_SELF_INTERSECTION_DEBUG
    if(all_fixed && topology_issue)
        std::cout << "DEBUG: boundary cycles of boundary edges involved in self-intersections.\n";
#endif

    visitor.end_iteration();
  }
  visitor.end_main_loop();

#ifdef CGAL_PMP_REMOVE_SELF_INTERSECTION_DEBUG
  std::cout << "solved by constrained smoothing: " << internal::self_intersections_solved_by_constrained_smoothing << std::endl;
  std::cout << "solved by unconstrained smoothing: " << internal::self_intersections_solved_by_unconstrained_smoothing << std::endl;
  std::cout << "solved by constrained hole-filling: " << internal::self_intersections_solved_by_constrained_hole_filling << std::endl;
  std::cout << "solved by unconstrained hole-filling: " << internal::self_intersections_solved_by_unconstrained_hole_filling << std::endl;
  std::cout << "unsolved: " << internal::unsolved_self_intersections << std::endl;
#endif

#ifdef CGAL_PMP_REMOVE_SELF_INTERSECTION_OUTPUT
  std::ofstream("results/final.off") << std::setprecision(17) << tmesh;
#endif

  return step < max_steps;
}

template <typename FaceRange, typename TriangleMesh>
bool remove_self_intersections(const FaceRange& face_range, TriangleMesh& tmesh)
{
  return remove_self_intersections(face_range, tmesh, parameters::all_default());
}

template <typename TriangleMesh, typename CGAL_PMP_NP_TEMPLATE_PARAMETERS>
bool remove_self_intersections(TriangleMesh& tmesh, const CGAL_PMP_NP_CLASS& np)
{
  return remove_self_intersections(faces(tmesh), tmesh, np);
}

template <typename TriangleMesh>
bool remove_self_intersections(TriangleMesh& tmesh)
{
  return remove_self_intersections(tmesh, parameters::all_default());
}

} // namespace experimental
} // namespace Polygon_mesh_processing
} // namespace CGAL

#endif // CGAL_POLYGON_MESH_PROCESSING_REPAIR_SELF_INTERSECTIONS_H
