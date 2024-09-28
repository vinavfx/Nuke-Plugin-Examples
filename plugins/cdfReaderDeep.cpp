// Copyright (c) 2011 The Foundry Visionmongers Ltd.  All Rights Reserved.

#include "DDImage/DeepReader.h"
#include "DDImage/DeepPlane.h"

#include "DDImage/Knobs.h"
#include "DDImage/Thread.h"

#include <stdint.h>
#include <stdio.h>

using namespace DD::Image;

class cdfDeepReaderFormat : public DeepReaderFormat
{
  void append(Hash& h) override
  {
  }

  void knobs(Knob_Callback f) override
  {
    Text_knob(f, "NOTE: This format is intended as an example for developers\nand for internal testing only.  DO NOT USE.");
  }
};

/**
 * This is a plugin to read 'Crude Deep Format' files.  NOTE THAT THIS FORMAT
 * IS NOT INTENDED FOR ACTUAL USE.  It is intended as an example and for testing
 * only, in the absence of any other deep data formats suitable for this purpose.
 * 
 * see cdfDeepWriter for details of the file layout.
 */
class cdfDeepReader : public DeepReader
{
  FILE* _file;
  std::vector<off_t> _lineOffsets;
  Lock _fileLock;
  DD::Image::Box _fileBox;
  DD::Image::ChannelSet _fileChannels;

public:
  static int32_t readInt32(FILE* f)
  {
    int32_t i;
    fread(&i, sizeof(i), 1, f);
    return i;
  }

  static int64_t readInt64(FILE* f)
  {
    int64_t i;
    fread(&i, sizeof(i), 1, f);
    return i;
  }

  static float readFloat(FILE* f)
  {
    float flt;
    fread(&flt, sizeof(flt), 1, f);
    return flt;
  }

  static std::vector<float> readFloat(FILE* f, size_t fltCount)
  {
    std::vector<float> flt(fltCount);
    fread(&flt[0], sizeof(float), fltCount, f);
    return flt;
  }

  cdfDeepReader(DeepReaderOwner* op, const std::string& filename) : DeepReader(op)
  {
    _file = fopen(filename.c_str(), "rb");
    if (!_file) {
      _op->error("cannot open %s", filename.c_str());
      return;
    }

    int l = readInt32(_file);
    int b = readInt32(_file);
    int r = readInt32(_file);
    int t = readInt32(_file);
    _fileChannels = ChannelSetInit(readInt32(_file));

    _fileBox = DD::Image::Box(l, b, r, t);

    setInfo(r - l, t - b, OutputContext(), _fileChannels);
    
    const int ht = t - b;

    _lineOffsets.push_back(sizeof(int32_t) * 5 + ht * sizeof(int64_t));

    const int wd = r - l;

    for (int y = b; y < t; y++) {
      const int64_t linesamplecount = readInt64(_file);
      const int64_t linesize = wd * sizeof(int32_t) + linesamplecount * sizeof(float) * _fileChannels.size();
      _lineOffsets.push_back(_lineOffsets.back() + linesize);
    }
  }

  ~cdfDeepReader() override
  {
    if (_file) {
      fclose(_file);
    }
  }

  void open(const std::string& filename)
  {
  }

  bool doDeepEngine(DD::Image::Box box, const ChannelSet& channels, DeepOutputPlane& plane) override
  {
    if (!_file) {
      _op->error("missing file");
      return false;
    }

    Guard g(_fileLock);

    plane = DeepOutputPlane(channels, box);

    for (int y = box.y(); y < box.t(); y++) {
      if (y < _fileBox.y() || y >= _fileBox.t()) {
        for (int ox = box.x(); ox != box.r(); ox++) {
          plane.addHole();
        }
        continue;
      }

      int oidx = y - _fileBox.y();
      assert(oidx >= 0 && oidx < _lineOffsets.size());

      off_t o = _lineOffsets[oidx];
      if (fseek(_file, o, SEEK_SET) == -1)
        _op->error("corrupt file");

      int minX = std::min(_fileBox.x(), box.x());
      int maxX = std::max(_fileBox.r(), box.r());
      
      for (int x = minX; x < maxX; x++) {
        if (x < _fileBox.x() || x >= _fileBox.r()) {
          plane.addHole();
          continue;
        }
        
        int sample = readInt32(_file);
        if (sample == 0) {
          if (x >= box.x() || x < box.r())
            plane.addHole();
          continue;
        }

        assert(sample <= 10000);
        std::vector<float> flts = readFloat(_file, sample * _fileChannels.size());

        if (x < box.x() || x >= box.r())
          continue;

        DeepOutPixel dop;
        int fltidx = 0;
        for (int i = 0; i < sample; i++) {
          DD::Image::ChannelSet allChans = _fileChannels + channels;
          foreach(z, allChans) {
            if (channels.contains(z)) {
              if (_fileChannels.contains(z)) {
                dop.push_back(flts[fltidx]);
              } else if (z == Chan_Alpha) {
                dop.push_back(1);
              } else {
                dop.push_back(0);
              }
            }
            if (_fileChannels.contains(z))
              fltidx++;
          }
        }
        plane.addPixel(dop);
      }
    }
    return true;
  }
};

static DeepReader* build(DeepReaderOwner* op, const std::string& fn)
{
  return new cdfDeepReader(op, fn);
}
static DeepReaderFormat* buildformat(DeepReaderOwner* op)
{
  return new cdfDeepReaderFormat();
}

static const DeepReader::Description d("cdf\0", "cdf", build, buildformat);

