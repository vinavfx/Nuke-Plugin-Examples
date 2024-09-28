#include <cassert>

#include <DDImage/Knobs.h>
#include <DDImage/SourceGeo.h>
#include <DDImage/Triangle.h>

using namespace DD::Image;

namespace tri 
{
  //
  // Constants
  //

  static const char* kTriangleClass = "Triangle";


  //
  // Declarations
  //

  class TriangleOp : public SourceGeo 
  {
    public:
      TriangleOp(Node* node);

      virtual const char* Class() const;
      virtual void knobs(Knob_Callback f);

      static Op* Build(Node* node);

      static const Op::Description description;

    protected:
      virtual void create_geometry(Scene& scene, GeometryList& out);
      virtual void get_geometry_hash();

    private:
      Vector3 _aPos; // Position of corner A of the triangle
      Vector3 _bPos; // Position of corner B of the triangle
      Vector3 _cPos; // Position of corner C of the triangle
  };


  //
  // Implementations
  //

  TriangleOp::TriangleOp(Node* node) :
    SourceGeo(node),
    _aPos(0, 0, 0),
    _bPos(1, 0, 0),
    _cPos(0, 1, 0)
  {}


  const char* TriangleOp::Class() const
  {
    return kTriangleClass;
  }


  void TriangleOp::knobs(Knob_Callback f) {
    SourceGeo::knobs(f); // Set up the common SourceGeo knobs.
    XYZ_knob(f, &_aPos[0], "point_a", "a");
    XYZ_knob(f, &_bPos[0], "point_b", "b");
    XYZ_knob(f, &_cPos[0], "point_c", "c");
  }


  Op* TriangleOp::Build(Node* node)
  {
    return new TriangleOp(node);
  }


  void TriangleOp::create_geometry(Scene& scene, GeometryList& out)
  {
    int obj = 0;

    if (rebuild(Mask_Primitives)) {
      out.delete_objects();
      out.add_object(obj);
      out.add_primitive(obj, new Triangle());
    }

    if (rebuild(Mask_Points)) {
      PointList& points = *out.writable_points(obj);
      points.resize(3);

      points[0] = _aPos;
      points[1] = _bPos;
      points[2] = _cPos;
    }

    if (rebuild(Mask_Attributes)) {
      Attribute* uv = out.writable_attribute(obj, Group_Vertices, "uv", VECTOR4_ATTRIB);
      assert(uv != NULL);
      uv->vector4(0).set(0, 0, 0, 1);
      uv->vector4(1).set(1, 0, 0, 1);
      uv->vector4(2).set(0, 1, 0, 1);
    }
  }


  void TriangleOp::get_geometry_hash()
  {
    SourceGeo::get_geometry_hash();

    // Add the three point positions to the hash for the Points group.
    _aPos.append(geo_hash[Group_Points]);
    _bPos.append(geo_hash[Group_Points]);
    _cPos.append(geo_hash[Group_Points]);
  }


  const Op::Description TriangleOp::description(kTriangleClass, TriangleOp::Build);

} // namespace tri

