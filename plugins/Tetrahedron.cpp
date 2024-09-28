// Copyright (c) 2021 The Foundry Visionmongers Ltd.  All Rights Reserved.

#include "DDImage/DDMath.h"
#include "DDImage/Knobs.h"
#include "DDImage/PolyMesh.h"
#include "DDImage/SourceGeo.h"

using namespace DD::Image;

// This example shows how to create a Tetrahedron 3D object able to be rendered
// in a 3D scene with proper UV mapping, proper normals representation and
// transformable via Axis_knob transform handles and vertices handles.
//
// # 3D coordinates Math explanation for Tetrahedron
//
// The Tetrahedron is circumscribed into a sphere of radius 1, so the
// distance between the world center and each vertices is exactly 1. Each
// face is a equilateral triangle.
//
// # '3D' view for x and y axis. Frontal view.
//                   y ^
//                     |
// 1 ................. ^p3
//                    /|\
//                   / | \
// 0                /  |  \
//  <-----------.--/---+---\--.-------------->
//              . /    |    \ .
// -sin(30)....../_____|_____\.
//              .p0    |      .p2
//           -cos(30)  0     cos(30)          x
//
// # '3D' view for x and z axis. Top view of the base of the object.
//
//                   z ^
//             p0      |     p2
// sin(30)..... .------+------.
//              .\     |     /.
//              . \    |    / .
//  <-----------+--\---+---/--+--------------->
//              .   \  |  /   .               x
//              .    \ | /    .
//              .     \|/     .
// -1 . . . . . . . . .v p1   .
//              .      |      .
//           -cos(30)  0   cos(30)
//
// ### UV mapping (2D-3D mapping)
//
// The following mapping describes how a 2D texture is mapped to the
// Tetrahedron.
//
// The 3D coordinates p[0-3] are mapped to faces 0-3 as the following
// representation shows:
//
// V ^
//   |            p33
// 2 |             ^                sqrt(3)/2
//   |            / \
//   |           /   \
//   |          /     \
//   |         /   3   \
//   |        /         \
//   |       /           \
// 1 |    p0^-------------^p2        sqrt(3)/4
//   |     / \           / \
//   |    /   \    0    /   \
//   |   /     \       /     \
//   |  /   1   \     /   2   \
//   | /         \   /         \
// 0 |/_____._____\./_____._____\
//   +----------------------------------------->
//   0      1      2      3      4    U
//   p31           p1            p32
//
//   p31, p32 and p33 represents how the same point p3 maps to form each
//   face 1, 2 and 3 respectively.
//
//   Given a 2D square image, the area matching the face 0 will be rendered
//   as the bottom of the Tetrahedron. The faces 1, 2 and 3 will each be
//   rendered as the sides.

namespace Tetra {
  static const char* kClassName = "Tetrahedron";
  static const char* kHelp = "Creates a 3D Tetrahedron";

  static const char* const kVertexLabels[4]{"p0", "p1", "p2", "p3"};
  static const char* const kAxisLabel = "transform";
  
  // U and V coordinates for texture mapping.
  static const float kU[5]{0.0f, 0.25f, 0.5f, 0.75f, 1.0f};
  static const float kV[3]{0.0f, sqrtf(3.0f) / 4.0f, sqrtf(3.0f) / 2.0f};

  // Tetrahedron 3D faces. Note that the base triangle k3dFaces[0] == {0, 2, 1}
  // makes the face normal point down, as expected. If we use {0, 1, 2} the
  // normal would be pointing up.
  static const int k3dFaces[4][3]{{0, 2, 1},  // p0, p2, p1
                                  {0, 1, 3},  // p0, p1, p31
                                  {1, 2, 3},  // p1, p2, p32
                                  {2, 0, 3}}; // p2, p0, p33

  // UV texture mapping. It is composed of {kU-index, kV-index} pairs, mapping
  // 2D to 3D coordinates for each of the faces of the Tetrahedron.
  static const int kUVMapping[12][2]{{1, 1}, {3, 1}, {2, 0}, // face 0
                                     {1, 1}, {2, 0}, {0, 0}, // face 1
                                     {2, 0}, {3, 1}, {4, 0}, // face 2
                                     {3, 1}, {1, 1}, {2, 2}};// face 3
  
  class Tetrahedron : public SourceGeo
  {
  public:
    static const Description kDescription;
    const char* Class() const override;
    const char* node_help() const override;
  
    explicit Tetrahedron(Node* node);

  protected:
    void get_geometry_hash() override;
    void create_geometry(Scene& scene, GeometryList& out) override;
    void geometry_engine(Scene& scene, GeometryList& out) override;
  
    void knobs(Knob_Callback callback) override;
  
  private:
    Vector3 _vertices[4];
    Matrix4 _localTransformMatrix;
  };
  
  Tetrahedron::Tetrahedron(Node* node)
    : SourceGeo(node)
  {
    static const float kRadians30 = radians(30);
    static const float kCos30 = cos(kRadians30);
    static const float kSin30 = sin(kRadians30);
    // Initial geometry vertices values.
    _vertices[0].set(-kCos30, -kSin30, kSin30);
    _vertices[1].set(kCos30, -kSin30, kSin30);
    _vertices[2].set(0, -kSin30, -1);
    _vertices[3].set(0, 1, 0);
    _localTransformMatrix.makeIdentity();
  }
  
  void Tetrahedron::get_geometry_hash()
  {
    // get_geometry_hash tells Nuke to rebuild the geometry if any of its
    // relevant data values changes. We are interested in rebuilding whenever
    // transformations or vertices change.
    SourceGeo::get_geometry_hash();

    for (auto& vertex : _vertices) {
      vertex.append(geo_hash[Group_Points]);
    }
  
    _localTransformMatrix.append(geo_hash[Group_Matrix]);
  }
  
  void Tetrahedron::create_geometry(Scene& scene, GeometryList& out)
  {
    int obj = 0;
  
    if (rebuild(Mask_Primitives)) {
      out.delete_objects();
      out.add_object(obj);
      // Constructs a PolyMesh containing the 4 faces and 4 vertices.
      auto mesh = new PolyMesh(4, 4);
      for (int i = 0; i < 4; ++i) {
        mesh->add_face(3, k3dFaces[i]);
      }
      out.add_primitive(obj, mesh);
    }
  
    if (rebuild(Mask_Points)) {
      PointList& points = *out.writable_points(obj);
      points.resize(4);
      // Add geometry points.
      for (int i = 0; i < 4; ++i) {
        points[i] = _vertices[i];
      }
    }
  
    if (rebuild(Mask_Attributes)) {
      // Add the UV mapping to allow rendering.
      Attribute* uv = out.writable_attribute(obj, Group_Vertices, "uv", VECTOR4_ATTRIB);
      assert(uv != nullptr);
      for (int i = 0; i < 12; ++i) {
        uv->vector4(i).set(kU[kUVMapping[i][0]], kV[kUVMapping[i][1]], 0, 1);
      }
    }
  }
  
  void Tetrahedron::geometry_engine(Scene& scene, GeometryList& out)
  {
    SourceGeo::geometry_engine(scene, out);
    // Apply Axis_knob transformation to the geometry.
    for (size_t i = 0; i < out.size(); ++i) {
      out[i].matrix = _localTransformMatrix * out[i].matrix;
    }
  }
  
  void Tetrahedron::knobs(Knob_Callback callback)
  {
    // Add default SourceGeo knobs, being 2 enumerations ("display" and
    // "render") and 3 checkboxes ("selectable", "cast shadow" and
    // "receive shadow".
    SourceGeo::knobs(callback);

    // Add Axis_knob to allow Move, Resize and Rotation transformations.
    auto axisKnob = Axis_knob(callback, &_localTransformMatrix, kAxisLabel);
  
    // Add handles to allow deforming the shape by grabbing and dragging vertices.
    for (int i = 0; i < 4; ++i) {
      auto knob = XYZ_knob(callback, &(_vertices[i].x), kVertexLabels[i]);
      knob->geoKnob()->setMatrixSource(axisKnob->axisKnob());
    }
  }
  
  static Op* build(Node* node) { return new Tetrahedron(node); }
  const Op::Description Tetrahedron::kDescription(kClassName, build);
  const char* Tetrahedron::Class() const { return kDescription.name; }
  const char* Tetrahedron::node_help() const { return kHelp; }
}
