#include "DDImage/Attribute.h"
#include "DDImage/GeometryList.h"
#include "DDImage/GeoWriter.h"
#include "DDImage/Scene.h"
#include "DDImage/Triangle.h"
#include "DDImage/Vector3.h"
#include "DDImage/Vector4.h"

#include <cstdio>

using namespace DD::Image;

namespace my3d {

  class my3dWriter : public GeoWriter
  {
  public:
    my3dWriter(WriteGeo* writeNode);
    virtual void execute(Scene& scene);

    static GeoWriter* Build(WriteGeo* readNode);

    static GeoWriter::Description description;
  };


  //
  // my3dReader methods
  //
  
  my3dWriter::my3dWriter(WriteGeo* writeNode) :
    GeoWriter(writeNode)
  {
  }

  
  void my3dWriter::execute(Scene& scene)
  {
    // If we can't open the file for writing, show an error message and abort.
    if (!open()) {
      geo->critical("my3dWriter: failed to open geometry file for writing");
      return;
    }

    // Write the header.
    FILE* f = (FILE*)file;
    fprintf(f, "{my3d}\n");

    // Loop over all objects and write out a point for the corner of every Triangle primitive.
    GeometryList* objects = scene.object_list();
    for (unsigned int obj = 0; obj < objects->size(); ++obj) {
      GeoInfo& info = objects->object(obj);
      const PointList* points = info.point_list();
      const Attribute* normals = info.get_typed_group_attribute(Group_Primitives, kNormalAttrName, NORMAL_ATTRIB);
      const Attribute* texCoords = info.get_typed_group_attribute(Group_Points, kUVAttrName, VECTOR4_ATTRIB);

      for (unsigned int p = 0; p < info.primitives(); ++p) {
        const Triangle* triangle = dynamic_cast<const Triangle*>(info.primitive(p));
        if (!triangle)
          continue;
        
        unsigned int corner = triangle->vertex(0);
        Vector3 pos = (*points)[corner];
        Vector3 normal = normals->normal(p);
        Vector4 uv = texCoords->vector4(corner);

        unsigned int corner2 = triangle->vertex(1);
        Vector3 edge = (*points)[corner2] - pos;
        float size = edge.length();

        fprintf(f, "%f %f %f %f %f %f %f %f %f\n", size,
                   pos.x, pos.y, pos.z,
                   normal.x, normal.y, normal.z,
                   uv.x, uv.y);
      }
    }

    close();
  }


  //
  // my3dReader static methods
  //

  GeoWriter* my3dWriter::Build(WriteGeo* writeNode)
  {
    return new my3dWriter(writeNode);
  }


  //
  // my3dReader static variables
  //

  GeoWriter::Description my3dWriter::description("my3d\0", my3dWriter::Build);

} // namespace my3d

