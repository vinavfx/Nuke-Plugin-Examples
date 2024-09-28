#include "DDImage/Iop.h"
#include "DDImage/Row.h"
#include "DDImage/DeepOp.h"
#include "DDImage/Knobs.h"
#include "DDImage/Pixel.h"
#include "DDImage/DeepComposite.h"

const char* CLASS = "DeepToImage";

using namespace DD::Image;

class DeepToImage : public DD::Image::Iop
{
  bool _volumetricComposition;
  
public:
  DeepToImage(Node* node) : Iop(node)
  {
    _volumetricComposition = true;
  }

  Op* default_input(int idx) const override
  {
    return nullptr;
  }

  int minimum_inputs() const override
  {
    return 1;
  }

  int maximum_inputs() const override
  {
    return 1;
  }

  bool test_input(int idx, Op*op) const override
  {
    return dynamic_cast<DeepOp*>(op);
  }

  DeepOp* input0()
  {
    return dynamic_cast<DeepOp*>(Op::input(0));
  }

  const char* node_help() const override
  {
    return "Composites deep data to a 2D image.";
  }

  const char* Class() const override {
    return CLASS;
  }

  const char* node_shape() const override
  {
    return DeepOp::DeepNodeShape();
  }

  void knobs(Knob_Callback f) override
  {
    Bool_knob(f, &_volumetricComposition, "volumetric_composition", "volumetric composition");
    Tooltip(f, "With this selected, Nuke will take into account the back depth of samples for compositing into 2D, rather than only using the front depth and assuming samples do not overlap.");
  }

  void _validate(bool r) override
  {
    if (input0()) {
      input0()->validate(true);
      DeepInfo deepInfo = input0()->deepInfo();

      DD::Image::Box realBox = deepInfo.box();

      info_.setFormats(deepInfo.formats());

      info_.set(realBox);
      info_.channels(deepInfo.channels() + Mask_Z);

      info_.first_frame(deepInfo.firstFrame());
      info_.last_frame(deepInfo.lastFrame());

      set_out_channels(deepInfo.channels() + Mask_Z);

      input0()->op()->cached(cached());
   } else {
      info_.set(DD::Image::Box());
      info_.channels(Mask_None);
    }
  }

  void getNeededChannels(ChannelSet& channels) const
  {
    if (_volumetricComposition || channels.contains(Chan_Z))
      channels += Mask_DeepBack;

    channels += Mask_DeepFront | Mask_Alpha;

    channels -= Mask_Z;
  }

  void _request(int x, int y, int r, int t, ChannelMask channels, int count) override
  {
    ChannelSet reqchan = channels;
    getNeededChannels(reqchan);
    input0()->deepRequest(DD::Image::Box(x, y, r, t), reqchan, count);
  }

  void engine(int y, int x, int r, const DD::Image::ChannelSet& cs, DD::Image::Row& row) override
  {
    DeepOp* deepIn = input0();

    if (!deepIn) {
      foreach(z, cs) {
        row.erase(z);
      }
      return;
    }

    bool doingZ = cs.contains(DD::Image::Mask_Z);
    bool doingDeepFront = cs.contains(DD::Image::Mask_DeepFront);
    bool doingDeepBack = cs.contains(DD::Image::Mask_DeepBack);

    DD::Image::DeepPlane deepRow;

    DD::Image::ChannelSet engineChans = cs;
    getNeededChannels(engineChans);

    if (!deepIn->deepEngine(y, x, r, engineChans, deepRow)) {
      Iop::abort();
      foreach(z, cs) {
        row.erase(z);
      }
      return;
    }

    DD::Image::ChannelSet chans = cs;
    chans -= Mask_Z;
    chans -= Mask_Deep;
    
    foreach(z, chans) {
      for (int i = x; i < r; i++) {
        row.writable(z)[i] = 0;
      }
    };

    while (x < r) {
      
      float* Zpix = doingZ ? (row.writable(Chan_Z) + x) : nullptr;
      float* Zfrontpix = doingDeepFront ? (row.writable(Chan_DeepFront) + x) : nullptr;
      float* Zbackpix = doingDeepBack ? (row.writable(Chan_DeepBack) + x) : nullptr;
      
      if (Zpix)
        *Zpix = 0;
      
      if (Zfrontpix)
        *Zfrontpix = INFINITY;
      
      if (Zbackpix)
        *Zbackpix = INFINITY;
      
      DeepPixel deepPixel = deepRow.getPixel(y, x);

      DD::Image::ChannelSet requiredChannels = DD::Image::Mask_DeepFront | DD::Image::Mask_Alpha;
      if (_volumetricComposition) {
        requiredChannels += DD::Image::Mask_DeepBack;
      }

      const bool isValidDeepPixel = deepPixel.channels().containsAll(requiredChannels);

      if (isValidDeepPixel) {
        if (_volumetricComposition && DetectOverlappingSamples(deepPixel)) {
          DeepOutPixel samples;
          CombineOverlappingSamples(deepRow.channels(), deepPixel, samples);
          CompositeSamples(samples.getPixel(deepRow.channels(), DeepPixel::eZAscending), chans, row, x, Zpix, Zfrontpix, Zbackpix);
        }
        else {
          CompositeSamples(deepPixel, chans, row, x, Zpix, Zfrontpix, Zbackpix);
        }
      }

      x++;
    }
  }

  int getViewableModes() const override
  {
    return eViewableMode2D;
  }

  OpHints opHints() const override
  {
    return OpHints::eChainable;
  }

  static const Description d;
  static const Description d2;
};

static Op* build(Node* node) { return new DeepToImage(node); }

const Op::Description DeepToImage::d(::CLASS, "Image/DeepToImage", build);
const Op::Description DeepToImage::d2("FromDeep", "Image/DeepToImage", build);
