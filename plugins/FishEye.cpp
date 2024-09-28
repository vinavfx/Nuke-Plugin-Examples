// FishEye.cpp
// Copyright (c) 2013 The Foundry Visionmongers Ltd.  All Rights Reserved.

#include "DDImage/CameraOp.h"
#include "DDImage/Format.h"
#include "DDImage/Knobs.h"
#include "DDImage/plugins.h"
#include "DDImage/Scene.h"

using namespace DD::Image;
using namespace std;

// The following example show how to write a camera Node with custom projection for Nuke "Scanline Render" Node.
// In particular the example try to simulate a no-linear projection: fish eye.
//
// The scanline render algorithm is mainly designed for linear projection. In this case polygons are properly clipped and rendered.
// In no-linear projection is not guarantee a correct rendering of primitives that are partially visible or completely
// invisible.
// To have a camera with custom projection, "lensNfunction" and "projection_is_linear" need to be overridden by CameraOp subclass.
// The "lensNfunction" return a pointer to a static function that has the purpose to transform vertex from object space
// in screen space.
// The "projection_is_linear" return a boolean value. With no-linear projection, "Scanline Render" force to tessellate
// uniformly all "polygons" present in the scene. That help the render quality for polygon particularly distorted by the nature of the no-linear
// projection.
// The knob scanlinerender.max_tesselation is used to control the level of the tessellation.
// 
// In general, when the overridden projection can be expressed by 4x4 Matrix it's enough to override the function "CameraOp::projection(mode)" 
// In this case Nuke OpenGL preview window will properly referring to the correct camera projection.

//! FishEye camera
class FishEye : public CameraOp
{

#if ((DD_IMAGE_VERSION_MAJOR >= 7) && (DD_IMAGE_VERSION_MINOR >= 1))
  enum { FISH_EYE_CAMERA = LENS_USER_CAMERA };
#else 
  enum { FISH_EYE_CAMERA = LENS_SPHERICAL };
#endif

  // This function is used by scanline render to transform an array
  // of vertex from "object" space in "screen" space.
  // It is called multiple time, according to the number of polygons
  // and to the number of rendering scene.
  // For motion blur/depth_of_field/super_sampling purpose "Scanline Render" can create many scene
  // each with a different camera context.
  // A scene has to store all rendering primitives that needs to be rendered properly
  // transformed in screen space.
  // The input parameter have the following meaning:
  // scene -  the rendering scene for a particular context
  // cam   -  the rendering camera for a particular context
  // transforms - is an array of matrix that can be query to have the proper transformation
  //              between different space.
  // v - is a pointer of an array of "rendering vertex" that needs to be transformed in screen space.
  //     In the purpose of this example the important component of the "rendering vertex" are
  //     v[i].PL() represent a vertex in object local coordinate
  //     v[i].P()  represent a vertex in screen coordinate
  // n - the number of vertex to transform
  // data - internal use

  static void FishEye_project_array( 
    Scene* scene, 
    CameraOp* cam, 
    MatrixArray* transforms, 
    VArray* v, 
    int n, 
    void* data )
  {
    FishEye* fisheye = static_cast<FishEye*>(cam);
    float fov = radians(fisheye->fov()) * 0.5f;
    bool fullframe = fisheye->fullframe();

    // for all points that needs to be transformed
    for (int i = 0; i < n; i++) {

      // transform the point in camera coordinate
      Vector4 Pe = transforms->matrix(LOCAL_TO_EYE).transform(v[i].PL(), 1);

      // get the direction from camera center to point
      Vector3 Pn(Pe.x, Pe.y, Pe.z);
      float d = Pn.normalize();

      float theta = acos(-Pn.z);
      float r = theta / fov;
      float sin_theta = sin(theta);

      float s = (sin_theta != 0.0f) ? Pn.x * r / sin_theta : 0.0f; 
      float t = (sin_theta != 0.0f) ? Pn.y * r / sin_theta : 0.0f;

      float z = (2.0f*(sqrt(d)-cam->Near()) / (cam->Far() - cam->Near()));     

      Vector4 Pc(s*d, t*d, z*d, d);

      // Note: Pc.w it's the value that directly influence the output in 
      // the Scanline render depth channel. The depth channel it will be stored as inverse of w

      if (fullframe) {
        Pc.x *= M_SQRT2_F;
        Pc.y *= M_SQRT2_F;
      }

      // from clip space to screen:
      v[i].P() = transforms->matrix(CLIP_TO_SCREEN).transform(Pc);
    }
  }

  // fish eye camera field of view
  float _fov;
  bool _fullframe;

public:
  //! This is the class constructor.
  FishEye(Node* node) : CameraOp(node)
  {
    projection_mode_ = FISH_EYE_CAMERA;
    _fov = 90.0f;
    _fullframe = false;
  }

  //! This is the class destructor.
  ~FishEye() override
  {}

  float fov() const 
  {  
    return _fov;
  }

  bool fullframe() const 
  {
    return _fullframe;
  }

  //! This specifies the knobs.
  void knobs(Knob_Callback f) override
  {
    Float_knob(f, &_fov, IRange(10, 180), "fov", "fov");
    Bool_knob(f, &_fullframe, "full_frame", "full frame");
    Divider( f, "" );
    AxisOp::knobs(f);
  }

  //! This override the camera projection
  LensNFunc* lensNfunction(int mode) const override
  {
    if (mode == FISH_EYE_CAMERA)
      return &FishEye_project_array;

    return CameraOp::lensNfunction(mode); 
  }

#if ((DD_IMAGE_VERSION_MAJOR >= 7) && (DD_IMAGE_VERSION_MINOR >= 1))

  //! This return if the projection is linear
  bool projection_is_linear(int mode) const override
  {
    if (mode == FISH_EYE_CAMERA)
      return false;
    return CameraOp::projection_is_linear(mode);
  }
#endif

  //! This is the node help.
  const char* node_help() const override
  {
    return "FishEye camera";
  }

  //! This is the node display name
  const char* displayName() const override
  {
    return "FishEye";
  }

  const char* Class() const override;

  static const Description description;

};

static Op* FishEyeConstructor(Node* node)
{
  return new FishEye(node);
}

const Op::Description FishEye::description("FishEye", "FishEye", FishEyeConstructor);

const char* FishEye::Class() const
{
  return description.name;
}
