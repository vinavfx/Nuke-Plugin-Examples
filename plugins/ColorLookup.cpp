// ColorLookupIop.C
// Copyright (c) 2009 The Foundry Visionmongers Ltd.  All Rights Reserved.

const char* const HELP =
  "Output is the value of the color lookup curve indexed by the input color";

#include "DDImage/ColorLookup.h"
#include "DDImage/Row.h"
#include "DDImage/Knobs.h"
#include "DDImage/DDMath.h"
#include "DDImage/LookupCurves.h"
#include "DDImage/NukeWrapper.h"

#include <stdexcept>
#include <memory>
#include <algorithm>

using namespace DD::Image;

static const char* const CLASS = "ColorLookup";

static const CurveDescription defaults[] = {
  { "master", "y C 0 1" },
  { "red",    "y C 0 1" },
  { "green",  "y C 0 1" },
  { "blue",   "y C 0 1" },
  { "alpha",  "y C 0 1" },
  { nullptr }
};

class ColorLookupIop : public ColorLookup
{
  LookupCurves lut;
  float range;
  float range_knob;
  float source_value[4];
  float target_value[4];
  bool _usePrecomputedTable;
  //bool identity[5];
	
  /*! Array holding the precomputed values of a channels LookupCurve between 0 and range. This is used to
      generate a texture to sample from when using the ColorLookup in the timeline.
  */
  std::unique_ptr<float[]> _bakedCurve;

  /*! Arrays holding the gradient values of the outermost keys of the LookupCurves. These values are used 
      for the linear interpolation of the curves if an input falls outside of the range for which the baked
      curves are defined when using ColorLookup on the timeline.
  */
  std::array<float, NUMTABLES> _leftmostGradient;
  std::array<float, NUMTABLES> _rightmostGradients;

  /*! The inverse of the precomputed range of the LookupCurves. We use the inverse as we need to divide the
  *   input in out shader script and multiplication is less expensive on the gpu.
  */
  std::array<float, NUMTABLES> _inversePrecomputedRange;

  /*! The leftmost and rightmost values in the precomputed tables. The leftmost is the min on the leftmost key's
  *   x value and zero, and the rightmost is the max of the rightmost key's x values and the range.
  */
  std::array<float, NUMTABLES> _leftmostValue;
  std::array<float, NUMTABLES> _rightmostValue;

  int _textureUnit;
  GLuint _textureId;

  /*! String used to store the fragment shader source code generated and returned by gpuEngine_body().
      Needs to be mutable since gpuEngine_body() is (quite reasonably) const.
  */
  mutable std::string _shaderBodyText;

  /*! Bake the values from the LookupCurves into _bakedCurve 
  */
  void bakeCurves();

public:
  ColorLookupIop(Node* node) : ColorLookup(node), lut(defaults), _usePrecomputedTable(true)
  {
    //identity[0..4] = true;
    range = range_knob = 1;
    source_value[0] = source_value[1] = source_value[2] = source_value[3] = 0.0f;
    target_value[0] = target_value[1] = target_value[2] = target_value[3] = 0.0f;
  }
  float lookup(int z, float value) const override;
  void _validate(bool) override;
  void knobs(Knob_Callback) override;
  int knob_changed(Knob* k) override;
  const char* Class() const override { return CLASS; }
  const char* node_help() const override { return HELP; }
  static const Iop::Description d;

  void pixel_engine(const Row& in, int y, int x, int r, ChannelMask channels, Row& out) override;

  // GPU implementation
  const char* gpuEngine_decl() const override;
  const char* gpuEngine_body() const override;
  Hash gpuEngine_shader_hash_at(double time) override;
  void gpuEngine_GL_begin(GPUContext* context) override;
  void gpuEngine_GL_end(GPUContext* context) override;
  int gpuEngine_getNumRequiredTexUnits() const override;
};

void ColorLookupIop::pixel_engine(const Row& in, int y, int x, int r, ChannelMask channels, Row& out)
{
  // Use the precomputed table if the LUT is using an expression
  foreach (z, channels) {
    if (lut.hasExpression(0) || lut.hasExpression(colourIndex(z)+1)) {
      _usePrecomputedTable = true;
      break;
    }
  }

  try {
    if (_usePrecomputedTable) {
      if (range == 1.0f) {
        ColorLookup::pixel_engine(in, y, x, r, channels, out);
      }
      else {
        foreach (z, channels) {
          const float* FROM = in[z] + x;
          float* TO = out.writable(z) + x;
          const float* END = FROM + (r - x);
          while (FROM < END)
            *TO++ = *FROM++ / range;
        }
        ColorLookup::pixel_engine(out, y, x, r, channels, out);
      }
    }
    else {
      // do a direct lookup

      foreach (z, channels) {
        const float* FROM = in[z] + x;
        float* TO = out.writable(z) + x;
        const float* END = FROM + (r - x);
        while (FROM < END)
          *TO++ = lookup(colourIndex(z), *FROM++);
      }
    }
  }
  catch (std::out_of_range& ex) {
    error(ex.what());
    out.copy(in, channels, x, r);
  }
}

#if 0
static bool identity(const Animation* a, float range)
{
  #if 1
  for (float v = 0; v <= range; v += range / 8)
    if (a->getValue(v) != v)
      return false;
  #else
  for (int i = 0; i < a->size(); i++) {
    const Animation_Key& k = a->key(i);
    if (k.x != k.y)
      return false;
    if (k.interpolation & Animation::USER_SET_SLOPE) {
      if (k.lslope != 1 || k.rslope != 1)
        return false;
    }
  }
  #endif
  return true;
}
#endif

void ColorLookupIop::_validate(bool for_real)
{
  if (range_knob <= 0)
    range = 1.0f;
  else
    range = range_knob;
  //for (int i = 0; i < 5; i++) identity[i] = lut.isIdentity(i, range);
  try {
    ColorLookup::_validate(for_real);
    bakeCurves();
  }
  catch (std::out_of_range& ex) {
    error(ex.what());
  }
}
 
float ColorLookupIop::lookup(int z, float value) const
{
  // If precomputed tables are being used, the value needs to be adjusted such that all the values
  // within the range are stored in the tables. Nothing needs to be done to the value if the effect
  // is being used on the timeline, as it's already been adjusted in bakeCurves() before lookup is called.
  if (_usePrecomputedTable && nodeContext() != eTimeline) {
    value *= range;
  }

  if (lut.hasExpression(0) || lut.hasExpression(z + 1)) {
    if ((value < 0 || value > 1)) {
      // if there's a precomputed table, then we can manage ones between 0 and 1 by lookup
      // within that, otherwise we can't manage it at all
      throw std::out_of_range("HDR input out of range for precomputed table");
      return NAN;
    }
  }
  
  value = float(lut.getValue(0, value));
  value = float(lut.getValue(z + 1, value));

  return value;
}

const char* setRgbScript =
  "source = nuke.thisNode().knob('source')\n"
  "target = nuke.thisNode().knob('target')\n"
  "lut = nuke.thisNode().knob('lut')\n"
  "lut.setValueAt(target.getValue(0), source.getValue(0), 1)\n"
  "lut.setValueAt(target.getValue(1), source.getValue(1), 2)\n"
  "lut.setValueAt(target.getValue(2), source.getValue(2), 3)\n";

const char* setRgbaScript =
  "source = nuke.thisNode().knob('source')\n"
  "target = nuke.thisNode().knob('target')\n"
  "lut = nuke.thisNode().knob('lut')\n"
  "lut.setValueAt(target.getValue(0), source.getValue(0), 1)\n"
  "lut.setValueAt(target.getValue(1), source.getValue(1), 2)\n"
  "lut.setValueAt(target.getValue(2), source.getValue(2), 3)\n"
  "lut.setValueAt(target.getValue(3), source.getValue(3), 4)\n";

const char* setAScript =
  "source = nuke.thisNode().knob('source')\n"
  "target = nuke.thisNode().knob('target')\n"
  "lut = nuke.thisNode().knob('lut')\n"
  "lut.setValueAt(target.getValue(3), source.getValue(3), 4)\n";


void ColorLookupIop::knobs(Knob_Callback f)
{
  Obsolete_knob(f, "layer", "knob channels $value");
  Newline(f);

  Bool_knob(f, &_usePrecomputedTable, "use_precomputed", "use precomputed table");

  // The timeline's gpu implementation of ColorLookup must use precomputed tables as it's unable to lookup LUT values
  // on the fly, so it doesn't make any sense to have the 'use precomputed' knob available there.
  SetFlags(f, Knob::NODEGRAPH_ONLY);
  Tooltip(f, "use precomputed table between 0 and 'range' for faster processing");

  Float_knob(f, &range_knob, IRange(1, 16), "range");
  Tooltip(f, "Values between 0 and this will use a lookup table and thus be much faster");
  
  LookupCurves_knob(f, &lut, "lut");
  Newline(f);
  AColor_knob(f, source_value, IRange(0, 4), "source");
  SetFlags(f, Knob::NO_ANIMATION | Knob::NO_RERENDER | Knob::DO_NOT_WRITE);
  Tooltip(f, "Pick a source color for adding points.");
  AColor_knob(f, target_value, IRange(0, 4), "target");
  SetFlags(f, Knob::NO_ANIMATION | Knob::NO_RERENDER | Knob::DO_NOT_WRITE);
  Tooltip(f, "Pick a destination color for adding points.");
  Newline(f);
  PyScript_knob(f, setRgbScript, "setRGB", "Set RGB");
  Tooltip(f, "Add points on the r, g, b curves mapping source to target.");
  PyScript_knob(f, setRgbaScript, "setRGBA", "Set RGBA");
  Tooltip(f, "Add points on the r, g, b, and a curves mapping source to target.");
  PyScript_knob(f, setAScript, "setA", "Set A");
  Tooltip(f, "Add points on the a curve mapping source to target.");
  Divider(f);
}

int ColorLookupIop::knob_changed(Knob* k)
{
  if (k->name() == "lut" || k == &Knob::showPanel) {
    // If any of the LUTs are using an expression, the pre-computed table will
    // be used as they cannot be evaluated within pixel_engine. Force the knob
    // values to match this state so it's clear the table is in use.

    bool hasExpression = false;
    for (size_t c = 0; c < lut.size(); ++c) {
      if (lut.hasExpression(c)) {
        hasExpression = true;
        break;
      }
    }

    if (hasExpression) {
      knob("use_precomputed")->set_value(true);
      knob("use_precomputed")->enable(false);
    }
    else {
      knob("use_precomputed")->enable(true);
    }
  }

  if (k->name() == "use_precomputed" || k == &Knob::showPanel) {
    knob("range")->enable(_usePrecomputedTable);
  }

  return 1;
}

void ColorLookupIop::bakeCurves()
{
  if (nodeContext() != eTimeline) {
    return;
  }

  if (!_bakedCurve) {
    _bakedCurve = std::make_unique<float[]>(NUMTABLES * kEntries);
  }

  // The master curve effects the range and outer gradients of the precomputed tables, so we need to fetch the
  // outer keys of the master curve in order to take this into account when determining the leftmost and rightmost
  // values and gradients of the precomputed tables
  LookupCurves::SKey leftmostMasterKey;
  LookupCurves::SKey rightmostMasterKey;
  lut.getOuterKeys(0, leftmostMasterKey, rightmostMasterKey);

  for (int tableIndex = 0; tableIndex < NUMTABLES; ++tableIndex) {
    LookupCurves::SKey leftmostKey;
    LookupCurves::SKey rightmostKey;
    lut.getOuterKeys(tableIndex+1, leftmostKey, rightmostKey);
    _leftmostValue[tableIndex] = std::min(leftmostKey.x, leftmostMasterKey.x);
    _rightmostValue[tableIndex] = std::max( rightmostKey.x, rightmostMasterKey.x);
    _leftmostGradient[tableIndex] = leftmostKey.slope * leftmostMasterKey.slope;
    _rightmostGradients[tableIndex] = rightmostKey.slope * rightmostMasterKey.slope;

    const float precomputedRange = _rightmostValue[tableIndex] - _leftmostValue[tableIndex];
    _inversePrecomputedRange[tableIndex] = (precomputedRange != 0) ? (1.0f / precomputedRange) : 0;

    const int indexOffset = tableIndex * kEntries;
    for ( int entryIndex = 0; entryIndex < kEntries; ++entryIndex) {
      const float x = (entryIndex / static_cast<float>(kEntries - 1)) * precomputedRange + _leftmostValue[tableIndex];
      _bakedCurve[entryIndex + indexOffset] = lookup(tableIndex, x);
    }
  }
}

const char* ColorLookupIop::gpuEngine_decl() const
{
  if (nodeContext() != eTimeline) {
    return nullptr;
  }

  return  "uniform sampler1DArray $$curveSampler; \n"
    "uniform vec4 $$inversePrecomputedRange; \n"
    "uniform vec4 $$leftmostValue; \n"
    "uniform vec4 $$rightmostValue; \n"
    "uniform vec4 $$leftmostGradients; \n"
    "uniform vec4 $$rightmostGradients; \n";
}

const char* ColorLookupIop::gpuEngine_body() const
{
  if (nodeContext() != eTimeline) {
    return nullptr;
  }

  std::stringstream shaderText;
  shaderText << "{ \n ";
	 
  shaderText << "  for ( int i = 0; i < 4; ++i ) { \n"
                "    float textureCoord; \n"
                "    float extrapolatedValue = 0.0; \n"
                "    if ( OUT[i] < $$leftmostValue[i] ) { \n"
                "      extrapolatedValue = ( OUT[i] - $$leftmostValue[i] ) * $$leftmostGradients[i]; \n"
                "      textureCoord = 0.0; \n"
                "    } \n"
                "    else if ( OUT[i] > $$rightmostValue[i] ) { \n"
                "      extrapolatedValue = (OUT[i] - $$rightmostValue[i]) * $$rightmostGradients[i]; \n"
                "      textureCoord = 1.0; \n"
                "    } \n"
                "    else { \n"
                "       textureCoord = ( OUT[i] - $$leftmostValue[i] ) * $$inversePrecomputedRange[i]; \n"
                "    } \n"
                "    OUT[i] = texture1DArray($$curveSampler, vec2(textureCoord, i)).r + extrapolatedValue; \n"
                "  } \n"

                "} \n";

  _shaderBodyText = shaderText.str();
  return _shaderBodyText.c_str();
}

Hash ColorLookupIop::gpuEngine_shader_hash_at(double time)
{
  Hash hash;

  hash.append(knob("range")->get_value_at(time));

  return hash;
}

void ColorLookupIop::gpuEngine_GL_begin(GPUContext* context)
{
  if (nodeContext() != eTimeline) {
    return;
  }

  context->bind("$$inversePrecomputedRange", 4, 1, _inversePrecomputedRange.data());
  context->bind("$$leftmostValue", 4, 1, _leftmostValue.data());
  context->bind("$$rightmostValue", 4, 1, _rightmostValue.data());
  context->bind("$$leftmostGradients", 4, 1, _leftmostGradient.data());
  context->bind("$$rightmostGradients", 4, 1, _rightmostGradients.data());


  _textureUnit = context->acquireTextureUnit();
  const bool invalidTextureUnit = (_textureUnit == -1);
  mFnAssertMsg(!invalidTextureUnit, "Correct::gpuEngine_GL_begin: failed to acquire texture unit!");
  if (invalidTextureUnit) {
    return;
  }

  glGenTextures(1, &_textureId);

  glActiveTextureARB(GL_TEXTURE0 + _textureUnit);
  glBindTexture(GL_TEXTURE_1D_ARRAY, _textureId);

  glTexImage2D(GL_TEXTURE_1D_ARRAY, 0, GL_R32F, kEntries, NUMTABLES, 0, GL_RED, GL_FLOAT, _bakedCurve.get());
  glTexParameteri(GL_TEXTURE_1D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_1D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_1D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  context->bind("$$curveSampler", _textureUnit);
}

void ColorLookupIop::gpuEngine_GL_end(GPUContext* context)
{
  if (nodeContext() != eTimeline) {
    return;
  }

  const bool validTexureUnit = _textureUnit != -1;
  if (validTexureUnit) {
    context->releaseTextureUnit(_textureUnit);
  }

  glBindTexture(GL_TEXTURE_1D_ARRAY, 0);
  glDeleteTextures(1, &_textureId);
}

int ColorLookupIop::gpuEngine_getNumRequiredTexUnits() const
{
  return 1;
}

static Iop* build(Node* node)
{
  return (new NukeWrapper(new ColorLookupIop(node)))->channels(Mask_RGBA);
}
const Iop::Description ColorLookupIop::d(CLASS, "Color/Correct/Lookup", build);
