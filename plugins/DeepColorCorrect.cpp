// Copyright (c) 2011 The Foundry Visionmongers Ltd.  All Rights Reserved.

// This is the new luminosity-keyed color corrector.

#include "DDImage/Row.h"
#include "DDImage/Knobs.h"
#include "DDImage/DDMath.h"
#include "DDImage/LookupCurves.h"
#include "DDImage/DeepFilterOp.h"
#include "DDImage/DeepPixelOp.h"
#include "DDImage/Pixel.h"
#include "DDImage/RGB.h"

#include <stdio.h>

using namespace DD::Image;

enum { SHADOW, MIDTONE, HIGHLIGHT, MASTER };

static const CurveDescription defaults[] = {
  { "shadow", "curve 1 s0 x0.09 0 s0" },
  { "midtone", "1-shadow-highlight" },
  { "highlight", "curve x0.5 0 s0 x1 1 s0" },
  { nullptr }
};

class DeepCorrect : public DeepPixelOp
{

  float midtone;
  float highlight;

  // First index is range, second index is rgba:
  float gamma[4][4];
  float contrast[4][4];
  float gain[4][4];
  float saturation[4][4];
  float offset[4][4];

  // Values are cooked into these combined values:
  float pow1[3][4];
  float mult[3][4];
  float sat[3][4];
  float add[3][4];

  int lookup_type;
  LookupCurves lookup;
  bool test; // checkmark in gui to show the lookup
  bool all_equal; // true if all ranges are the same
  bool no_saturation; // all saturation controls are at 1

public:

  DeepCorrect(Node* node) : DeepPixelOp(node), lookup(defaults)
  {
    midtone = .18f;
    highlight = 1;
    for (int z = 0; z < 4; z++) {
      for (int x = 0; x < 4; x++) {
        gamma[x][z] = 1;
        contrast[x][z] = 1;
        gain[x][z] = 1;
        saturation[x][z] = 1;
        offset[x][z] = 0;
      }
    }
    lookup_type = 0;
    test = false;
  }


  // Wrapper function to work around the "non-virtual thunk" issue on linux when symbol hiding is enabled.
  bool doDeepEngine(DD::Image::Box box, const ChannelSet &channels, DeepOutputPlane &plane) override
  {
    return DeepPixelOp::doDeepEngine(box, channels, plane);
  }


  // Wrapper function to work around the "non-virtual thunk" issue on linux when symbol hiding is enabled.
  void getDeepRequests(DD::Image::Box box, const DD::Image::ChannelSet& channels, int count, std::vector<RequestData>& requests) override
  {
    DeepPixelOp::getDeepRequests(box, channels, count, requests);
  }


  // We always need the rgb no matter what channels are asked for:
  void in_channels(int, ChannelSet& channels) const override
  {
    if (all_equal && no_saturation && !test)
      return;
    // Must turn on the rgb if any color channels are requested:
    ChannelSet done;
    foreach (z, channels) {
      if (colourIndex(z) < 4 && !(done & z))
        done.addBrothers(z, 3);
    }
    channels += done;
  }

  // for menu listing and function controls
  void knobs(Knob_Callback) override;
  void createTab(Knob_Callback f, const char* label, int knobCnt);

  static const Iop::Description d;
  const char* Class() const override { return d.name; }

  Op* op() override
  {
    return this;
  }

  void _validate(bool) override;
  void processSample(int y,
                             int x,
                             const DD::Image::DeepPixel& deepPixel,
                             size_t sampleNo,
                             const DD::Image::ChannelSet& channels,
                             DeepOutPixel& output) const override;

  const char* node_help() const override;
};

// Safe power function that does something reasonable for negative numbers
// e must be clamped to the range -MAXPOW..MAXPOW
#define MAXPOW 1000.0f
static inline float P(float x, float e)
{
  return x > 0 ? powf(x, e) : x;
}

// These are the values that contrast does not change for each range:
static const float K[4] = {
  .04f, .18f, .65f, .18f
};

void DeepCorrect::_validate(bool for_real)
{
  all_equal = true;
  no_saturation = true;
  for (int n = 0; n < 3; n++) {
    for (int c = 0; c < 4; c++) {
      const float c1 = contrast[n][c];
      const float k1 = K[n];
      const float c2 = contrast[MASTER][c];
      const float k2 = K[MASTER];
      const float g = clamp(1 / (gamma[n][c] * gamma[MASTER][c]), 0.0f, MAXPOW);
      // The formula is (((a/k1)**c1*k1/k2)**c2*k2)**g, but it is expaneded
      // here to a single exponent and multiplier:
      pow1[n][c] = clamp(c1 * c2 * g, 0.0f, MAXPOW);
      const float e1 = clamp((1 - c1) * c2 * g, -MAXPOW, MAXPOW);
      const float e2 = clamp((1 - c2) * g, -MAXPOW, MAXPOW);
      mult[n][c] = P(k1, e1) * P(k2, e2) * gain[n][c] * gain[MASTER][c];
      sat[n][c] = saturation[n][c] * saturation[MASTER][c];
      if (sat[n][c] != 1)
        no_saturation = false;
      add[n][c] = offset[n][c] + offset[MASTER][c];
      if (n) {
        if (pow1[n][c] != pow1[0][c] ||
            mult[n][c] != mult[0][c] ||
            sat[n][c] != sat[0][c] ||
            add[n][c] != add[0][c])
          all_equal = false;
      }
      //      printf("pow1[%d][%d] = %g ", n,c,pow1[n][c]);
      //      printf("mult[%d][%d] = %g ", n,c,mult[n][c]);
      //      printf("sat[%d][%d] = %g ", n,c,sat[n][c]);
      //      printf("add[%d][%d] = %g\n", n,c,add[n][c]);
    }
  }
  DeepPixelOp::_validate(for_real);
  // This gets the most common case where black becomes non-black.
  // User could still change the weighting curves so the midtones and highlights
  // have non-zero effect at zero:
  //  if (add[0][0] || add[0][1] || add[0][2] || add[0][3])
  //    info_.black_outside(false);
}

void DeepCorrect::processSample(int y,
                                int x,
                                const DD::Image::DeepPixel& deepPixel,
                                size_t sampleNo,
                                const DD::Image::ChannelSet& channels,
                                DeepOutPixel& output) const
{
  Channel chan[4];

  const float* inptr[4];
  float* outptr[4];

  ChannelSet done;

  DD::Image::Pixel outPixel(channels);

  foreach (z, channels) { // visit every channel asked for
    if (z == Chan_Z || z == Chan_Alpha || z == Chan_DeepFront || z == Chan_DeepBack
#ifdef NUKE_OBJECT_ID
        || z == Chan_Object_ID
#endif
        ) {
      outPixel[z] = deepPixel.getUnorderedSample(sampleNo, z);
      continue;
    }
    if (done & z)
      continue;             // skip if we did it as a side-effect of another channel
    if (colourIndex(z) >= 4) {
      outPixel[z] = deepPixel.getUnorderedSample(sampleNo, z);
      continue;
    }

    for (int c = 0; c < 4; c++) {
      chan[c] = brother(z, c);
      if (done.contains(chan[c]))
        chan[c] = Chan_Black;
      done += chan[c];

      static const float sZero = 0.0;

      inptr[c] = chan[c] == Chan_Black ? &sZero : &deepPixel.getUnorderedSample(sampleNo, chan[c]);
      outptr[c] = &outPixel[chan[c]];
    }

    const float* END = inptr[0] + 1;

    if (test) {
      for (;;) {
        float g = y_convert_rec709(*inptr[0], *inptr[1], *inptr[2]);
        float w0 = float(lookup.getValue(0, g));
        float w2 = float(lookup.getValue(2, g));
        float w1 = 1 - w0 - w2;
        if (w2 > w1)
          g = 1;
        else if (w0 > w1)
          g = 0;
        else
          g = .25f;
        *outptr[0]++ = g;
        *outptr[1]++ = w1 * 0.25f + w2;
        *outptr[2]++ = g;
        *outptr[3]++ = g;
        if (++inptr[0] >= END)
          break;
        ++inptr[1];
        ++inptr[2];
        ++inptr[3];
      }
    }
    else if (all_equal) {
      if (no_saturation) {
        for (int c = 0; c < 4; c++) {
          if (!intersect(channels, chan[c]))
            continue;
          const float* I = inptr[c];
          const float* E = I + 1;
          float* O = outptr[c];
          if (pow1[0][c] != 1) {
            for (;;) {
              *O++ = P(*I * sat[0][c], pow1[0][c]) * mult[0][c] + add[0][c];
              if (++I >= E)
                break;
            }
          }
          else {
            for (;;) {
              *O++ = *I * sat[0][c] * mult[0][c] + add[0][c];
              if (++I >= E)
                break;
            }
          }
        }
      }
      else {
        for (;;) {
          float g = y_convert_rec709(*inptr[0], *inptr[1], *inptr[2]);
          for (int c = 0; c < 4; c++) {
            if (c == 3 && !intersect(channels, chan[c]))
              continue;
            const float a = *inptr[c];
            *outptr[c]++ =
              P(a * sat[0][c] + g * (1 - sat[0][c]), pow1[0][c]) * mult[0][c] + add[0][c];
          }
          if (++inptr[0] >= END)
            break;
          ++inptr[1];
          ++inptr[2];
          ++inptr[3];
        }
      }
    }
    else {
      for (;;) {
        float g = y_convert_rec709(*inptr[0], *inptr[1], *inptr[2]);
        float w0 = float(lookup.getValue(0, g));
        float w2 = float(lookup.getValue(2, g));
        float w1 = 1 - w0 - w2;
        for (int c = 0; c < 4; c++) {
          if (c == 3 && !intersect(channels, chan[c]))
            continue;
          const float a = *inptr[c];
          *outptr[c]++ =
            (P(a * sat[0][c] + g * (1 - sat[0][c]), pow1[0][c]) * mult[0][c] + add[0][c]) * w0 +
            (P(a * sat[1][c] + g * (1 - sat[1][c]), pow1[1][c]) * mult[1][c] + add[1][c]) * w1 +
            (P(a * sat[2][c] + g * (1 - sat[2][c]), pow1[2][c]) * mult[2][c] + add[2][c]) * w2;
        }
        if (++inptr[0] >= END)
          break;
        ++inptr[1];
        ++inptr[2];
        ++inptr[3];
      }
    }
  }
  foreach(z, channels) {
    output.push_back(outPixel[z]);
  }
}

void DeepCorrect::knobs(Knob_Callback f)
{
  createTab(f, "@b;master", MASTER);
  createTab(f, "@b;shadows", 0);
  createTab(f, "@b;midtones", 1);
  createTab(f, "@b;highlights", 2);
  
  Tab_knob(f, "Masking");
  DeepPixelOp::knobs(f);

  Tab_knob(f, "Ranges");
  Bool_knob(f, &test, "test");
  Tooltip(f, "Turn this on to adjust the lookup curves. The output is then "
             "replaced with black, gray, or white to show what range the is "
             "being used there. Green or magenta indicate mixtures of ranges.");
  LookupCurves_knob(f, &lookup, "lookup");
  Tooltip(f, "Do not attempt to adjust the midtones curve. It is always "
             "1 minus the other two curves");
}

static const char* names[5][6];

void DeepCorrect::createTab(Knob_Callback f, const char* label, int knobCnt)
{

  const char* name = label + 3; // skip the @b;

  BeginGroup(f, name, label);

  // We have to give every knob a unique name. This is done by assigning
  // all the ones on subsequent tabs the name <tabname>_<name>
  if (!names[knobCnt][0]) {
    if (knobCnt == MASTER) {
      names[knobCnt][0] = "gamma";
      names[knobCnt][1] = "contrast";
      names[knobCnt][2] = "gain";
      names[knobCnt][3] = "saturation";
      names[knobCnt][4] = "offset";
    }
    else {
      for (int j = 0; j < 5; j++) {
        char* s = new char[24];
        sprintf(s, "%s.%s", name, names[MASTER][j]);
        names[knobCnt][j] = s;
      }
    }
  }
  // Knobs are shown in the order that they are applied to the color:
  AColor_knob(f, saturation[knobCnt], names[knobCnt][3], names[3][3]);
  SetRange(f, 0, 4);
  AColor_knob(f, contrast[knobCnt], names[knobCnt][1], names[3][1]);
  SetRange(f, 0, 4);
  AColor_knob(f, gamma[knobCnt], names[knobCnt][0], names[3][0]);
  SetRange(f, .2, 5);
  AColor_knob(f, gain[knobCnt], names[knobCnt][2], names[3][2]);
  SetRange(f, 0, 4);
  AColor_knob(f, offset[knobCnt], names[knobCnt][4], names[3][4]);
  SetRange(f, -1, 1);

  EndGroup(f);
}

const char* DeepCorrect::node_help() const
{
  return
    "Shadow/midtone/highlight ranges are controlled by lookup curves on the Ranges tab.\n"
    "Setting saturation to zero makes the color equal the gray used for the range.\n"
    "Setting contrast to zero makes the color equal a fixed gray value. You can "
    "alter this value by changing the gamma.\n"
    "You can use this to make mattes by setting gain for alpha to zero and "
    "setting the offset for alpha to 1 in the range you want a matte for.";
}

static Op* build(Node* node) { return new DeepCorrect(node); }
const Op::Description DeepCorrect::d("DeepColorCorrect", nullptr, build);
