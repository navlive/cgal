#include <iostream>
#include <fstream>

#include <CGAL/Exact_predicates_inexact_constructions_kernel.h>
#include <CGAL/Polygon_repair_2/Polygon_repair_2.h>
#include <CGAL/draw_polygon_2.h>

// #include <CGAL/IO/WKT.h>

typedef CGAL::Exact_predicates_inexact_constructions_kernel Kernel;
typedef Kernel::Point_2 Point;
typedef CGAL::Polygon_2<Kernel> Polygon;
typedef CGAL::Polygon_with_holes_2<Kernel> Polygon_with_holes;
typedef CGAL::Multipolygon_with_holes_2<Kernel> Multipolygon;
typedef CGAL::Polygon_repair_2::Polygon_repair_2<Kernel> Polygon_repair;

int main(int argc, char* argv[]) {
  // std::ifstream ifs( (argc==1)?"data/polygon.wkt":argv[1]);

  // Square
  // Point ps[] = {Point(0,0), Point(1,0), Point(1,1), Point(0,1)};
  // Polygon p(ps, ps+4);
  // Multipolygon mp;
  // mp.add_polygon(p);

  // Bowtie
  // Point ps[] = {Point(0,0), Point(1,1), Point(1,0), Point(0,1)};
  // Polygon p(ps, ps+4);
  // Multipolygon mp;
  // mp.add_polygon(p);

  // Overlapping edge
  // Point ps1[] = {Point(0,0), Point(1,0), Point(1,1), Point(0,1)};
  // Polygon p1(ps1, ps1+4);
  // Point ps2[] = {Point(1,0), Point(2,0), Point(2,1), Point(1,1)};
  // Polygon p2(ps2, ps2+4);
  // Multipolygon mp;
  // mp.add_polygon(p1);
  // mp.add_polygon(p2);

  // Edge partly overlapping (start)
  // Point ps1[] = {Point(0,0), Point(1,0), Point(1,1), Point(0,1)};
  // Polygon p1(ps1, ps1+4);
  // Point ps2[] = {Point(1,0), Point(2,0), Point(2,0.5), Point(1,0.5)};
  // Polygon p2(ps2, ps2+4);
  // Multipolygon mp;
  // mp.add_polygon(p1);
  // mp.add_polygon(p2);

  // Edge partly overlapping (middle)
  // Point ps1[] = {Point(0,0), Point(1,0), Point(1,1), Point(0,1)};
  // Polygon p1(ps1, ps1+4);
  // Point ps2[] = {Point(1,0.25), Point(2,0.25), Point(2,0.75), Point(1,0.75)};
  // Polygon p2(ps2, ps2+4);
  // Multipolygon mp;
  // mp.add_polygon(p1);
  // mp.add_polygon(p2);

  // Square with hole
  // Point ps1[] = {Point(0,0), Point(1,0), Point(1,1), Point(0,1)};
  // Polygon_with_holes p(Polygon(ps1, ps1+4));
  // Point ps2[] = {Point(0.25,0.25), Point(0.75,0.25), Point(0.75,0.75), Point(0.25,0.75)};
  // Polygon h(ps2, ps2+4);
  // p.add_hole(h);
  // Multipolygon mp;
  // mp.add_polygon(p);

  // Square with hole touching boundary
  // Point ps1[] = {Point(0,0), Point(1,0), Point(1,1), Point(0,1)};
  // Polygon_with_holes p(Polygon(ps1, ps1+4));
  // Point ps2[] = {Point(0.25,0.25), Point(0.75,0.25), Point(0.75,0.75), Point(0,1)};
  // Polygon h(ps2, ps2+4);
  // p.add_hole(h);
  // Multipolygon mp;
  // mp.add_polygon(p);

  // Square with hole touching boundary (self-intersecting loop)
  // Point ps[] = {Point(0,0), Point(1,0), Point(1,1), Point(0,1), 
  //               Point(0.25,0.25), Point(0.75,0.25), Point(0.75,0.75), Point(0,1)};
  // Polygon p(ps, ps+8);
  // std::cout << p << std::endl;
  // Multipolygon mp;
  // mp.add_polygon(p);

  Polygon_repair pr;
  pr.add_to_triangulation(mp);
  pr.label_triangulation();

  // for (auto const f: pr.triangulation().all_face_handles()) {
  //   std::cout << f->label() << " ";
  // } std::cout << std::endl;

  pr.reconstruct_multipolygon();
  std::cout << pr.multipolygon() << std::endl;


  // CGAL::Polygon_repair_2::repair(mp);

  return 0;
}
