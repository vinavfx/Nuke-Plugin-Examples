// Copyright (c) 2009 The Foundry Visionmongers Ltd.  All Rights Reserved.

/*! \class CornerPin2DIop
    Allows four points to fit an image to another in translation, rotaion and scale
 */

#include "DDImage/Convolve.h"
#include "DDImage/DDWindows.h"
#include "DDImage/Transform.h"
#include "DDImage/Format.h"
#include "DDImage/ViewerContext.h"
#include "DDImage/gl.h"
#include "DDImage/Knobs.h"

using namespace DD::Image;

static const char* const CLASS = "CornerPin2D";
static const char* const HELP = 
  "Allows four points to fit an image to another in translation, rotation and scale.";

struct xyStruct
{
  double x, y;
  bool enable;
};

class CornerPin2D : public Transform
{
  xyStruct sc[4];    // source coordinates
  xyStruct dc[4];    // destination coordinates
  
  bool  _inverted;

  float _extraMatrix[16];
  ConvolveArray _extraMatrixArray;

  // map 0,0,1,1 square to the four corners:
  void setCornerPinMatrix(xyStruct c[4], Matrix4& q)
  {
    q.makeIdentity();
    double dx3 = (c[0].x - c[1].x) + (c[2].x - c[3].x);
    double dy3 = (c[0].y - c[1].y) + (c[2].y - c[3].y);

    if (dx3 == 0 && dy3 == 0) {
      q.a00 = c[1].x - c[0].x;
      q.a01 = c[2].x - c[1].x;
      q.a03 = c[0].x;
      q.a10 = c[1].y - c[0].y;
      q.a11 = c[2].y - c[1].y;
      q.a13 = c[0].y;
    }
    else {
      double dx1 = c[1].x - c[2].x;
      double dy1 = c[1].y - c[2].y;
      double dx2 = c[3].x - c[2].x;
      double dy2 = c[3].y - c[2].y;
      double z = (dx1 * dy2 - dx2 * dy1);
      q.a30 = (dx3 * dy2 - dx2 * dy3) / z;
      q.a31 = (dx1 * dy3 - dx3 * dy1) / z;
      q.a00 = (c[1].x - c[0].x) + q.a30 * c[1].x;
      q.a01 = (c[3].x - c[0].x) + q.a31 * c[3].x;
      q.a03 = c[0].x;
      q.a10 = (c[1].y - c[0].y) + q.a30 * c[1].y;
      q.a11 = (c[3].y - c[0].y) + q.a31 * c[3].y;
      q.a13 = c[0].y;
    }
  }

  void setMatrix(const xyStruct sc[4], const xyStruct dc[4], Matrix4& matrix)
  {
    // pack the enabled points together into start of array:
    xyStruct sc2[4], dc2[4];
    int ix, cnt = 0;
    for (ix = 0; ix < 4; ix++) {
      if (sc[ix].enable) {
        // point is enabled
        sc2[cnt] = sc[ix];
        dc2[cnt] = dc[ix];
        cnt++;
      }
    }

    OutputContext oc = outputContext();
    Matrix4 fullToProxy, proxyToFull;
    fullToProxy.makeIdentity();
    proxyToFull.makeIdentity();
    oc.to_proxy(fullToProxy);
    oc.from_proxy(proxyToFull);
    proxyToFull.transpose();

    Matrix4 world(_extraMatrix);
    world.transpose();
    world = fullToProxy * world * proxyToFull;

    matrix.makeIdentity();
    if (cnt == 0) {
      matrix = world;
    }
    else if (cnt == 1) {
      //translate by a single point;
      Matrix4 t;
      t.translation(dc2[0].x - sc2[0].x, dc2[0].y - sc2[0].y);
      matrix = world * t;
    }
    else {
    // copy the last point to the unenabled points to get 4 of them:
      if (cnt == 2) {
        //create a third point
        sc2[2].x = sc2[0].x - (sc2[1].y - sc2[0].y);
        sc2[2].y = sc2[0].y + (sc2[1].x - sc2[0].x);

        dc2[2].x = dc2[0].x - (dc2[1].y - dc2[0].y);
        dc2[2].y = dc2[0].y + (dc2[1].x - dc2[0].x);

        ++cnt;
      }
      if (cnt == 3) {
        //create a fourth point
        sc2[3].x = sc2[1].x + (sc2[2].x - sc2[0].x);
        sc2[3].y = sc2[1].y + (sc2[2].y - sc2[0].y);

        dc2[3].x = dc2[1].x + (dc2[2].x - dc2[0].x);
        dc2[3].y = dc2[1].y + (dc2[2].y - dc2[0].y);
    }

    //transform from source coordinates to a 0,0,1,1, square
      Matrix4 p, q;
    setCornerPinMatrix(sc2, p);
    setCornerPinMatrix(dc2, q);

    matrix = world * (q * p.inverse());
    }
    
    if (_inverted)
      matrix = matrix.inverse();
  }

public:
  void _validate(bool for_real) override
  {
    setMatrix(sc, dc, *matrix());
    Transform::_validate(for_real);
  }

  void matrixAt(const OutputContext& context, Matrix4& matrix) override
  {
    xyStruct sc[4];
    xyStruct dc[4];
    Hash hash;
    knob("to1")->store(DoublePtr, &dc[0].x, hash, context);
    knob("enable1")->store(BoolPtr, &sc[0].enable, hash, context);
    knob("to2")->store(DoublePtr, &dc[1].x, hash, context);
    knob("enable2")->store(BoolPtr, &sc[1].enable, hash, context);
    knob("to3")->store(DoublePtr, &dc[2].x, hash, context);
    knob("enable3")->store(BoolPtr, &sc[2].enable, hash, context);
    knob("to4")->store(DoublePtr, &dc[3].x, hash, context);
    knob("enable4")->store(BoolPtr, &sc[3].enable, hash, context);
    knob("from1")->store(DoublePtr, &sc[0].x, hash, context);
    knob("from2")->store(DoublePtr, &sc[1].x, hash, context);
    knob("from3")->store(DoublePtr, &sc[2].x, hash, context);
    knob("from4")->store(DoublePtr, &sc[3].x, hash, context);
    knob("invert")->store(BoolPtr, &_inverted, hash, context);
    setMatrix(sc, dc, matrix);
  }

  const char* Class() const override { return CLASS; }
  const char* node_help() const override { return HELP; }
  static const Description desc;

  CornerPin2D(Node* node) : Transform(node), _inverted(false)
  {
    // initialize the matrix
    for ( int i = 0; i < 16; i++ )
    {
      _extraMatrix[i] = 0.0f;
    }

    _extraMatrix[0] = _extraMatrix[5] = _extraMatrix[10] = _extraMatrix[15] = 1.0f;
    _extraMatrixArray.set(4, 4, _extraMatrix);
    const Format& format = input_format();

    sc[0].x = sc[3].x = format.x();
    sc[1].x = sc[2].x = format.r();
    sc[0].y = sc[1].y = format.y();
    sc[2].y = sc[3].y = format.t();

    dc[0].x = dc[3].x = sc[0].x; //(format.x()*3+format.r())/4;
    dc[1].x = dc[2].x = sc[1].x; //(format.x()+format.r()*3)/4;
    dc[0].y = dc[1].y = sc[0].y; //(format.y()*3+format.t())/4;
    dc[2].y = dc[3].y = sc[2].y; //(format.y()+format.t()*3)/4;

    sc[0].enable = sc[1].enable = sc[2].enable = sc[3].enable = true;
    dc[0].enable = dc[1].enable = dc[2].enable = dc[3].enable = true;
  }

  void knobs(Knob_Callback f) override
  {
    XY_knob(f, &dc[0].x, "to1");
    SetFlags(f, DD::Image::Knob::ALWAYS_SAVE);
    Bool_knob(f, &sc[0].enable, "enable1");
    XY_knob(f, &dc[1].x, "to2");
    SetFlags(f, DD::Image::Knob::ALWAYS_SAVE);
    Bool_knob(f, &sc[1].enable, "enable2");
    XY_knob(f, &dc[2].x, "to3");
    SetFlags(f, DD::Image::Knob::ALWAYS_SAVE);
    Bool_knob(f, &sc[2].enable, "enable3");
    XY_knob(f, &dc[3].x, "to4");
    SetFlags(f, DD::Image::Knob::ALWAYS_SAVE);
    Bool_knob(f, &sc[3].enable, "enable4");
    PyScript_knob(f,
        "nuke.thisNode().knob('to1').fromScript(nuke.thisNode().knob('from1').toScript())\n"
        "nuke.thisNode().knob('to2').fromScript(nuke.thisNode().knob('from2').toScript())\n"
        "nuke.thisNode().knob('to3').fromScript(nuke.thisNode().knob('from3').toScript())\n"
        "nuke.thisNode().knob('to4').fromScript(nuke.thisNode().knob('from4').toScript())\n"
        , "copy_from", "Copy 'from'");
    Tooltip(f, "Take the contents from the 'from' knobs and put them in the 'to' knobs.");
    SetFlags(f, DD::Image::Knob::STARTLINE);

    BeginClosedGroup(f, "extra matrix");
    DD::Image::Knob *k = Array_knob(f, &_extraMatrixArray, 4, 4, "transform_matrix", "", true);
    k->set_flag(Knob::NO_CURVE_EDITOR | Knob::NO_MULTIVIEW);
    Tooltip(f, "This matrix gets concatenated against the transform defined by the other knobs.");
    EndGroup(f); 
    
    Bool_knob(f, &_inverted, "invert", "invert");
    SetFlags(f, DD::Image::Knob::ALWAYS_SAVE | DD::Image::Knob::STARTLINE);
    Transform::knobs(f);
    Tab_knob(f, 0, "From");
    XY_knob(f, &sc[0].x, "from1");
    SetFlags(f, DD::Image::Knob::ALWAYS_SAVE);
    XY_knob(f, &sc[1].x, "from2");
    SetFlags(f, DD::Image::Knob::ALWAYS_SAVE);
    XY_knob(f, &sc[2].x, "from3");
    SetFlags(f, DD::Image::Knob::ALWAYS_SAVE);
    XY_knob(f, &sc[3].x, "from4");
    SetFlags(f, DD::Image::Knob::ALWAYS_SAVE);
    PyScript_knob(f, "f = None\n"
        "try:\n"
        "  f = nuke.thisNode().input(0).format()\n"
        "except:\n"
        "  f = nuke.root().format()\n"
        "f1 = nuke.thisNode().knob('from1').fromScript(\"0 0\")\n"
        "f2 = nuke.thisNode().knob('from2').fromScript(str(f.width()) + \" 0\")\n"
        "f3 = nuke.thisNode().knob('from3').fromScript(str(f.width()) + \" \" + str(f.height()))\n"
        "f4 = nuke.thisNode().knob('from4').fromScript(\"0 \" + str(f.height()))\n"
        , "set_to_input", "Set to input");
    SetFlags(f, DD::Image::Knob::STARTLINE);
    ClearFlags(f, Knob::ENDLINE);
    Tooltip(f, "Set the size of the from knobs to the input format.");
    PyScript_knob(f,
        "nuke.thisNode().knob('from1').fromScript(nuke.thisNode().knob('to1').toScript())\n"
        "nuke.thisNode().knob('from2').fromScript(nuke.thisNode().knob('to2').toScript())\n"
        "nuke.thisNode().knob('from3').fromScript(nuke.thisNode().knob('to3').toScript())\n"
        "nuke.thisNode().knob('from4').fromScript(nuke.thisNode().knob('to4').toScript())\n"
        , "copy_to", "Copy 'to'");
    Tooltip(f, "Take the contents from the 'to' knobs and put them in the 'from' knobs.");
    ClearFlags(f, DD::Image::Knob::STARTLINE);
  }


  /*! Draw the outlines of the source and destination quadrilaterals. */
  void draw_handle(ViewerContext* ctx) override
  {
    if (ctx->draw_lines()) {
      const Info& i = concat_input_->info();
      glColor(ctx->node_color());
      gl_rectangle((float)i.x(), (float)i.y(), (float)i.r(), (float)i.t());
    }

    Transform::draw_handle(ctx);
  }
};

static Iop* build(Node* node) { return new CornerPin2D(node); }
const Iop::Description CornerPin2D::desc(CLASS, "Transform/CornerPin2D", build);

// end of CornerPin2D.C
