#include "DDImage/Attribute.h"
#include "DDImage/GeometryList.h"
#include "DDImage/GeoReader.h"
#include "DDImage/GeoReaderDescription.h"
#include "DDImage/Triangle.h"
#include "DDImage/Vector3.h"

#include <iostream>
#include <fstream>
#include <string>

using namespace DD::Image;

namespace my3d {

  // If you wish to include custom knobs in your 3d reader, you also need to implement a
  // GeoReaderFormat for your reader. Below is a simple example of how to achieve this.
  class my3dReaderFormat : public GeoReaderFormat
  {
    friend class my3dReader;

  private:

    // The following variable receives values from the reader-specific knob
    bool        _readOnEachFrame;

  public:

    my3dReaderFormat();
    virtual ~my3dReaderFormat() {};

    static GeoReaderFormat* buildformat(ReadGeo* iop);

    // GeoReaderFormat
    virtual void knobs(Knob_Callback c);
    virtual void extraKnobs(Knob_Callback c);
    virtual void append(Hash& hash);
    //~GeoReaderFormat
  };

  class my3dReader : public GeoReader
  {
  public:
    my3dReader(ReadGeo* readNode);

    // DD::Image::GeoReader
    virtual int knob_changed(Knob* k);
    virtual void append(Hash& newHash);
    virtual void get_geometry_hash(Hash* geo_hash);
    virtual void _validate(const bool for_real);
    virtual void geometry_engine(Scene& scene, GeometryList& out);
    // DD::Image::GeoReader

    static GeoReader* Build(ReadGeo* readNode, int fd, const unsigned char* buf, int bufSize);
    static bool Test(int fd, const unsigned char* buf, int bufSize);

    static GeoReader::Description description;

  private:

    // Used to store a pointer to the read node
    ReadGeo*    _pReadGeo;
  };

  //
  // my3dReaderFormat methods
  //

  my3dReaderFormat::my3dReaderFormat() :
    _readOnEachFrame(false)
  {
  }

  void my3dReaderFormat::knobs(Knob_Callback f)
  {
    // This places knobs in the format specific area of the main tab
    Bool_knob(f, &_readOnEachFrame, "read_on_each_frame", "read on each frame");
    Tooltip(f, "Activate this to read the objects on each frame. This should be activated for animated objects.");
  }

  void my3dReaderFormat::extraKnobs(Knob_Callback f)
  {
    // This places knobs after all other knobs
    // (For example, the "SceneGraph" tab knob and sceneView knob in the abcReader)
  }

  void my3dReaderFormat::append(Hash& new_hash)
  {
    new_hash.append(_readOnEachFrame);
  }

  /*! Build the my3d file interface
   */
  /*static*/ GeoReaderFormat* my3dReaderFormat::buildformat(ReadGeo* iop)
  {
    return new my3dReaderFormat();
  }

  //
  // my3dReader methods
  //
  
  my3dReader::my3dReader(ReadGeo* readNode) :
    GeoReader(readNode)
  , _pReadGeo(readNode)
  {
  }

  int my3dReader::knob_changed(Knob* k)
  {
    // Perform tasks based on knob_changed events from the base ReadGeo class or the associated GeoReaderFormat
    return true;
  }

  void my3dReader::append(Hash& newHash)
  {
    // Append any local variables to the hash in order to invalidate the op when they change.
    // Get the read_on_each_frame knob from the reader format
    Knob* myKnob =  _pReadGeo->knob("read_on_each_frame");

    newHash.append(myKnob->get_value());
}

  void my3dReader::get_geometry_hash(Hash* geo_hash)
  {
    // Use this function to invalidate different aspects of the geometry. e.g.
    //    geo_hash[Group_Primitives].append(myLocalVariable1);
    //    geo_hash[Group_Matrix].append(myLocalVariable2);
  }
  
  void my3dReader::_validate(const bool for_real)
  {
    // Use this to do any pre-render setup code.
  }

  void my3dReader::geometry_engine(Scene& scene, GeometryList& out)
  {
    const unsigned int kMaxLineLen = 4096;

    // Open the file
    std::ifstream in(filename());
    
    // Skip over the header.
    in.ignore(kMaxLineLen, '\n');

    int obj = 0;
    out.add_object(obj);
    PointList* points = out.writable_points(obj);
    Attribute* normals = out.writable_attribute(obj, Group_Primitives, kNormalAttrName, NORMAL_ATTRIB);
    Attribute* texCoords = out.writable_attribute(obj, Group_Points, kUVAttrName, VECTOR4_ATTRIB);

    Vector3 pos, normal;
    Vector4 uv(0, 0, 0, 1);
    float size;
    int n = 0;
    while (in.good()) {
      in >> size >> pos.x >> pos.y >> pos.z >> normal.x >> normal.y >> normal.z >> uv.x >> uv.y;
      in.ignore(kMaxLineLen, '\n'); // skip any trailing characters.
      
      out.add_primitive(obj, new Triangle(n  * 3, n * 3 + 1, n * 3 + 2));

      Vector3 v1 = normal.cross(Vector3(1, 0, 0));
      Vector3 v2 = v1.cross(normal);
      v1.normalize();
      v2.normalize();
      points->push_back(pos);
      points->push_back(pos + (v1 * size));
      points->push_back(pos + (v2 * size));

      normals->add(1);
      normals->normal(n) = normal;

      texCoords->add(3);
      texCoords->vector4(n * 3) = uv;
      texCoords->vector4(n * 3 + 1) = uv + Vector4(size, 0, 0, 0);
      texCoords->vector4(n * 3 + 2) = uv + Vector4(0, size, 0, 0);

      ++n;
    }
  }


  //
  // my3dReader static methods
  //

  GeoReader* my3dReader::Build(ReadGeo* readNode, int fd, const unsigned char* buf, int bufSize)
  {
    return new my3dReader(readNode);
  }


  bool my3dReader::Test(int fd, const unsigned char* buf, int bufSize)
  {
    const char* kHeader = "{my3d}";
    const size_t kHeaderLen = strlen(kHeader);

    // Check that the buffer starts with our header.
    return (bufSize >= (int)kHeaderLen && strncmp(kHeader, (const char*)buf, kHeaderLen) == 0);
  }


  //
  // my3dReader static variables
  //

  // if your reader implements a GeoReaderFormat, use this implementation of description:
  /*! Build the my3d file interface
   */
  static GeoReader* build(ReadGeo* op, int fd, const unsigned char* b, int n)
  {
    return new my3dReader(op);
  }

  GeoReader::Description my3dReader::description("my3d\0", build, my3dReaderFormat::buildformat, my3dReader::Test);

  // Otherwise, use this implementation
  // GeoReader::Description my3dReader::description("my3d\0", my3dReader::Build, my3dReader::Test);

} // namespace my3d

