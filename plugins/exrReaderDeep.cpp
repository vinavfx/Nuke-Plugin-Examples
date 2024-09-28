// exrReaderDeep.cpp

///////////////////////////////////////////////////////////////////////////
//
// Copyright (c) 2011 The Foundry Visionmongers Ltd.  All Rights Reserved.
// Portions contributed and copyright held by others as indicated.  All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
//    * Redistributions of source code must retain the above
//      copyright notice, this list of conditions and the following
//      disclaimer.
//
//    * Redistributions in binary form must reproduce the above
//      copyright notice, this list of conditions and the following
//      disclaimer in the documentation and/or other materials provided with
//      the distribution.
//
//    * Neither the name of The Foundry Visionmongers nor any other contributors 
//      to this software may be used to endorse or promote products derived from 
//      this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS
// IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
// THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
// PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR
// CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
// EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
// PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
// LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
// NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
// SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//
///////////////////////////////////////////////////////////////////////////


#include <OpenEXR/ImfMultiPartInputFile.h>
#include <OpenEXR/ImfDeepScanLineInputPart.h>
#include <OpenEXR/ImfDeepFrameBuffer.h>

#include <OpenEXR/ImfPartType.h>
#include <OpenEXR/ImfChannelList.h>

#include <OpenEXR/ImfStringAttribute.h>
#include <OpenEXR/ImfIntAttribute.h>
#include <OpenEXR/ImfFloatAttribute.h>
#include <OpenEXR/ImfVecAttribute.h>
#include <OpenEXR/ImfBoxAttribute.h>
#include <OpenEXR/ImfStringVectorAttribute.h>
#include <OpenEXR/ImfTimeCodeAttribute.h>
#include <OpenEXR/ImfStandardAttributes.h>
#include <OpenEXR/ImfMatrixAttribute.h>

#include <OpenEXR/ImfFramesPerSecond.h>

#include <stdio.h>
#include <numeric>

#include "DDImage/DeepReader.h"
#include "DDImage/DeepOp.h"
#include "DDImage/Thread.h"

#include "exrGeneral.h"
#include "ExrChannelNameToNuke.h"

using namespace DD::Image;

class exrDeepReaderFormat : public DeepReaderFormat
{

  bool _doNotAttachPrefix;
  bool _blackOutside;

public:

  exrDeepReaderFormat()
  {
    _doNotAttachPrefix = false;
    _blackOutside = false;
  }

  bool doNotAttachPrefix() const { return _doNotAttachPrefix; }

  bool blackOutside() const
  {
    return _blackOutside;
  }

  void knobs(Knob_Callback f) override
  {
    Bool_knob(f, &_doNotAttachPrefix, "noprefix",  "do not attach prefix");
    Tooltip(f, "By default the 'exr' prefix is attached to metadata keys to make it distinct from other metadata in the tree.  Enable this option to read the metadata 'as is'' without attaching the exr prefix.");

    Bool_knob(f, &_blackOutside, "black_outside", "black outside");
    Tooltip(f, "Enable this option to add black deep pixels outside the data window. This is to avoid edge pixels being repeated when converting to a 2d image.");
  }

  void append(Hash& hash) override
  {
    hash.append(_doNotAttachPrefix);
  }
};

// DeepScanlineBuffer: used for storing a deep scanline per thread, to allow multiple
// engine threads to decompress scanlines in parallel.
class DeepScanlineBuffer 
{
public:
  DeepScanlineBuffer()
    : _deepScanlines()
  {}
  
  // Get the deep scanline buffer to use for the current thread. This function allocates buffers and adds them
  // into a boost::unordered_map, so should only be called by one thread at a time, but it's left to the calling code to 
  // manage locking.
  //
  // If the current thread hasn't asked for a buffer before, this will create a new one. If the buffer already 
  // exists but is too small, this will resize it. In case we don't know the required size yet, sizeNeeded 
  // defaults to 0 so we can delay allocating storage until the size is known.
  const std::vector<char> &getBuffer(const size_t &sizeNeeded = 0)
  {
    // Get the ID of the thread requesting the buffer.
    const my_thread_id_type threadID = getThreadID();

    // Check whether a deep scanline buffer already exists for this thread.
    std::map<my_thread_id_type, std::vector<char> >::iterator it = _deepScanlines.find(threadID);

    if (it != _deepScanlines.end()) {
      // The buffer already exists - resize it if it's not big enough.
      if (it->second.size() < sizeNeeded) {
        it->second.resize(sizeNeeded);
      }
      return it->second;
    }
    else {
      // No buffer was found for this thread - create a new one big enough for the current scanline.
      std::pair<std::map<my_thread_id_type, std::vector<char> >::iterator, bool> returnValue = _deepScanlines.insert(std::make_pair(threadID, std::vector<char>(0)));
      std::vector<char> &buffer = returnValue.first->second;
      buffer.resize(sizeNeeded);
      return buffer;
    }
  }

private:
  // A boost::unordered_map that will contain one deep scanline buffer for each process ID that asks for one.
  std::map<my_thread_id_type, std::vector<char> > _deepScanlines;
 
};

/**
 * OpenEXR2 reader for deep data.
 *
 * Missing features, to be adapted from the existing EXR reader:
 *  stereo
 */
class exrReaderDeep : public DeepReader
{
  std::string _filename;
  ChannelSet _decodeChannels;
  ChannelMap _decodeChannelMap;
  Imf::MultiPartInputFile *_file;
  Imf::DeepScanLineInputPart *_part;
  int _partNumber;
  std::map<Channel, std::string> _chans;
  std::map<Channel, Imf::PixelType> _chanTypes;
  MetaData::Bundle _meta;
  DD::Image::OutputContext _outputContext;
  DeepScanlineBuffer _deepScanlineBuffer;

  Lock _lock;

  void createChannelMap(const Imf::Header& header);

  const char* chanName(Channel chan)
  {
    return _chans[chan].c_str();
  }

  // Helper class for storing scanline buffer and sample counts
  struct LineBufferUtils
  {
    std::vector<char> deepScanlineBuffer;
    std::vector<unsigned> sampleCounts;
    Imf::DeepFrameBuffer frameBuffer;
    unsigned totalSampleCount;
    int exrY;
    int boxY;

    LineBufferUtils()
    : totalSampleCount(0)
    , exrY(0)
    , boxY(0)
    {}

  };

  bool decodeLine(Imf::DeepScanLineInputPart& part, LineBufferUtils& lineBufferUtils, const Box& box, const ChannelSet& reqChannels, DeepInPlaceOutputPlane& plane);
  void setMetaData( const Imf::Header& header, DD::Image::MetaData::Bundle& meta, bool doNotAttachPrefix );
  unsigned countSamples(Imf::DeepScanLineInputPart& part, LineBufferUtils& lineBufferUtils);
  void initScanlineBuffer(Imf::DeepScanLineInputPart& part, LineBufferUtils& lineBufferUtils);
  
public:

  exrReaderDeep(DeepReaderOwner* iop, const std::string& filename);
  ~exrReaderDeep() override;
  
  bool doDeepEngine(Box box, const ChannelSet& reqChannels, DeepOutputPlane& plane) override;
  const MetaData::Bundle& fetchMetaData(const char* key);
};

exrReaderDeep::exrReaderDeep(DeepReaderOwner* iop, const std::string& filename) : DeepReader(iop), _filename(filename)
{
  _file = nullptr;
  _part = nullptr;
  exrDeepReaderFormat *exrOptions = dynamic_cast<exrDeepReaderFormat*>( iop->handler() );
  bool doNotAttachPrefix = exrOptions ? exrOptions->doNotAttachPrefix() : false;

  try {
    _file = new Imf::MultiPartInputFile( filename.c_str() );

    std::string view( iop->readerOutputContext().viewname() );

    int partNumber = -1;

    for(int i=0; i < _file->parts(); i++) {
      const Imf::Header& header = _file->header(i);
      if ( i == 0 )
        setMetaData( header, _metaData, doNotAttachPrefix ); // read 'global' metadata from first part

      // look for a part with a type set to DEEPSCANLINE
      if ( ! header.hasType() || header.type() != Imf::DEEPSCANLINE )
        continue;

      if( header.hasView() ) {
        if( view == header.view() ) {
          partNumber = i;
          break;
        } else {
          // wrong view, but it'll do if all else fails
          partNumber = i;
        }
      } else {
        // no view mentioned - if we wanted main, that's exactly right
        if( view == "main" ) {
          partNumber = i;
          break;
        } else {
          // no view in file - if we aren't using an earlier part, use this one
          if ( partNumber == -1 ) 
            partNumber = i;
        }
      }
    }

    _outputContext = _owner->readerOutputContext();

    if( partNumber == -1) {
      throw Iex::InputExc("no deep data found in file");
    }
    
    _partNumber = partNumber;
    _part = new Imf::DeepScanLineInputPart( *_file, _partNumber);

    const Imf::Header& header = _part->header();

    createChannelMap(header);

    // read part metadata - overrides global metadata

    setMetaData( header, _metaData, doNotAttachPrefix );

    Box displayBox = boxToBox(header.displayWindow(), header.displayWindow());
    Box dataBox = boxToBox(header.dataWindow(), header.displayWindow());

    ChannelSet availableChannels = _decodeChannels;
    if (availableChannels.contains(Mask_DeepFront)) {
      availableChannels += Mask_DeepBack;
    }

    setInfo(displayBox.w(),
            displayBox.h(),
            _outputContext,
            availableChannels);

    // TP 163307 - We need to make sure that the box we set here is equivalent to the data
    // window of the exr, otherwise there will be problems when reading scanline EXRs created
    // from a flattened deep EXR (DeepReed -> DeepToImage -> Write).  The flattened scanline
    // EXR will contain an extra bit of metadata (chunkCount) which, if less than its data
    // window, will cause an error when reading. The adjustments made below are done to match
    // what happens in DeepReader::setInfo before it sets the outputContext/deepInfo boxes. We
    // are doing the same for consistency and to reduce off by one errors that being different
    // was introducing.
    DD::Image::Box adjustedDatabox = dataBox;
    adjustedDatabox.w(dataBox.w() - 1);
    adjustedDatabox.h(dataBox.h() - 1);

    // TP 229434 - As for generic exr, add black pixel around the edge of the box.
    // This avoid to replicate the edge pixels.
    // This is not done if the bounding box matches the display window.
    const bool addEdgePixels = exrOptions->blackOutside() && dataBox != displayBox;

    if (addEdgePixels) {
      int adjustedChunkCount = 0;

      if (dataBox.x() > displayBox.x()) {
        adjustedDatabox.x(adjustedDatabox.x()-1);
      }

      if (dataBox.r() < displayBox.r()) {
        adjustedDatabox.r(adjustedDatabox.r()+1);
      }

      if (dataBox.y() > displayBox.y()) {
        adjustedDatabox.y(adjustedDatabox.y()-1);
        adjustedChunkCount++;
      }

      if (dataBox.t() < displayBox.t()) {
        adjustedDatabox.t(adjustedDatabox.t()+1);
        adjustedChunkCount++;
      }

      // As described before for the fix of TP 163307, the "exr/chunkCount" metadata has to be updated. Otherwise if the chunkCount is less than
      // the data window will cause error when reading back as flat 2d exr image.
      // The "chunkCount" seems to rappresent the number of vertical line in the databox when the exr is stored as scanline format.
      // This seems to be true for all availble compression modes.
      // The exr deep reader doesn't support tile representation, so it's not needed to consider this case.
      // The exr writer doesn't perform any check chunkCount value, it's seems happy to write any value.
      // The error occurs only at reading time in the 2d exr reader.
      MetaData::Bundle::iterator it = _metaData.find("exr/chunkCount");
      const bool updateExrChunkCount = (adjustedChunkCount > 0) && (it != _metaData.end()) && MetaData::isPropertyInt(it->second);

      if ( updateExrChunkCount) {
        _metaData.setData("exr/chunkCount", MetaData::getPropertyInt(it->second, 0) + adjustedChunkCount);
      }
    }

    _outputContext.to_proxy_box(adjustedDatabox);
    _deepInfo.box().set(adjustedDatabox);
  }
  
  catch ( Iex::BaseExc& e ) {
    _owner->readerInternalError( e.what() );
  }
}

exrReaderDeep::~exrReaderDeep()
{
  delete _file;
  delete _part;
}

/**
 * decode the ofx channel names to Nuke channel numbers and 
 * names
 */
void exrReaderDeep::createChannelMap(const Imf::Header& header)
{
  for (Imf::ChannelList::ConstIterator it = header.channels().begin();
       it != header.channels().end();
       it++) {
    
    const char* chanName = it.name();

    ExrChannelNameToNuke exrChannelNameMapper(chanName, std::vector<std::string>());
    std::string nukeName = exrChannelNameMapper.nukeChannelName();

    Channel channel = getExrChannel(nukeName.c_str());
    _chans[channel] = chanName;
    _decodeChannels += channel;
  }
  
  _decodeChannelMap = ChannelMap(_decodeChannels);
}

unsigned exrReaderDeep::countSamples(Imf::DeepScanLineInputPart& part, LineBufferUtils& lineBufferUtils)
{
  const Imf::Header& header = part.header();

  const int dataWid = header.dataWindow().size().x + 1;
  const int dataX = header.dataWindow().min.x;

  std::vector<unsigned>& sampleCounts = lineBufferUtils.sampleCounts;
  sampleCounts.resize(dataWid, 0);

  // Set the slice for the sample counts
  Imf::DeepFrameBuffer& frameBuffer = lineBufferUtils.frameBuffer;
  frameBuffer.insertSampleCountSlice(Imf::Slice(Imf::UINT,
                                                        (char*)(&sampleCounts[0] - dataX),
                                                        sizeof(unsigned), 0));

  // Read the sample counts from the data buffer.
  int exrY = lineBufferUtils.exrY;
  std::vector<char>& deepScanlineBuffer = lineBufferUtils.deepScanlineBuffer;
  part.readPixelSampleCounts(&deepScanlineBuffer[0], frameBuffer, exrY, exrY);

  // Accumulate  all the samples
  unsigned result = std::accumulate(sampleCounts.begin(), sampleCounts.end(), 0);

  // Store the results
  lineBufferUtils.totalSampleCount = result;

  return result;
}

void exrReaderDeep::initScanlineBuffer(Imf::DeepScanLineInputPart& part, LineBufferUtils& lineBufferUtils)
{
  // Only one engine thread should read from the input file at a time.
  Guard g(_lock);

  // rawPixelData works out how much space is required
  // for the scanline and will return without copying anything into the buffer if the space
  // required is larger than the buffer size passed in (pixSize).

  int exrY = lineBufferUtils.exrY;
  std::vector<char>& deepScanlineBuffer = lineBufferUtils.deepScanlineBuffer;

  // Query the number of bytes required to read the chunk
  Imf::Int64 pixSize = 0;
  part.rawPixelData(exrY, nullptr, pixSize);

 // Request a buffer of the required size.
 deepScanlineBuffer.resize((size_t)pixSize);

 // Read the data into the buffer.
 part.rawPixelData(exrY, &deepScanlineBuffer[0], pixSize);

 // Check that the size of the data read matches the size of the buffer; if not, something has gone wrong
 // during the read.
 mFnAssertMsg(pixSize == deepScanlineBuffer.size(), "Buffer size not correct for attempted read of deep scan line.");
}

bool exrReaderDeep::decodeLine(Imf::DeepScanLineInputPart& part, LineBufferUtils& lineBufferUtils, const Box& box, const ChannelSet& reqChannels, DeepInPlaceOutputPlane& plane)
{
  // Skip out if no samples
  if (lineBufferUtils.totalSampleCount == 0) {
    return false;
  }

  const Imf::Header& header = part.header();

  const int dataWid = header.dataWindow().size().x + 1;
  const int dataX = header.dataWindow().min.x;

  unsigned chanCount = _decodeChannelMap.size();
  
  std::vector<std::vector<const float*> > samples;
  samples.reserve(chanCount);

  // set up the slices for the actual data.  Note these pointers
  // don't actually point anywhere yet, they can't be filled in
  // until we have done readPixelSampleCounts (but they need to be
  // present for setFrameBuffer)
  //
  // Due to an apparent bug in the EXR library, we need to decode
  // ALL the channels in the file, even if we aren't interested
  // in them.
  Imf::DeepFrameBuffer& frameBuffer = lineBufferUtils.frameBuffer;
  foreach(z, _decodeChannels) {
    samples.push_back(std::vector<const float*>(dataWid));
    frameBuffer.insert(chanName(z),
                       Imf::DeepSlice(Imf::FLOAT,
                                              (char *)(&samples.back()[0] - dataX),
                                              sizeof(const float*),        // xstride
                                              0,                           // ystride
                                              sizeof(float) * chanCount)); // samplestride
  }

  // allocate the actual data for the samples, and then go back and
  // set the samples vectors that the framebuffer is pointing at
  // to point to spans within this.
  std::vector<float> data;
  data.resize(lineBufferUtils.totalSampleCount * chanCount, 0);

  std::vector<unsigned>& sampleCounts = lineBufferUtils.sampleCounts;
  foreach(z, _decodeChannels) {
    const int chanNo = _decodeChannelMap.chanNo(z);
    const float* ptr = &data[0] + chanNo;

    for (int x = 0; x < dataWid; x++) {
      samples[chanNo][x] = ptr;
      ptr += chanCount * sampleCounts[x];
    }
  }

  // Read the data from the buffer into the frame buffer. This will decompress
  // the raw pixel data if necessary.
  int exrY = lineBufferUtils.exrY;
  std::vector<char>& deepScanlineBuffer = lineBufferUtils.deepScanlineBuffer;
  part.readPixels(&deepScanlineBuffer[0], frameBuffer, exrY, exrY);

  const size_t reqChanSize = reqChannels.size();
  for (int x = box.x(); x < box.r(); x++) {

    float originalFormatX = static_cast<float>(x);
    float originalFormatY = 0.0f;  // We're only interested in the x value here
    _outputContext.from_proxy_xy(originalFormatX, originalFormatY);

    const unsigned lineX = static_cast<int>(originalFormatX) - dataX;

    if (lineX >= sampleCounts.size() || sampleCounts[lineX] == 0)
      plane.setSampleCount(lineBufferUtils.boxY, x, 0); // if we're out of range, add a hole (no data)
    else {
      const unsigned sampleCount = sampleCounts[lineX];
      plane.setSampleCount(lineBufferUtils.boxY, x, sampleCount);
      DeepOutputPixel out = plane.getPixel(lineBufferUtils.boxY, x);
      float* output = out.writable();

      // copy data from the unpacked buffer into the output plane.
      // in some future version it might be nice to unpack directly
      // into the output plane's buffer.  Possibly need some API changes
      // for this.

      foreach(z, reqChannels) {
        Channel sourceChannel = z;
        const size_t channelIdx = plane.channels().chanNo(z);

        // if we don't have separate back data then just copy the front data, which makes
        // all the samples 0-depth (if we didn't do this then the back would end up being 0)
        if (sourceChannel == Chan_DeepBack && !_decodeChannelMap.contains(sourceChannel)) {
          sourceChannel = Chan_DeepFront;
        }

        if (_decodeChannelMap.contains(sourceChannel)) {
          const int chanNo = _decodeChannelMap.chanNo(sourceChannel);
          for (size_t i = 0; i < sampleCount; ++i) {
            output[channelIdx + i*reqChanSize] = samples[chanNo][lineX][i * chanCount];
          }
        }
        else {
          for (size_t i = 0; i < sampleCount; ++i) {
            output[channelIdx + i*reqChanSize] = 0;
          }
        }
      }

    }
  }
  return true;
}

void exrReaderDeep::setMetaData( const Imf::Header& header, DD::Image::MetaData::Bundle& meta, bool doNotAttachPrefix)
{
  std::map<Imf::PixelType, int> pixelTypes;

  for (Imf::ChannelList::ConstIterator it = header.channels().begin();
       it != header.channels().end();
       it++) {

    ExrChannelNameToNuke exrChannelNameMapper(it.name(), std::vector<std::string>());
    std::string nukeName = exrChannelNameMapper.nukeChannelName();

    Channel channel = getExrChannel( nukeName.c_str() );
    if ( channel != Chan_DeepBack && channel != Chan_DeepFront )
      pixelTypes[ it.channel().type ]++;
  }

  if (pixelTypes[Imf::FLOAT] > 0) {
    meta.setData(MetaData::DEPTH, MetaData::DEPTH_FLOAT);
  }
  else if (pixelTypes[Imf::HALF] > 0) {
    meta.setData(MetaData::DEPTH, MetaData::DEPTH_HALF);
  }

  exrHeaderToMetadata( header, meta, doNotAttachPrefix );
}

bool exrReaderDeep::doDeepEngine(Box box, const ChannelSet& reqChannels, DeepOutputPlane& plane)
{
  const Imf::Header& header = _part->header();
  DeepInPlaceOutputPlane outPlane(reqChannels, box);
  plane = outPlane;
  
  std::vector<LineBufferUtils> lineBuffers(box.h());

  auto exrYValid = [&](int exrY) -> bool {
    if (exrY < header.dataWindow().min.y || exrY > header.dataWindow().max.y) {
      return false;
    }

    return true;
  };

  try {

    unsigned totalSamples = 0;
    int currentLine = 0;
    /*
     * The main purpose of this loop is to count samples so we know how much
     * memory we need to allocate for DeepOutputPlane in advance.
     */
    for (int y = box.y(); y < box.t(); ++y, ++currentLine) {
      float originalFormatX = 0.0f;   // We're only interested in Y here
      float originalFormatY = static_cast<float>(y);
      _outputContext.from_proxy_xy(originalFormatX, originalFormatY);

      int exrY = lineToLine(static_cast<int>(originalFormatY), header.displayWindow());
      if (!exrYValid(exrY)) {
        continue;
      }

     /*
      * In order to query sample counts for the scanline, we need
      * to copy raw data into a scanline buffer first. Because 'decodeLine(...)' uses
      * the same data from the scanline buffer and we don't want to copy the raw data twice
      * we need to keep the scanline buffers until 'decodeLine(...)' is called.
      */
      LineBufferUtils& lineBuffer = lineBuffers[currentLine];
      lineBuffer.exrY = exrY;
      lineBuffer.boxY = y;
      initScanlineBuffer(*_part, lineBuffer);
      totalSamples += countSamples(*_part, lineBuffer);
    }

    // Allocate memory for DeepOutputPlane
    outPlane.reserveSamples(totalSamples);

    currentLine = 0;
    for (int y = box.y(); y < box.t(); ++y, ++currentLine) {
      float originalFormatX = 0.0f;   // We're only interested in Y here
      float originalFormatY = static_cast<float>(y);
      _outputContext.from_proxy_xy(originalFormatX, originalFormatY);

      int exrY = lineToLine(static_cast<int>(originalFormatY), header.displayWindow());
      bool decoded = exrYValid(exrY) ? decodeLine(*_part, lineBuffers[currentLine], box, reqChannels, outPlane)
                                     : false;

      if (!decoded)
        for (int x = box.x(); x < box.r(); x++)
          outPlane.setSampleCount(y, x, 0);
    }
  }
  catch ( Iex::BaseExc& e ) {
    _op->error( e.what() );
    return false;
  }

  mFnAssert(outPlane.isComplete());
  return true;
}

static DeepReader* build(DeepReaderOwner* iop, const std::string& fn)
{
  return new exrReaderDeep(iop, fn);
}

static DeepReaderFormat* buildFormat(DeepReaderOwner* op)
{
  return new exrDeepReaderFormat();
}

static const DeepReader::Description d("exr\0", build, buildFormat);

