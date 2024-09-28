// Grade.C
// Copyright (c) 2009 The Foundry Visionmongers Ltd.  All Rights Reserved.

const char* const HELP =
  "<p>Applies a linear ramp followed by a gamma function to each color channel.</p>"
  "<p>  A = multiply * (gain-lift)/(whitepoint-blackpoint)<br>"
  "  B = offset + lift - A*blackpoint<br>"
  "  output = pow(A*input + B, 1/gamma)</p>"
  "The <i>reverse</i> option is also provided so that you can copy-paste this node to "
  "invert the grade. This will do the opposite gamma correction followed by the "
  "opposite linear ramp.";

#include "DDImage/PixelIop.h"
#include "DDImage/Row.h"
#include "DDImage/DDMath.h"
#include "DDImage/NukeWrapper.h"
#include <string.h>
#include <algorithm>

using namespace DD::Image;

static const char* const CLASS = "Grade";

class GradeIop : public PixelIop
{
  float blackpoint[4];
  float whitepoint[4];
  float black[4];
  float white[4];
  float add[4];
  float multiply[4];
  float gamma[4];
  bool reverse;
  bool black_clamp;
  bool white_clamp;
  
  // String used to store the fragment shader source code generated and returned by gpuEngine_body().
  // Needs to be mutable since gpuEngine_body() is (quite reasonably) const.
  mutable std::string _shaderBodyText;
  
public:
  GradeIop(Node* node) : PixelIop(node)
  {
    for (int n = 0; n < 4; n++) {
      black[n] = blackpoint[n] = add[n] = 0.0f;
      white[n] = whitepoint[n] = multiply[n] = 1.0f;
      gamma[n] = 1.0f;
    }
    reverse = false;
    black_clamp = true;
    white_clamp = false;
  }
  // indicate that channels only depend on themselves:
  void in_channels(int, ChannelSet& channels) const override { }
  void pixel_engine(const Row &in, int y, int x, int r, ChannelMask, Row &) override;
  void knobs(Knob_Callback) override;
  const char* Class() const override { return CLASS; }
  const char* node_help() const override { return HELP; }
  static const Iop::Description d;
  
  const char* gpuEngine_decl() const override;
  const char* gpuEngine_body() const override;
  Hash gpuEngine_shader_hash_at(double time) override;
  void gpuEngine_GL_begin(DD::Image::GPUContext* context) override;
  void gpuEngine_GL_end(DD::Image::GPUContext* context) override;
  
  void _validate(bool for_real) override;
  bool pass_transform() const override { return true; }

  /// Returns true if all of the components of the gamma knob are set to (exactly) 1 and none
  /// of them are animated. If that's the case the GPU shader can be simplified and in particular
  /// avoid an expensive pow() call. Otherwise the GPU path will use a uniform for uploading the
  /// gamma components, regardless of whether they're animated as there's little benefit to
  /// baking them into the shader as the pow() call can't be optimised out if they're not all 1.
  ///
  /// If we bake non-animated gamma values into the shader, the compiler may be able to optimise
  /// a little more, since it could exclude the calculations and subsequent selection logic needed
  /// to handle the case whethe gamma has the opposite sign (though in practice this will almost
  /// certainly only make any difference if all the components have the same sign).
  /// However, that should be small compared to the cost of the pow() and we've also seen problems
  /// on linux RHEL 6 where a baked in value of 2.0 (though not 1.99 or 2.01!) would cause the GLSL
  /// linker to crash! (Bug 46269 - NukeStudio - Grade Soft-effect - Entering '2.0' in the gamma
  /// knob and hitting Enter crashes Studio)
  ///
  /// (Note: There is the possibility that gamma is set as animated but in fact all the keys have
  /// a value of 1, in which case we'd needlessly pay the cost of the pow() calls, but that's 
  /// unlikely and something we shoudl encourage users to avoid.)
  bool isGammaConstantOne() const;
};

void GradeIop::_validate(bool for_real)
{
  bool change_any = black_clamp | white_clamp;
  bool change_zero = false;
  for (int z = 0; z < 4; z++) {
    float A = whitepoint[z] - blackpoint[z];
    A = A ? (white[z] - black[z]) / A : 10000.0f;
    A *= multiply[z];
    float B = add[z] + black[z] - blackpoint[z] * A;
    if (A != 1 || B || gamma[z] != 1.0f) {
      change_any = true;
      if (B)
        change_zero = true;
    }
  }
  set_out_channels(change_any ? Mask_All : Mask_None);
  PixelIop::_validate(for_real);
  if (change_zero)
    info_.black_outside(false);
}

void GradeIop::pixel_engine(const Row& in, int y, int x, int r,
                            ChannelMask channels, Row& out)
{
  auto clampBlack = [](const float x) { return std::max(x, 0.0f); };
  auto clampWhite = [](const float x) { return std::min(x, 1.0f); };
  auto clampBoth =  [](const float x) { return std::max(std::min(x, 1.0f), 0.0f); };
  const int w = r-x;

  for (Channel n : channels) {
    unsigned z = colourIndex(n);
    if (z > 3) {
      out.copy(in, n, x, r);
      continue;
    }
    float A = whitepoint[z] - blackpoint[z];
    A = A ? (white[z] - black[z]) / A : 10000.0f;
    A *= multiply[z];
    float B = add[z] + black[z] - blackpoint[z] * A;
    if (!B && in.is_zero(n)) {
      out.erase(n);
      continue;
    }
    float G = gamma[z];
    const float* inBegin = in[n] + x;
    float* outptr = out.writable(n) + x;
    if (!reverse) {
      // do the linear interpolation:
      if (A != 1 || B) {
        std::transform(inBegin, inBegin+w, outptr, [A, B](const float x) { return x*A+B; });
        inBegin = outptr;
      }
      // clamp
      if (white_clamp || black_clamp) {
        if (!white_clamp) {
          std::transform(inBegin, inBegin+w, outptr, clampBlack);
        }
        else if (!black_clamp) {
          std::transform(inBegin, inBegin+w, outptr, clampWhite);
        }
        else {
          std::transform(inBegin, inBegin+w, outptr, clampBoth);
        }
        inBegin = outptr;
      }
      // do the gamma:
      if (G <= 0) {
        std::transform(inBegin, inBegin+w, outptr,
          [](const float x) {
            if (x < 0.0f)
              return 0.0f;
            if (x > 1.0f)
              return INFINITY;
            return x;
          }
        );
      }
      else if (G != 1.0f) {
        G = 1.0f / G;
        std::transform(inBegin, inBegin+w, outptr,
          [G](const float x) {
            if (x < 0.0f)
              return x;
            if (x < 1.0f)
              return powf(x, G);
            return 1.0f + (x - 1.0f) * G;
          }
        );
      }
      else if (inBegin != outptr) {
        std::copy(inBegin, inBegin+w, outptr);
      }
    }
    else {
      // Reverse gamma:
      if (G <= 0) {
        std::transform(inBegin, inBegin+w, outptr,
          [](const float x) {
            return x > 0.0f ? 1.0f : 0.0f;
          }
        );
        inBegin = outptr;
      }
      else if (G != 1.0f) {
        std::transform(inBegin, inBegin+w, outptr,
          [G](const float x) {
            if (x <= 0.0f)
              return x;              //V = 0.0f;
            else if (x < 1.0f)
              return powf(x, G);
            return 1.0f + (x - 1.0f) * G;
          }
        );
        inBegin = outptr;
      }
      // Reverse the linear part:
      if (A != 1.0f || B) {
        if (A)
          A = 1 / A;
        else
          A = 1.0f;
        B = -B * A;
        std::transform(inBegin, inBegin+w, outptr, [A, B](const float x) { return x*A+B; });
        inBegin = outptr;
      }
      // clamp
      if (white_clamp || black_clamp) {
        if (black_clamp) {
          std::transform(inBegin, inBegin+w, outptr, clampBlack);
        }
        else if (white_clamp) {
          std::transform(inBegin, inBegin+w, outptr, clampWhite);
        }
        inBegin = outptr;
      }
      else if (inBegin != outptr) {
        std::copy(inBegin, inBegin+w, outptr);
      }
    }
  }
}

#include "DDImage/Knobs.h"

void GradeIop::knobs(Knob_Callback f)
{
  AColor_knob(f, blackpoint, IRange(-1, 1), "blackpoint");
  Tooltip(f, "This color is turned into black");
  AColor_knob(f, whitepoint, IRange(0, 4), "whitepoint");
  Tooltip(f, "This color is turned into white");
  AColor_knob(f, black, IRange(-1, 1), "black", "lift");
  Tooltip(f, "Black is turned into this color");
  AColor_knob(f, white, IRange(0, 4), "white", "gain");
  Tooltip(f, "White is turned into this color");
  AColor_knob(f, multiply, IRange(0, 4), "multiply");
  Tooltip(f, "Constant to multiply result by");
  AColor_knob(f, add, IRange(-1, 1), "add", "offset");
  Tooltip(f, "Constant to add to result (raises both black & white, unlike lift)");
  AColor_knob(f, gamma, IRange(0.2, 5), "gamma");
  Tooltip(f, "Gamma correction applied to final result");
  Bool_knob(f, &reverse, "reverse");
  SetFlags(f, Knob::STARTLINE);
  Tooltip(f, "Invert the math to undo the correction");
  Bool_knob(f, &black_clamp, "black_clamp", "black clamp");
  Tooltip(f, "Output that is less than zero is changed to zero");
  Bool_knob(f, &white_clamp, "white_clamp", "white clamp");
  Tooltip(f, "Output that is greater than 1 is changed to 1");
}

const char* GradeIop::gpuEngine_decl() const
{
  if (nodeContext() != eTimeline)
    return nullptr;
  
  // If all the gamma components are not guaranteed to be 1 then we need to upload their
  // values to a uniform - there's little point baking non-animated values into the shader
  // as we'll have to pay for the pow() either way.
  if (!isGammaConstantOne()) {
    return
      "uniform vec4 $$A; \n"
      "uniform vec4 $$B; \n"
      "uniform vec4 $$gamma; \n"  
    ;
  }
  else {
    return
      "uniform vec4 $$A; \n"
      "uniform vec4 $$B; \n"
    ;
  }
}

bool GradeIop::isGammaConstantOne() const
{
  DD::Image::Knob* gamma = knob("gamma");

  // we can use pow optimisation if gamma isn't animated and all the 4 components of the knob are 1

  if (gamma->is_animated()) {
    return false;
  }

  const int kNumComponents = 4;
  for (size_t i = 0; i < kNumComponents; ++i) {
    if (gamma->get_value(i) != 1.0) {
      return false;
    }
  }

  return true;
}

const char* GradeIop::gpuEngine_body() const
{
  if ( nodeContext() != eTimeline)
    return nullptr;
  
  // We're using uniforms for the linear inpterpolation terms A and B, which contain the dependency on the
  // whitepoint, blackpoint, white, black, multiply and add knobs. Uniforms are used to allow animation of
  // the knobs. The usage of A and B in the shader is very simple and I've discerned no performance advantage
  // to baking the values into the shader source when they're not animated.
  // Note that the values of A and B are assumed to be set apparopriately according to whether we're applying
  // a forward or reverse grade, i.e. the shader does not manipulate them before applying a reverse grade.
  //
  // The following knobs always have their values baked into the source:
  //    reverse, black_clamp and white_clamp
  // This is because they are only bools so even if they're animated there will only be a small number of variations
  // of the source code and baking the values into the source allows the compiler to do some noticeable optimisation,
  // especially with reverse baked in as that completely eliminates half the code.
  // Note that the knobs that affect the source need to be take into account by gpuEngine_shader_hash_at.
  
  // NOTE: GLSL 1.2, which is our current minimum spec due to Snow Leopard being GL 2.1, means we don't have
  // mix(T, T, bvec), hence the casting of bvec to vec4 in various places.
  
  std::stringstream shaderText;
  
  if (reverse == false) {
    //
    // The forward case
    //
    
    shaderText <<
    
    "  { \n"
    
    // Save the inpt alpha value so we can simply restore it at the end rather than trying to make sure that every
    // part of the processing leaves it untouched.
    "    float inputAlpha = OUT.a; \n"
    
    //
    // Linear interpolation
    //
    "    vec4 fwd_out = OUT * $$A + $$B;\n"
    
    //
    // Apply clamping
    //
    "    fwd_out = mix( fwd_out, vec4(0.0), (vec4(1.0) - clamp(sign(fwd_out), vec4(0.0), vec4(1.0))) * float($black_clamp$) );\n"
    "    fwd_out = mix( fwd_out, vec4(1.0), clamp(sign(fwd_out - vec4(1.0)), vec4(0.0), vec4(1.0)) * float($white_clamp$) );\n"
    ;

    //
    // Apply gamma.
    //    
    if (!isGammaConstantOne()) {
      
      // At least one of the gamma components is not 1 or is animated so we can't exclude the gamma code.
      
      shaderText <<
      
      // If the (clamped) input colour is zero or negative then the pow() call will yield undefined results, I get
      // NaNs on my machine. The code futher will select fwd_out in that case but since it's using a mix() the linear
      // interpolation would invole the undefined result of the pow() and so itself be undefined.
      // So, if the input is <= 0 we just pass 1 into the pow() call so we get a valid result, which we can
      // subsequently safely 'mix out' ignore.
      "    vec4 inputToPow = mix(fwd_out, vec4(1.0), vec4(lessThanEqual(fwd_out, vec4(0.0)))); \n"
      
      // TODO
      // The CPU calculation uses 1/gamma as the exponent in the pow() and also as a scale if not using the pow().
      // If gamma is exactly zero this results in infinity, which we want to avoid - see my comment for kPlusInfinity.
      // Since, if gamma is zero, we're not going to select the results calculated assuming positive gamma anyway,
      // the value we use for 1/gamma doesn't matter so long as it doesn't result in infinity or NaN creeping
      // into the final answer.
      "    vec4 gammaForDenom = mix($$gamma, vec4(1.0), vec4(equal($$gamma, vec4(0.0)))); \n"
      "    vec4 oneOverGamma = 1.0 / gammaForDenom; \n"
            
      // Calculate the value assuming gamma is strictly positive.
      //
      // First calculate the values depending on if the value is < or > 1, select the appropriate value, then select
      // either the original value or the gamma-applied one depending on whether the value was negative.
      // Result if value < 1
      "    vec4 fwd_ltone_with_gamma = pow(inputToPow, oneOverGamma);\n"
      // Result if value > 1
      "    vec4 fwd_gtone_with_gamma = vec4(1.0) + (fwd_out - vec4(1.0)) * oneOverGamma;\n"
      // Select from the above based on the value being < > 1
      "    vec4 withPositiveGamma = mix(fwd_ltone_with_gamma, fwd_gtone_with_gamma, clamp(sign(fwd_out - vec4(1.0)), vec4(0.0), vec4(1.0)));\n"
      // Select from the above based on the value being < > 0
      "    withPositiveGamma = mix(fwd_out, withPositiveGamma, clamp(sign(fwd_out), vec4(0.0), vec4(1.0)));\n"
      
      // TODO
      // The CPU path uses infinity but we're not going to do that here for a few reasons, at least for now:
      // - Not all GPUs may be sufficiently IEEE 754 compliant for this to be well defined.
      // - We still don't see '+inf' in the colour picker but something around 65500, presumably because of issues
      //   related to the half float render targets Hiero uses, or maybe what we're doing with the read-back?
      // - As described above, subsequent calculatons seem to result in NaNs.
      // So, for now just use a pretty large number. Note that I'm deliberately using something well within half
      // float representation.
      // NOTE: This means the final results can differ between the GPU and CPU paths. I've not seen obvious
      // discrepencies in the rendered image (even with zero gamma and super whites) but the colour sampler
      // will report different values, e.g. 'inf' for the CPU path but ~10,000 for the GPU path.
      "    const vec4 kPlusInfinity = vec4(9999.0); \n"
      
      // Now do the calculations needed if any of the gamma components are negative or zero.
      //
      // If the pre-gamma value input is < 1 then set it to 0, if it's > 1 set it to +inf, otherwise leave it with its value of 1.
      "    vec4 withNegativeGamma = step(vec4(1.0), fwd_out); \n" // Sets 0 and 1 for input < 1 and  >= 1, respectively.
      "    bvec4 inputGreaterThan1 = greaterThan(fwd_out, vec4(1.0)); \n"
      "    withNegativeGamma = mix(withNegativeGamma, kPlusInfinity, vec4(inputGreaterThan1)); \n"  // Sets +inf for input > 1.
      
      // Select the appropriate set of values according to the gamma sign, on a component-wise basis.
      "    fwd_out = mix(withPositiveGamma, withNegativeGamma, vec4(lessThanEqual($$gamma, vec4(0.0)))); \n"
      ;
    }
    
    shaderText <<

    // Ensure we pass on the original alpha value.
    "    OUT = vec4(fwd_out.rgb, inputAlpha); \n"
    "  } \n"
    ;
  }
  else {
    
    //
    // The reverse case
    //
    
    shaderText <<
    
    "  { \n"

    // Save the inpt alpha value so we can simply restore it at the end rather than trying to make sure that every
    // part of the processing leaves it untouched.
    "    float inputAlpha = OUT.a; \n"
    
    "    vec4 rev_out = OUT; \n"
    ;
    
    //
    // Apply gamma
    //
    if (!isGammaConstantOne()) {
      
      // At least one of the gamma components is not 1 or is animated so we can't exclude the gamma code.
      
      shaderText <<
      
      // If the (clamped) input colour is zero or negative then the pow() call will year undefined results, I get
      // nans on my machine. The code futher will select fwd_out in that case but since it's using a mix() the linear
      // interpolation would invole the undefined result of the pow() and so itself be undefined.
      // So, if the input is <= 0 we just pass 1 into the pow() call so we get a valid result, which we can
      // subsequently safely 'mix out' ignore.
      "    vec4 inputToPow = mix(rev_out, vec4(1.0), vec4(lessThanEqual(rev_out, vec4(0.0)))); \n"      
      
      // Calculate the value assuming gamma is strictly positive.
      //
      // Result if value < 1
      "    vec4 rev_ltone_with_gamma = pow(inputToPow, $$gamma);\n"
      // Result if value > 1
      "    vec4 rev_gtone_with_gamma = vec4(1.0) + (rev_out - 1.0) * $$gamma;\n"
      // Select from the above based on the value being < > 1
      "    vec4 rev_with_gamma = mix(rev_ltone_with_gamma, rev_gtone_with_gamma, clamp(sign(rev_out - vec4(1.0)), vec4(0.0), vec4(1.0)));\n"
      // Select from the above based on the value being < > 0
      "    vec4 withPositiveGamma = mix(rev_out, rev_with_gamma, clamp(sign(rev_out), vec4(0.0), vec4(1.0)));\n"
      
      // Now do the calculations needed if any of the gamma components are negative or zero.
      //
      // If the pre-gamma value input is > 0 then we set the output value to 1, otherwise we set it to 0.
      "    vec4 withNegativeGamma = mix(vec4(0.0), vec4(1.0), vec4(greaterThan(rev_out, vec4(0.0)))); \n"
      
      // Select the appropriate set of values according to the gamma sign, on a component-wise basis.
      "    rev_out = mix(withPositiveGamma, withNegativeGamma, vec4(lessThanEqual($$gamma, vec4(0.0)))); \n"
      ;
    }
    
    shaderText <<
    
    //
    // Reverse linear
    //
    "    rev_out = rev_out * $$A + $$B;\n"
    
    //
    // Apply clamping
    //
    "    rev_out = mix( rev_out, vec4(0.0), (vec4(1.0) - clamp(sign(rev_out), vec4(0.0), vec4(1.0))) * float($black_clamp$) );\n"
    "    rev_out = mix( rev_out, vec4(1.0), clamp(sign(rev_out - vec4(1.0)), vec4(0.0), vec4(1.0)) * float($white_clamp$) );\n"
    
    // Ensure we pass on the original alpha value.
    "    OUT = vec4(rev_out.rgb, inputAlpha); \n"
    "  } \n"
    ;
  }
  
  _shaderBodyText = shaderText.str();

  return _shaderBodyText.c_str();
}

Hash GradeIop::gpuEngine_shader_hash_at(double time)
{
  Hash hash;
  
  // We just need to take a hash of the few knobs that affect the source code, evaluated at the
  // specified time.
  hash.append(knob("reverse")->get_value_at(time));
  hash.append(knob("black_clamp")->get_value_at(time));
  hash.append(knob("white_clamp")->get_value_at(time));
  hash.append(isGammaConstantOne());
  
  return hash;
}

// TODO - This gets (almost) duplicated in Text2.cpp and possibly elsewhere - centralise somewhere.
static void BindKnobVec4(Iop* iop, GPUContext* gpuContext, const char* knobNameWithDollars, float alpha)
{
  const int kNumComponents = 4;
  const int kNumVectors = 1;
  float vec4[kNumComponents] = { 0.0f, 0.0f, 0.0f, alpha };
  
  const char* knobName = knobNameWithDollars + 2;
  DD::Image::Knob* knob = iop->knob(knobName);
  for (int i = 0; i < kNumComponents; ++i)
    vec4[i] = static_cast<float>(knob->get_value(i));
  
  // The alpha values of the knobs are ignored and the fourth components are instead hard-coded to
  // values that will cause the alpha to be passed straight through unaffected, as
  // soft effects currently only affect RGB.
  // This hackette was being done directly in the shader code but now we're using uniforms we handle
  // it cpu-side (whether we use 0 or 1 depends on the knob, hance the need for an argument here).
  // TODO: add a channels knob to the Grade soft effect and update this accordingly.
  vec4[3] = alpha;
  
  bool result = gpuContext->bind(knobNameWithDollars, kNumComponents, kNumVectors, vec4);
  mFnAssert(result);
}

void GradeIop::gpuEngine_GL_begin(GPUContext* context)
{
  if (nodeContext() != eTimeline)
    return;
  
  // Calculate the velues of the linear interpolation coofficients, as in pixel_engine(), then update
  // the associated shader uniforms.
  
  float A[4] = { 0.0f };
  float B[4] = { 0.0f };
  
  // TODO
  // For now we're assuming rgba data so hard-code the alpha components of A and B to 1 and 0, respoectively,
  // so we don't end up affecting the alpha part of the image. In the long run we need some kind of channel
  // selection in the GPU path.
  A[3] = 1.0f;
  B[3] = 0.0f;
  
  for (int i = 0; i < 3; ++i) {
    float a = whitepoint[i] - blackpoint[i];
    a = a ? (white[i] - black[i]) / a : 10000.0f;
    a *= multiply[i];
    
    float b = add[i] + black[i] - blackpoint[i] * a;
    
    // If we're doing a reverse grade then the shader code assumes the A and B uniforms have already been
    // modified appropriately for direct application.
    if (reverse) {
      if (a != 1.0f || b) {
        if (a)
          a = 1.0f / a;
        else
          a = 1.0f;
        b = -b * a;
      }
    }
    
    A[i] = a;
    B[i] = b;
  }

  context->bind("$$A", 4, 1, A);
  context->bind("$$B", 4, 1, B);
  
  if (!isGammaConstantOne()) {
    BindKnobVec4(this, context, "$$gamma", 1.0f); // See also the equivalent baking of gamma.a = 1 in gpuEngine_body().
  }
}

void GradeIop::gpuEngine_GL_end(GPUContext* context)
{
  if (nodeContext() != eTimeline)
    return;
}

static Iop* build(Node* node)
{
  return (new NukeWrapper(new GradeIop(node)))->channelsRGBoptionalAlpha()->mixLuminance();
}
const Iop::Description GradeIop::d(CLASS, "Color/Correct/Grade", build);
