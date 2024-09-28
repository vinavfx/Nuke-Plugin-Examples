// Copyright (c) 2011 The Foundry Visionmongers Ltd.  All Rights Reserved.

#include "DDImage/DeepWriter.h"
#include "DDImage/DeepOp.h"

#include <stdint.h>
#include <stdio.h>

namespace Nuke {
  namespace Deep {

    using namespace DD::Image;

    /**
     * This is a plugin to read 'Crude Deep Format' files.  NOTE THAT THIS FORMAT
     * IS NOT INTENDED FOR ACTUAL USE.  It is intended as an example and for testing
     * only, in the absence of any other deep data formats suitable for this purpose.
     *
     * The CDF format consists of
     *
     *  int32_t bbox l
     *  int32_t bbox b (ascending is upwards)
     *  int32_t bbox r
     *  int32_t bbox t
     *  int32_t channel mask (as per Mask_ values in DDImage/Channel.h)
     *  int64_t[size] offsets
     * 
     * The offsets table contains offsets within the file for each line between b and t.
     * Pixel data is according to the following layout:
     * 
     *  int32_t sampleCount
     *
     *  float sample 0 chan 0
     *  float sample 0 chan 1
     *  ...
     *  float sample 0 chan n
     *  float sample 1 chan 0
     *  float sample 1 chan 1
     *  ...
     *  float sample 1 chan n
     *  ...
     *  float sample n chan 0
     *  float sample n chan 1
     *  ...
     *  float sample n chan n
     *
     */
    class cdfWriter : public DeepWriter
    {
    public:
      cdfWriter(DeepWriterOwner* o) : DeepWriter(o) { }

      /** helper function for writing out an int32_t */
      static void writeInt32(FILE* f, int32_t i)
      {
        fwrite(&i, sizeof(i), 1, f);
      }

      /** helper function for writing out an int64_t */
      static void writeInt64(FILE* f, int64_t i)
      {
        fwrite(&i, sizeof(i), 1, f);
      }

      /** helper function for writing out a float */
      static void writeFloat(FILE* f, float flt)
      {
        fwrite(&flt, sizeof(flt), 1, f);
      }

      /**
       * definition of knobs.  Only knob is warning not to use this format
       * in production
       */
      void knobs(Knob_Callback f)
      {
        Text_knob(f, "NOTE: This format is intended as an example for developers\nand for internal testing only.  DO NOT USE.");
      }

      void execute()
      {
        input()->validate(true);
        const DeepInfo& di = input()->deepInfo();

        const DD::Image::ChannelSet& channels = di.channels();

        input()->deepRequest(di.box(), channels);
        input()->fillCache();

        FILE* f = openFile();
        if (!f)
          return;

        const int l = di.box().x();
        const int r = di.box().r();
        const int b = di.box().y();
        const int t = di.box().t();

        DD::Image::ChannelSet writingChannels = Mask_RGBA | Mask_Deep;
        writingChannels &= channels;

        writeInt32(f, l);
        writeInt32(f, b);
        writeInt32(f, r);
        writeInt32(f, t);
        writeInt32(f, writingChannels.value());

        for (int y = b; y < t; y++) {
          DeepPlane plane;
          if (!input()->deepEngine(y, l, r, channels, plane))
            return;

          size_t psc = 0;

          for (int x = l ; x < r; x++) {
            DeepPixel pixel = plane.getPixel(y, x);
            psc += pixel.getSampleCount();
          }

          writeInt64(f, psc);
        }

        for (int y = b; y < t; y++) {
          DeepPlane plane;
          if (!input()->deepEngine(y, l, r, channels, plane))
            return;

          for (int x = l ; x < r; x++) {
            DeepPixel pixel = plane.getPixel(y, x);
            writeInt32(f, pixel.getSampleCount());
            for (int i = 0; i < pixel.getSampleCount(); i++) {
              foreach(z, writingChannels) {
                writeFloat(f, pixel.getUnorderedSample(i, z));
              }
            }
          }
        }

        closeFile(f);
      }
    };

    static DeepWriter* build(DeepWriterOwner* iop)
    {
      return new cdfWriter(iop);
    }

    static const DeepWriter::Description d("cdf\0", build);
  }
}
