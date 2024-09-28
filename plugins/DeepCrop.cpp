// Copyright (c) 2011 The Foundry Visionmongers Ltd.  All Rights Reserved.

#include "DDImage/DeepFilterOp.h"
#include "DDImage/Knobs.h"

static const char* CLASS = "DeepCrop";

using namespace DD::Image;

class DeepCrop : public DeepFilterOp
{

  float _zrange[2];
  bool _useZMin;
  bool _useZMax;
  bool _outsideZRange;

  float _bbox[4];
  bool _useBBox;
  bool _outsideBBox;

public:
  DeepCrop(Node* node) : DeepFilterOp(node) {
    _zrange[0] = 1;
    _zrange[1] = 2;

    _bbox[0] = _bbox[2] = input_format().width();
    _bbox[1] = _bbox[3] = input_format().height();

    _bbox[0] *= 0.2;
    _bbox[1] *= 0.2;

    _bbox[2] *= 0.8;
    _bbox[3] *= 0.8;

    _useZMin = true;
    _useZMax = true;
    _useBBox = true;

    _outsideZRange = false;
    _outsideBBox = false;
  }

  const char* node_help() const override
  {
    return "Crop deep data in front of or behind certain planes, or inside or outside of a box.";
  }

  const char* Class() const override {
    return CLASS;
  }

  Op* op() override
  {
    return this;
  }

  void knobs(Knob_Callback f) override
  {
    Float_knob(f, &_zrange[0], "znear");
    SetRange(f, 0, 1);
    Tooltip(f, "The near deep value");

    Bool_knob(f, &_useZMin, "use_znear", "use");
    Tooltip(f, "Whether to use the near deep value");

    Float_knob(f, &_zrange[1], "zfar");
    SetRange(f, 0, 1);
    Tooltip(f, "The far deep value");

    Bool_knob(f, &_useZMax, "use_zfar", "use");
    Tooltip(f, "Whether to use the far deep value");

    Bool_knob(f, &_outsideZRange, "outside_zrange", "keep outside zrange");
    Tooltip(f, "Whether to keep the samples with deep between the range (false), or outside the range (true)");

    BBox_knob(f, &_bbox[0], "bbox");
    SetFlags(f, Knob::ALWAYS_SAVE);
    Tooltip(f, "2D bounding box for clipping");

    Bool_knob(f, &_useBBox, "use_bbox", "use");
    Tooltip(f, "Whether to use the 2D bounding box");

    Bool_knob(f, &_outsideBBox, "outside_bbox", "keep outside bbox");
    Tooltip(f, "Whether to keep samples within the 2D bounding box (false), or outside it (true)");
  }

  int knob_changed(DD::Image::Knob* k) override
  {
    knob("znear")->enable(_useZMin);
    knob("zfar")->enable(_useZMax);
    knob("bbox")->enable(_useBBox);
    return 1;
  }

  void _validate(bool for_real) override
  {
    DeepFilterOp::_validate(for_real);
    if (_useBBox && !_outsideBBox) {
      _deepInfo.box().set(floor(_bbox[0] - 1), floor(_bbox[1] - 1), ceil(_bbox[2] + 1), ceil(_bbox[3] + 1));
    }
  }

  bool isCropped(DeepPixel &pixel, size_t sample, DD::Image::Channel channel)
  {
    float z = pixel.getUnorderedSample(sample, channel);
    bool inZ = (!_useZMax || z <= _zrange[1]) && (!_useZMin || z >= _zrange[0]);
    return ((_useZMin || _useZMax) && inZ == _outsideZRange);
  }

  bool doDeepEngine(DD::Image::Box box, const ChannelSet& channels, DeepOutputPlane& plane) override
  {
    if (!input0())
      return true;

    DeepOp* in = input0();
    DeepPlane inPlane;

    ChannelSet needed = channels;
    needed += Mask_DeepFront;

    if (!in->deepEngine(box, needed, inPlane))
      return false;

    DeepInPlaceOutputPlane outPlane(channels, box);
    outPlane.reserveSamples(inPlane.getTotalSampleCount());

    //samples per pixel after croping
    std::vector<int> validSamples;

    for (DD::Image::Box::iterator it = box.begin(), itEnd = box.end(); it != itEnd; ++it) {

      const int x = it.x;
      const int y = it.y;

      bool inXY = (x >= _bbox[0] && x < _bbox[2] && y >= _bbox[1] && y < _bbox[3]);

      if (_useBBox && inXY == _outsideBBox) {
        // pixel out of crop bounds
        outPlane.setSampleCount(y, x, 0);
        continue;
      }

      DeepPixel inPixel = inPlane.getPixel(it);
      size_t inPixelSamples = inPixel.getSampleCount();

      validSamples.clear();
      validSamples.reserve(inPixelSamples);

      // find valid samples
      for (size_t iSample = 0; iSample < inPixelSamples; ++iSample) {

        if (isCropped(inPixel, iSample, DD::Image::Chan_DeepFront))
          continue;

        if (isCropped(inPixel, iSample, DD::Image::Chan_DeepBack))
          continue;

        validSamples.push_back(iSample);
      }

      outPlane.setSampleCount(it, validSamples.size());

      DeepOutputPixel outPixel = outPlane.getPixel(it);

      // copy valid samples to DeepOutputPlane
      size_t outSample = 0;
      for (size_t inSample : validSamples)
      {
        const float* inData = inPixel.getUnorderedSample(inSample);
        float* outData = outPixel.getWritableUnorderedSample(outSample);
        for ( int iChannel = 0, nChannels = channels.size();
              iChannel < nChannels;
              ++iChannel, ++outData, ++inData
              ) {
          *outData = *inData;
        }
        ++outSample;
      }
    }

    outPlane.reviseSamples();
    mFnAssert(outPlane.isComplete());
    plane = outPlane;

    return true;
  }

  static const Description d;
  static const Description d2;
  static const Description d3;
};

static Op* build(Node* node) { return new DeepCrop(node); }
const Op::Description DeepCrop::d(::CLASS, "Image/DeepCrop", build);
const Op::Description DeepCrop::d2("DeepMask", "Image/DeepCrop", build);
const Op::Description DeepCrop::d3("DeepClip", "Image/DeepCrop", build);
