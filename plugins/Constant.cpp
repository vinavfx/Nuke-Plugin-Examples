// Constant.C
// Copyright (c) 2009 The Foundry Visionmongers Ltd.  All Rights Reserved.

static const char* const CLASS = "Constant";

static const char* const HELP =
  "Constant:\n"
  "Produces an image where every pixel is the same color. This includes "
  "pixels outside the image area as well as inside it."
  "\n\n"
  "The first and last frame set the length of the clip for operators that "
  "want that information.";

#include "DDImage/DDWindows.h"
#include "DDImage/Black.h"
#include "DDImage/Row.h"
#include "DDImage/Format.h"
#include "DDImage/Scene.h"
#include "DDImage/LUT.h"
#include "DDImage/gl.h"
#include "DDImage/GeoInfo.h"
#include "DDImage/Knobs.h"
#include "DDImage/VertexContext.h"

using namespace DD::Image;

class Constant : public Black
{
protected:
  void _validate(bool) override;
  Channel channel[4];
  float color[4];
  int first_frame, last_frame;
  FormatPair formats;
public:
  Constant(Node* node) : Black(node)
  {
    channel[0] = Chan_Red;
    channel[1] = Chan_Green;
    channel[2] = Chan_Blue;
    channel[3] = Chan_Alpha;
    color[0] = color[1] = color[2] = color[3] = 0;
    first_frame = 1;
    last_frame = 1;
    formats.format(nullptr);
  }
  void engine(int y, int x, int r, ChannelMask, Row & row) override;
  void fragment_shader(const VertexContext&, Pixel&) override;
  bool shade_GL(ViewerContext* ctx, GeoInfo& geo) override;
  int slowness() const { return 0; }
  void knobs(Knob_Callback) override;
  const char* Class() const override { return ::CLASS; }
  static const Description d;
  const char* node_help() const override { return HELP; }
};

void Constant::_validate(bool)
{
  // Set the user-selected channels, see if any are non-zero:
  bool non_zero = false;
  info_.channels(Mask_None);
  for (int z = 0; z < 4; z++) {
    info_.turn_on(channel[z]);
    if (color[z])
      non_zero = true;
  }
  // When used internally by other plugins they don't set formats,
  // they just put the format into the info_. Detect this and don't
  // write over the format they chose:
  if (formats.format()) {
    info_.format(*formats.format());
    info_.full_size_format(*formats.fullSizeFormat());
  }
  info_.black_outside(!non_zero);
#if 1
  // Use a bounding box the size of the image. This is what most file
  // readers would do if they read an image that was a solid color:
  info_.set(format());
#else
  // Use a 1x1 bounding box. This should be correct as the color is
  // replicated from the one pixel everywhere. However plenty of
  // operators have dependencies on the bounding box.  Some pre-4.8
  // versions of Nuke did this if and only if the constant was black,
  // but even that caused problems.
  info_.set(format().x(), format().y(), format().x() + 1, format().y() + 1);
#endif
  info_.first_frame(first_frame);
  info_.last_frame(last_frame);
}

void Constant::engine(int y, int x, int r, ChannelMask channels, Row& row)
{
  for (int i = 4; i--; )
    if (intersect(channels, channel[i])) {
      float C = color[i];
      if (C) {
        float* TO = row.writable(channel[i]) + x;
        float* END = TO + (r - x);
        while (TO < END)
          *TO++ = C;
      }
      else {
        row.erase(channel[i]);
      }
    }

}

void Constant::fragment_shader(const VertexContext& vtx, Pixel& out)
{
  for (int i = 0; i < 4; i++)
    out[channel[i]] = color[i];

  // be sure to set as solid if no alpha is present
  if ( channel[3] != Chan_Alpha )
    out[Chan_Alpha] = 1.0f;
}

bool Constant::shade_GL(ViewerContext* ctx, GeoInfo& geo)
{
  glColor4fvLUT(color);
  return true;
}

void Constant::knobs(Knob_Callback f)
{
  Channel_knob(f, channel, 4, "channels");
  AColor_knob(f, color, "color");
  Format_knob(f, &formats, "format");
  Obsolete_knob(f, "full_format", "knob format $value");
  Obsolete_knob(f, "proxy_format", nullptr);
  Int_knob(f, &first_frame, "first", "frame range");
  SetFlags(f, Knob::NO_ANIMATION);
  Int_knob(f, &last_frame, "last", "");
  SetFlags(f, Knob::NO_ANIMATION);
}

static Iop* build(Node* node) { return new Constant(node); }
const Iop::Description Constant::d(::CLASS, "Image/Constant", build);
