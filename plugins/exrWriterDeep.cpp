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
#include <OpenEXR/ImfChannelList.h>

#include <OpenEXR/ImfHeader.h>
#include <OpenEXR/ImfMultiPartOutputFile.h>
#include <ImfDeepScanLineOutputFile.h>
#include <OpenEXR/ImfDeepScanLineOutputPart.h>
#include <OpenEXR/ImfDeepScanLineOutputPart.h>
#include <OpenEXR/ImfPartType.h>
#include <OpenEXR/ImfDeepFrameBuffer.h>

#include <OpenEXR/ImfChannelListAttribute.h>
#include <OpenEXR/ImfIntAttribute.h>
#include <OpenEXR/ImfStringAttribute.h>
#include <OpenEXR/ImfTimeCodeAttribute.h>
#include <OpenEXR/ImfStandardAttributes.h>
#include <OpenEXR/ImfMatrixAttribute.h>
#include <OpenEXR/ImfBoxAttribute.h>

#include <OpenEXR/ImfFramesPerSecond.h>
#include <OpenEXR/ImfMisc.h>
#include <OpenEXR/half.h>

#include <stdio.h>
#include <atomic>
#include <map>
#include <set>
#include <memory>
#include <algorithm>

#include "DDImage/DeepWriter.h"
#include "DDImage/DeepOp.h"
#include "DDImage/Executable.h"
#include "DDImage/Thread.h"
#include "DDImage/MetaData.h"

#include "exrGeneral.h"

//#define DEBUG_DEEP_EXR 1

using namespace DD::Image;


static Matrix4 getMatrix(const MetaData::Bundle& metadata,
                         const char* propname)
{
  MetaData::Bundle::PropertyPtr prop = metadata.getData(propname);
  if (!prop)
    return Matrix4::identity();

  return MetaData::getPropertyMatrix(prop);
}

static const int ctypesDeepLength = 3;
static const Imf::Compression ctypesDeep[ctypesDeepLength] = {
  Imf::NO_COMPRESSION,
  Imf::ZIPS_COMPRESSION,
  Imf::RLE_COMPRESSION
};
static const char* const cnamesDeep[ctypesDeepLength + 1] = {
  "none",
  "Zip (1 scanline)",
  "RLE",
  nullptr
};

/**
 * OpenEXR2 writer for deep data.
 *
 * Missing features, to be adapted from the existing EXR writer:
 *  stereo
 */
class exrWriterDeep : public DeepWriter
{
  typedef std::vector<std::vector<const float*> > SamplePtrs ;
  typedef std::vector<std::vector<std::vector <float> > > FloatSamples;
  typedef std::vector<std::vector<std::vector <half> > > HalfSamples ;
  typedef std::vector<unsigned> SampleCounts;

  int _datatype;
  int _compression;
  enum ExrMetaDataMode _metadataMode;
  bool _doNotWriteNukePrefix;
  
  bool writeLines(int y, int t, const Format*, const DD::Image::Box& box, const ChannelSet& channels, Imf::DeepScanLineOutputPart& part, int depth);
  bool fetchLine(int y, int yStart, const DD::Image::Box& box, const ChannelSet& channels, int floatdepth, SampleCounts& sampleCounts, SamplePtrs& samplePtrs, FloatSamples& samples, HalfSamples& halfSamples );

  class RangeLoader {

      exrWriterDeep* _op;
      std::atomic<std::int32_t> _nextY;
      int _t;
      const DD::Image::Box& _box;
      const ChannelSet& _channels;
      int _floatdepth;
      SampleCounts& _sampleCounts;
      SamplePtrs& _samplePtrs;
      FloatSamples& _samples;
      HalfSamples& _halfSamples;

      static void loadRangeThreadFunc(unsigned int threadNum, unsigned int, void * data);
      void loadRange();
  public:
      void run();
      void wait();
      RangeLoader( exrWriterDeep* op, int y, int t, const DD::Image::Box& box, const ChannelSet& channels, int floatdepth,
                     SampleCounts& sampleCounts, SamplePtrs& samplePtrs, FloatSamples& samples, HalfSamples& halfSamples):
        _op(op), _nextY(y), _t(t), _box(box), _channels(channels), _floatdepth(floatdepth),
        _sampleCounts(sampleCounts), _samplePtrs(samplePtrs), _samples(samples), _halfSamples(halfSamples) {}
  };

  bool isDeepChannel(Channel& z) { return z == Chan_DeepFront || z == Chan_DeepBack ; }

public:
  void knobs(Knob_Callback f) override;
  int knob_changed(Knob *k) override;
  exrWriterDeep(DeepWriterOwner* o);
  void execute() override;
 };


exrWriterDeep::exrWriterDeep(DeepWriterOwner* o) : DeepWriter(o), _datatype(0), _compression(1), _metadataMode(eDefaultMetaData), _doNotWriteNukePrefix(false)
{ 
}



void exrWriterDeep::knobs(Knob_Callback f)
{
  Enumeration_knob(f, &_datatype, dnames, "datatype");
  Enumeration_knob(f, &_compression, cnamesDeep, "compression");

  Enumeration_knob(f, (int*)&_metadataMode, metadata_modes, "metadata");
  Tooltip(f, "Which metadata to write out to the EXR file."
             "<p>'no metadata' means that no custom attributes will be created and only metadata that fills required header fields will be written.<p>'default metadata' means that the optional timecode, edgecode, frame rate and exposure header fields will also be filled using metadata values.");

  Bool_knob(f, &_doNotWriteNukePrefix, "noprefix", "do not attach prefix" );
  Tooltip(f, "By default unknown metadata keys have the prefix 'nuke' attached to them before writing them into the file.  Enable this option to write the metadata 'as is' without the nuke prefix.");
}

int exrWriterDeep::knob_changed(Knob *k)
{
  if ( k == &Knob::showPanel || k->is("metadata") ) {
    _owner->op()->knob( "noprefix")->enable( ExrMetaDataMode(_metadataMode) >=  eAllMetadataExceptInput );
    return 1;
  }
  return 0;
}


void exrWriterDeep::RangeLoader::loadRangeThreadFunc(unsigned int threadNum, unsigned int, void * data)
{
  exrWriterDeep::RangeLoader* rangeLoader = static_cast<exrWriterDeep::RangeLoader*> ( data );
  rangeLoader->loadRange();
}

void exrWriterDeep::RangeLoader::run()
{
  int n = Thread::numThreads - 1;
  int h = _nextY - _t;
  if ( h < n)
    n = h;

  // notice that one less parallel thread is launched, as the current thread
  // will also be running.
  if ( n > 0 )
    Thread::spawn(loadRangeThreadFunc, n, this);

  loadRange();
}

void exrWriterDeep::RangeLoader::wait()
{
  Thread::wait(this);
}

void exrWriterDeep::RangeLoader::loadRange()
{
  while (  _nextY >=  _t ) {
    // Atomic decrement and assign
    int thisLine =  _nextY--;
    // Have to retest the condition now in case someone took the line we tested
    // above. Bug 33927.
    if ( thisLine >= _t ) {
      if ( thisLine >=  _box.y() && thisLine <  _box.t()) {
        _op->fetchLine(thisLine,  _t, _box,  _channels,  _floatdepth,  _sampleCounts,  _samplePtrs,  _samples,  _halfSamples );
      }
    }
  }
}

bool exrWriterDeep::fetchLine(int y,
                              int t,
                              const DD::Image::Box& box,
                              const ChannelSet& channels,
                              int floatdepth,
                              SampleCounts& sampleCounts,
                              SamplePtrs& samplePtrs,
                              FloatSamples& samples,
                              HalfSamples& halfSamples )
{
  DD::Image::DeepPlane plane;

  if (!input()->deepEngine(y, box.x(), box.r(), channels, plane)) { // if false then it aborted
    return false;
  }

#ifdef DEBUG_DEEP_EXR
  const ChannelMap& channelMap = plane.channels();
  std::cout << "channel map size " << channelMap.size();
  foreach(z, channels) {
    std::cout << z << " maps to " << channelMap.chanNo(z) << std::endl;
  }

  std::cout << "actual channels " ;
  ChannelSet ac = channelMap;
  foreach(z, ac){
    std::cout << " " << z << std::endl;
  }
#endif

  int batchSize = sampleCounts.size() / box.w();
  int rowStart = ( batchSize - ((y - t)+1) );
  int rowOffset = (rowStart * box.w());

#ifdef DEBUG_DEEP_EXR
  std::cout << "Fetchline " << y << " rowStart " << rowStart << " lines = " << batchSize << std::endl;
#endif

  const size_t chanCount = plane.channels().size();
  // first we create a vector of sample counts, one per pixel
  for (int x = box.x(); x < box.r(); x++)
    sampleCounts[rowOffset + x - box.x()] = plane.getPixel(y, x).getSampleCount();

  // copy the data into the right structure
  int channelIndex = 0;
  if ( floatdepth == 32 ) {
    foreach(z, channels) {
      for (int x = box.x(); x < box.r(); x++) {
        int xOffset = rowOffset + x - box.x();
        int sampleCount = sampleCounts[xOffset];
        samples[channelIndex][xOffset].resize( sampleCount);
        float* writable = &samples[channelIndex][xOffset][0];
        const float* readable = &plane.getPixel(y, x).getUnorderedSample(0, z);

        for ( int s = 0; s < sampleCount; s++ ) {
          *writable++ = *readable;
          readable += chanCount;
        }
        samplePtrs[channelIndex][xOffset] = &samples[channelIndex][xOffset][0];
      }
      channelIndex++;
    }
  } else {
    // if 16 bit we need a copy in half-float format
    // where halfSamples[channel][x][pixel]
    foreach(z, channels) {
      for (int x = box.x(); x < box.r(); x++) {
        int xOffset = rowOffset + x - box.x();

        mFnAssert( channelIndex>=0 && (size_t)channelIndex < samples.size());
        mFnAssert( xOffset>=0 && (size_t)xOffset < samples[channelIndex].size());
        mFnAssert( channelIndex>=0 && (size_t)channelIndex < samplePtrs.size());
        mFnAssert( xOffset>=0 && (size_t)xOffset < samplePtrs[channelIndex].size());

        int sampleCount = sampleCounts[xOffset];
        if ( floatdepth == 32 || isDeepChannel(z) ) {
          if (sampleCount) {
            samples[channelIndex][xOffset].resize(sampleCount);
            float* writable = &samples[channelIndex][xOffset][0];
            const float* readable = &plane.getPixel(y, x).getUnorderedSample(0, z);
            for ( int s = 0; s < sampleCount; s++ ) {
              *writable++ = *readable;
              readable += chanCount;
            }
            samplePtrs[channelIndex][xOffset] = &samples[channelIndex][xOffset][0];
          }
          else {
            samplePtrs[channelIndex][xOffset] = nullptr;
          }
        } else {
          if (sampleCount) {
            halfSamples[channelIndex][xOffset].resize( sampleCount );
            half* writable = &halfSamples[channelIndex][xOffset][0];
            const float* readable = &plane.getPixel(y, x).getUnorderedSample(0, z);
            for ( int s = 0; s < sampleCount; s++ ) {
              *writable++ =  half(*readable);
              readable += chanCount;
            }
            samplePtrs[channelIndex][xOffset] = ( (float*)&halfSamples[channelIndex][xOffset][0] );
          }
          else {
            samplePtrs[channelIndex][xOffset] = nullptr;
          }
        }
      }
      channelIndex++;
    }
  }
  return true;
}

/**
 * Write out a particular line.
 *
 * Returns false if the input was aborted()
 */
bool exrWriterDeep::writeLines(int y, int t, const Format *format, const DD::Image::Box& box, const ChannelSet& channels, Imf::DeepScanLineOutputPart& part, int floatdepth)
{
  int numberOfRows = abs(t - y) + 1;
  assert( numberOfRows != 0 );

#ifdef DEBUG_DEEP_EXR
  std::cout << "Writing " << y << " to " << t << " lines " << numberOfRows << std::endl;
#endif

  SamplePtrs samplePtrs( channels.size() );
  FloatSamples samples( channels.size()  );
  HalfSamples halfSamples( channels.size() );

  SampleCounts sampleCounts( box.w() * numberOfRows );

  int channelIndex = 0;
  foreach(z, channels) {
    samplePtrs[channelIndex].resize( box.w() * numberOfRows );
    samples[channelIndex].resize( box.w() * numberOfRows );
    halfSamples[channelIndex].resize( box.w() * numberOfRows );
    channelIndex++;
  }


  RangeLoader loader (this, y, t, box, channels, floatdepth, sampleCounts, samplePtrs, samples, halfSamples );
  loader.run();
  loader.wait();

  Imf::DeepFrameBuffer frameBuffer;

  int startLine = format->t() - 1 - y;
  int yOffset = startLine * box.w();

  // create and insert the sample count slice
  frameBuffer.insertSampleCountSlice(Imf::Slice(Imf::UINT,
                                                        (char*)(&sampleCounts[0] - box.x()  -  yOffset ),
                                                        sizeof(unsigned int), // xstride
                                                        sizeof(unsigned int) * box.w() ));  // ystride

  channelIndex = 0;
  // create and insert the slices for the actual data
  foreach(z, channels) {
    if ( floatdepth == 32 || isDeepChannel(z) )
      frameBuffer.insert(getExrChannelName(z),
                         Imf::DeepSlice( Imf::FLOAT,
                                                 (char *)(&samplePtrs[channelIndex][0] - box.x() -  yOffset ),
                                                 sizeof(const float*), // xstride
                                                 sizeof(const float*) * box.w(), // ystride
                                                 Imf::pixelTypeSize(Imf::FLOAT) ) );  // samplestride
    else
      frameBuffer.insert(getExrChannelName(z),
                         Imf::DeepSlice( Imf::HALF,
                                                 (char *)(&samplePtrs[channelIndex][0] - box.x() - yOffset ) ,
                                                 sizeof(const half*), // xstride
                                                 sizeof(const half*) * box.w(), // ystride
                                                 Imf::pixelTypeSize(Imf::HALF) ) ); // samplestride

    channelIndex++;
  }

  part.setFrameBuffer(frameBuffer);
  part.writePixels(numberOfRows);

  return true;
}

void exrWriterDeep::execute()
{
  if ( ! input() ) {
    return;
  }

  // unfortunatly because of the way that deep writers work 'store' may not happen on the knobs before
  // execute so we grab the values from the knobs directly.

  _datatype = int(_owner->op()->knob("datatype")->get_value());
  _compression = int(_owner->op()->knob("compression")->get_value());
  _metadataMode = (enum ExrMetaDataMode)(int)_owner->op()->knob("metadata")->get_value();
  _doNotWriteNukePrefix = _owner->op()->knob("noprefix")->get_value() > 0.0;

  input()->validate(true);
  ChannelSet channels = input()->deepInfo().channels();
  const MetaData::Bundle& metadata = _owner->input()->op()->fetchMetaData(nullptr);

  int floatdepth = _datatype ? 32 : 16;

  Imf::Compression compression = ctypesDeep[this->_compression];

  channels &= ( _owner->channels() );
  if (!channels) {
    _owner->op()->critical("exrWriter: No channels selected (or available) for write\n");
    return;
  }

  channels += Mask_Deep; // must write deep channels
  channels += Mask_Alpha; // don't make sense without alpha

  std::vector<Imf::Header> headers(1); // when we do stereo this could be multi-part
  int viewCount = 1;
  
  for (int v = 0; v < viewCount ; v++) {

    // TODO stereo
    // DeepOp * op = dynamic_cast<DeepOp*>( _owner->op()->input(0,v) );
    DeepOp* op = input();
    assert( op );
    
    op->validate(true);
    const DeepInfo& di = op->deepInfo();
    op->deepRequest(di.box(), channels);
    
    DD::Image::Box box = di.box();

    const int formatLine = di.format()->t() - 1;
    
    Imath::Box2i dataBox(Imath::Vec2<int>(box.x(), formatLine - box.t() + 1 ),
                                 Imath::Vec2<int>(box.r() - 1, formatLine - box.y()));
    
    int partNumber = v;
    headers[partNumber].setType(Imf::DEEPSCANLINE);
    headers[partNumber].displayWindow().min.x = 0;
    headers[partNumber].displayWindow().min.y = 0;
    headers[partNumber].displayWindow().max.x = di.format()->width()-1;
    headers[partNumber].displayWindow().max.y = di.format()->height()-1;

    headers[partNumber].dataWindow().min.x = box.x();
    headers[partNumber].dataWindow().min.y = formatLine - box.t() + 1;
    headers[partNumber].dataWindow().max.x = box.r() - 1;
    headers[partNumber].dataWindow().max.y = formatLine - box.y();

    headers[partNumber].compression() = compression;

    const bool writeFullLayerNames = true;  // We always use full layer names
    metadataToExrHeader( (enum ExrMetaDataMode)_metadataMode, metadata, headers[partNumber], input()->op(), nullptr, _doNotWriteNukePrefix, writeFullLayerNames );

    if ( _metadataMode != eNoMetaData ) {
      // add some specific deep stuff
      Matrix4 Nl = getMatrix(metadata, DD::Image::MetaData::DTEX::DTEX_NL);
      Matrix4 NP = getMatrix(metadata, DD::Image::MetaData::DTEX::DTEX_NP);
      bool haveMetaDataMatrix = !Nl.isIdentity() || !NP.isIdentity();

      if ( ! haveMetaDataMatrix ) {
        Nl = getMatrix(metadata, DD::Image::MetaData::EXR::EXR_WORLD_TO_CAMERA );
        NP = getMatrix(metadata, DD::Image::MetaData::EXR::EXR_WORLD_TO_NDC);
      }

      haveMetaDataMatrix = !Nl.isIdentity() || !NP.isIdentity();

      if ( haveMetaDataMatrix ) {
        float nlVal[4][4];
        float npVal[4][4];
        for ( int row = 0; row < 4; row++ ) {
          for ( int column = 0; column < 4; column++ ) {
            nlVal[column][row] = Nl[column][row];
            npVal[column][row] = NP[column][row];
          }
        }

        headers[partNumber].insert( std::string( DD::Image::MetaData::EXR::EXR_WORLD_TO_CAMERA ).substr(4), Imf::M44fAttribute(Imath::M44f(nlVal)) );
        headers[partNumber].insert( std::string( DD::Image::MetaData::EXR::EXR_WORLD_TO_NDC ).substr(4), Imf::M44fAttribute(Imath::M44f(npVal)) );
      }
    }

    
    foreach(z, channels) {
#ifdef DEBUG_DEEP_EXR
      std::cout << " adding " << z << std::endl;
#endif
      headers[partNumber].channels().insert(getExrChannelName(z), Imf::Channel( floatdepth == 32 || isDeepChannel(z) ? Imf::FLOAT : Imf::HALF ));
    }
#ifdef DEBUG_DEEP_EXR
    std::cout << " done " << std::endl;
#endif
  }
  
  // now actually write out the data
  try { 
    // make file - use all available threads
    Imf::setGlobalThreadCount(Thread::numThreads);
    Imf::MultiPartOutputFile out(_owner->filename(), &headers[0], headers.size(), false, Thread::numThreads);
        
    for (int v = 0; v < viewCount ; v++) {
    
      // TODO stereo
      //DeepOp * op = dynamic_cast<DeepOp*>( _owner->op()->input(0,v) );
      DeepOp* op = input();
      assert( op );
      Imf::DeepScanLineOutputPart part(out, v);
      const DeepInfo& di = op->deepInfo();
      DD::Image::Box box = di.box();
      int batchSize = 64;

      for (int y = (box.t() - 1); y >= box.y(); y -= batchSize) {
#ifdef DEBUG_DEEP_EXR
        std::cout << "write line " << y << std::endl;
#endif
        int t = ( y + 1 ) - batchSize;
        if ( t < box.y() ) {
          t = box.y();
        }

        writeLines( y, t, di.format(), box, channels, part, floatdepth);

        _owner->op()->progressFraction(float(box.t()-y-box.y()) / (box.t()-box.y()));
        if ( _owner->op()->aborted() || _owner->op()->cancelled() ) {
          return;
        }
      }
    }
  }
  catch ( Iex::BaseExc& e ) {
#ifdef DEBUG_DEEP_EXR
    std::cout << e.what() << std::endl;
#endif
    _owner->op()->error( e.what() );
  }
}

static DeepWriter* build(DeepWriterOwner* iop)
{
  return new exrWriterDeep(iop);
}

static const DeepWriter::Description d("exr\0", build);

