// exrWriter.C
// Copyright (c) 2009 The Foundry Visionmongers Ltd.  All Rights Reserved.

/* Reads exr files using libexr.
   This is an example of a file reader that is not a subclass of
   FileWriter. Instead this uses the library's reader functions and
   a single lock so that multiple threads do not crash the library.

   04/14/03     Initial Release                Charles Henrich
   12/04/03     User selectable compression,   Charles Henrich
        float precision, and autocrop
   10/04    Defaulted autocrop to off    spitzak
   5/06        black-outside and reformatting    spitzak
 */
#include "DDImage/DDWindows.h"
#include "DDImage/Writer.h"
#include "DDImage/Row.h"
#include "DDImage/Knobs.h"
#include "DDImage/Tile.h"
#include "DDImage/DDString.h"
#include "DDImage/MetaData.h"
#include "DDImage/LUT.h"
#include "DDImage/NukePreferences.h"
#include "DDImage/Application.h"

#include <errno.h>
#include <stdio.h>

#include <OpenEXR/ImfPartType.h>
#include <OpenEXR/ImfMultiPartOutputFile.h>
#include <OpenEXR/ImfOutputPart.h>
#include <OpenEXR/ImfChannelList.h>
#include <OpenEXR/ImfArray.h>
#include <OpenEXR/ImfCompression.h>
#include <OpenEXR/ImfStringAttribute.h>
#include <OpenEXR/ImfIntAttribute.h>
#include <OpenEXR/ImfBoxAttribute.h>
#include <OpenEXR/ImfDoubleAttribute.h>
#include <OpenEXR/ImfStringVectorAttribute.h>
#include <OpenEXR/ImfTimeCodeAttribute.h>
#include <OpenEXR/ImfStandardAttributes.h>
#include <OpenEXR/ImfFramesPerSecond.h>
#include <OpenEXR/ImfMatrixAttribute.h>
#include <OpenEXR/ImfChannelListAttribute.h>
#include <OpenEXR/ImfChromaticities.h>
#include <OpenEXR/ImfChromaticitiesAttribute.h>
#include <OpenEXR/ImathBox.h>
#include <OpenEXR/half.h>
#include <OpenEXR/ImfMisc.h>

#include "exrGeneral.h"

#ifdef FN_OS_WINDOWS
  #include <process.h>
#else
#include <unistd.h>
#endif

// turn on debug output for exr writes
// #define _DEBUG_EXR_

using namespace DD::Image;

namespace {
    const char* const kFirstPartKnobName    = "first_part";
    const char* const kFirstPartKnobLabel   = "first part";
}
namespace Foundry
{
namespace Nuke
{

class exrWriter : public Writer
{
private:
  void autocrop_tile(Tile& img, ChannelMask channels, int* bx, int* by, int* br, int* bt);
  int datatype;
  int compression;
  float _dwCompressionLevel;
  bool autocrop;
  bool _acesFormat;
  bool writeHash;
  int _hero;
  int _leftView;
  int _rightView;
  int _metadataMode;
  bool _doNotWriteNukePrefix;
  bool _followStandard;
  int _multipartInterleaveMode;
  bool _truncateChannelNames;
  bool _writeFullLayerNames;
  DD::Image::Knob* _firstPartKnob;

  // data[views][channels][Box::h * Box::w]
  typedef std::vector<std::vector<std::vector<float>>> FloatSamples;
  // data[views][channels][Box::h * Box::w]
  typedef std::vector<std::vector<std::vector<half>>> HalfSamples;

  // return the name of the selected layer of the _firstPartKnob
  inline std::string getFirstPartMenuValue() const;

  // manage the state of the _firstPartKnobs state.
  void updateFirstPartMenuState();

  // If an environment variable is set
  // use the <filename.exr>.tmp format
  // otherwise default to a <hash>.tmp
  std::string getTempFileName();

  template<class T>
  void resizeBuffer(T& buf, uint32_t viewSize, uint32_t channelSize, uint32_t imageSize)
  {
    buf.resize(viewSize);
    for (uint32_t i = 0U; i < viewSize; ++i) {
      buf[i].resize(channelSize);
      for (uint32_t j = 0U; j < channelSize; ++j) {
        buf[i][j].resize(imageSize);
      }
    }
  }
public:

  // Multipart interleave modes
  enum MultipartInterleaveMode {
    eInterleave_Channels_Layers_Views,  // This makes a single part file for backwards compatibility
    eInterleave_Channels_Layers,        // This separates views for forwards compatibility with EXR 2.0
    eInterleave_Channels                // This separates views and layers for faster reading of individual layers
  };

  // Multpart mode labels
  static const char* const multipartModeLabels[];

  // String comparison for possibly null strings
  struct LessThanStr
  {
    bool operator()(const char* s1, const char* s2) const;
  };

  exrWriter(Write* iop);
  ~exrWriter() override;

  const Iop* firstInput(const std::set<int>& wantViews) const;

  void execute() override;
  void knobs(Knob_Callback f) override;
  int knob_changed(Knob *k) override;
  static const Writer::Description d;

  // Make it default to linear colorspace:
  LUT* defaultLUT() const override { return LUT::GetLut(LUT::FLOAT, this); }

  int split_input(int i) const override
  {
    // BW: It may be that executingViews is empty.  This is user controlled
    // using the "views" knob on the Write node.
    return int(executingViews().size() ? executingViews().size() : 1);
  }

  //! This writer is capable of writing out the overscan, so passthrough should not
  //! clip to the format.
  bool clipToFormat() const override
  {
    return false;
  }

  /**
   * return the view which we are expecting on input N
   */
  int view(const int inputIndex) const
  {
    // Assign inputs directly from the executingViews set
    std::set<int> views = executingViews();

    // Iterate through the views
    int currentInputIndex = 0;
    std::set<int>::const_iterator itView = views.begin();
    while (itView != views.end()) {
      // Return if we've reached the input index
      if (currentInputIndex == inputIndex) {
        return *itView;
      }
      // Move to the next input
      currentInputIndex++;
      itView++;
    }

    // BW: It may be that executingViews is empty.  This is user controlled
    // using the "views" knob on the Write node.  In other cases we do not
    // expect view() to be called for non-existent inputs.
    mFnAssertMsg(views.empty(), "exrWriter: View for input not found");
    return 0;
  }

  /**
   * return the input which we are expecting for view V
   */
  int inputIndex(int view) const
  {
    // Inputs are assigned directly from the executingViews set
    // ACES compliant EXR: For the stereoscopic images the ACES image container restricts
    // the set of image views that can appear in a file to the left view and right view only
    const int acesViews[] = {_leftView, _rightView};
    const std::set<int> acesViewsMap(acesViews, acesViews + 2);

    const bool isStereoscopic = (executingViews().size() > 1) && (_multipartInterleaveMode == eInterleave_Channels_Layers_Views);
    const std::set<int> views = _acesFormat && isStereoscopic ? acesViewsMap : executingViews();

    // Iterate through the views
    int currentInputIndex = 0;
    std::set<int>::const_iterator itView = views.begin();
    while (itView != views.end()) {
      // Return if we've found the view
      if (*itView == view) {
        return currentInputIndex;
      }
      // Move to the next input
      currentInputIndex++;
      itView++;
    }

    // BW: It may be that executingViews is empty.  This is user controlled
    // using the "views" knob on the Write node.  In other cases we do not
    // expect inputIndex() to be called for non-existent inputs.
    mFnAssertMsg(views.empty(), "exrWriter: Input for view not found");
    return 0;
  }

  bool compressionHasLevel() const
  {
    const Imf::Compression compressionVal = ctypes[this->compression];
    const bool hasLevel = ( compressionVal == Imf::DWAA_COMPRESSION ||
                            compressionVal == Imf::DWAB_COMPRESSION );
    return hasLevel;
  }

  const OutputContext& inputContext(int i, OutputContext& o) const override
  {
    o = iop->outputContext();
    o.view(view(i));
    return o;
  }

  const char* help() override
  {
    return "<b>OpenEXR</b> is a high dynamic-range (HDR) image file format originally developed by Industrial Light & Magic for use in computer imaging applications.\n"
           "Current version - v2.3.0";
  }

};

const char* const exrWriter::multipartModeLabels[] = {
  "channels, layers and views",
  "channels and layers",
  "channels",
  nullptr
};

/*
* purpose : Is s2 less than s1
*   Returns true if s1 appears before s2 in alphanumeric order.
*   Returns false if s2 appears before s1 in alphanumeric order.
*   Returns false if s1 and s2 are equal
*
* Edge cases:
*   Returns false if both strings are NULL.
*   Returns true if s1 is NULL
*   Retruns false if s2 is NULL.
*/
bool exrWriter::LessThanStr::operator()(const char* s1, const char* s2) const
{
  // Collate null pointers
  if (s1 == nullptr && s2 == nullptr)
    return false;
  if (s1 == nullptr)
    return true;
  if (s2 == nullptr)
    return false;
  // does s1 appear before s2
  return strcmp(s1, s2) < 0;
}

static Writer* build(Write* iop)
{
  return new exrWriter(iop);
}

const Writer::Description exrWriter::d("exr\0sxr\0", build);

exrWriter::exrWriter(Write* iop)
  : Writer(iop)
  , datatype(0)
  , compression(1)
  , _dwCompressionLevel(45.0f)
  , autocrop(false)
  , _acesFormat(false)
  , writeHash(true)
  , _hero(1)
  , _leftView(1)
  , _rightView(2)
  , _metadataMode(eDefaultMetaData)
  , _doNotWriteNukePrefix(false)
  , _followStandard(0)
  // Default to backwards compatibility
  , _multipartInterleaveMode(eInterleave_Channels_Layers_Views)
  , _truncateChannelNames(false)
  , _writeFullLayerNames(false)
  , _firstPartKnob(nullptr)
{
  setFlags(DONT_CHECK_INPUT0_CHANNELS);
  //RP:defaulting compression level to the same as in in OpenEXR
  //FROM:ImfDwaCompressor.cpp
  // Compression level is controlled by setting an int/float/double attribute
  // on the header named "dwaCompressionLevel". This is a thinly veiled name for
  // the "base-error" value mentioned above. The "base-error" is just
  // dwaCompressionLevel / 100000. The default value of 45.0 is generally
  // pretty good at generating "visually lossless" values at reasonable
  // data rates. Setting dwaCompressionLevel to 0 should result in no additional
  // quantization at the quantization stage (though there may be
  // quantization in practice at the CSC/DCT steps). But if you really
  // want lossless compression, there are plenty of other choices
  // of compressors ;)
}

exrWriter::~exrWriter()
{
}

const Iop* exrWriter::firstInput(const std::set<int>& wantViews) const
{
  for (int i = 0; i < iop->inputs(); ++i) {
    if (wantViews.find(view(i)) == wantViews.end())
      continue;

    return iop->input(i);
  }
  return &input0();
}

void exrWriter::execute()
{
  // get all the views
  std::set<int> execViews = executingViews();
  // get the write nodes views to execute
  std::set<int> wantViews = iop->executable()->viewsToExecute();

  if (_acesFormat) {
    // ACES compliant EXR: For the stereoscopic images the ACES image container restricts
    // the set of image views that can appear in a file to the left view and right view only
    const int acesViews[] = { _leftView, _rightView };
    const std::set<int> acesViewsMap(acesViews, acesViews + 2);

    const bool isStereoscopic = (_multipartInterleaveMode == eInterleave_Channels_Layers_Views && execViews.size() > 1);
    if (isStereoscopic) {
      execViews = acesViewsMap;
      mFnAssertMsg(execViews.size() == 2, "exrWriter: ACES compliant EXR files are restricted to two views only");
    }
  }

  if (_acesFormat || wantViews.size() == 0) {
    wantViews = execViews;
  }

  int floatdepth = datatype ? 32 : 16;

  Imf::Compression compression = ctypes[this->compression];

  ChannelSet channels(firstInput(wantViews)->channels());
  channels &= (iop->channels());

  // This applays only for ACES compliant EXR files;
  //
  // When exporting less than three channels from the RGB set then the
  // remaining channels are exported as black as we always need to
  // export at least the RGB channels in order to write out an
  // ACES compliant EXR file.
  //
  // For non ACES files there is no restriction in terms of the number
  // of channels to be written into the exported file;
  ChannelSet blackChannels = Mask_None;

  if (_acesFormat) {
    // ACES compliant EXR: The ACES image container restricts the set of image
    // channels that can appear in a file to RGB and RGBA
    channels &= Mask_RGBA;
    if (channels.size() < 3) {
      blackChannels = Mask_RGB;
      blackChannels -= channels;

      channels = Mask_RGB;
    }
    mFnAssertMsg(channels & Mask_RGBA || channels & Mask_RGB, "exrWriter: ACES compliant EXR files are restricted to RGB or RGBA channels");

    // ACES compliant EXR: The compression attribute shall always contain the value 0
    // indicating no compression, in an ACES compliant EXR file
    compression = Imf::NO_COMPRESSION;

    // ACES compliant EXR: The red, green, blue, and alpha values
    // are of type half (16-bit floating-point)
    floatdepth = 16;
  }

  if (!channels) {
    iop->critical("exrWriter: No channels selected (or available) for write\n");
    return;
  }

  if (premult() && !lut()->linear() &&
      (channels & Mask_RGB) && (input0().channels() & Mask_Alpha))
    channels += (Mask_Alpha);

  // TO DO:
  // these vectors are the ID and name of the views we will
  // write out. it would be nice to have a unified single
  // struct to encapsulate this data.
  std::vector<int> viewIDs;
  std::vector<std::string> viewNames;

  if (wantViews.size() == 1) {
    _hero = *wantViews.begin();
  }

  // now since we want to write out the Hero View first if it has been requested
  // select that from the view map, and add it to our vectors first.
  if (execViews.find(_hero) != execViews.end()) {
    viewIDs.push_back(_hero);
    viewNames.push_back(OutputContext::viewName(_hero, iop));
  }

  // get the rest of the views.
  for (std::set<int>::const_iterator i = execViews.begin(); i != execViews.end(); ++i) {
    if (*i != _hero) {
      viewIDs.push_back(*i);
      viewNames.push_back(OutputContext::viewName(*i, iop));
    }
  }

  DD::Image::Box bound;

  bool sizewarn = false;

  bool firstInputBbox = true;

  for (int i = 0; i < iop->inputs(); ++i) {

    if (wantViews.find(view(i)) == wantViews.end())
      continue;

    Iop* input = iop->input(i);
    int bx = input->x();
    int by = input->y();
    int br = input->r();
    int bt = input->t();
    if (input->black_outside()) {
      if (bx + 2 < br) {
        bx++;
        br--;
      }
      if (by + 2 < bt) {
        by++;
        bt--;
      }
    }

    input->request(bx, by, br, bt, channels, 1);

    if (br - bx > input0().format().width() * 1.5 ||
        bt - by > input0().format().height() * 1.5) {
      // print this warning before it possibly crashed due to requesting a
      // huge buffer!
      if (sizewarn) {
        fprintf(stderr, "!WARNING! Bounding Box Area is > 1.5 times larger "
                        "than format. You may want crop your image before writing it.\n");
        sizewarn = true;
      }
    }

    if (autocrop) {

      Tile img(*input, input->x(), input->y(),
               input->r(), input->t(), channels, true);

      if (iop->aborted()) {
        //iop->critical("exrWriter: Write failed [Unable to get input tile]\n");
        return;
      }

      autocrop_tile(img, channels, &bx, &by, &br, &bt);
      bt++; /* We (aka nuke) want r & t to be beyond the last pixel */
      br++;
    }

    if (firstInputBbox) {
      bound.y(by);
      bound.x(bx);
      bound.r(br);
      bound.t(bt);
    }
    else {
      bound.y(std::min(bound.y(), by));
      bound.x(std::min(bound.x(), bx));
      bound.r(std::max(bound.r(), br));
      bound.t(std::max(bound.t(), bt));
    }

    firstInputBbox = false;
  }

  const Format& inputFormat = firstInput(wantViews)->format();

  Imath::Box2i C_datawin;
  C_datawin.min.x = bound.x();
  C_datawin.min.y = inputFormat.height() - bound.t();
  C_datawin.max.x = bound.r() - 1;
  C_datawin.max.y = inputFormat.height() - bound.y() - 1;

  Imath::Box2i C_dispwin;
  C_dispwin.min.x = 0;
  C_dispwin.min.y = 0;
  C_dispwin.max.x = inputFormat.width() - 1;
  C_dispwin.max.y = inputFormat.height() - 1;

  // Bug 33310 - nuke.cancel() in write node fails to cancel
  if (iop->aborted())
  { // abort before writing anything so that we don't end up with partial files written
    return;
  }

  try {
    // The number of distinct channels including disparity channels
    // Note: Disparity channels do not belong to any view
    int numchannels = channels.size();

    // Determine the number of parts to write

    // Resolve the layers

    // Note that not all channels belong to a layer.
    // We need to find all of the layers (including NULL for channels outside a layer)
    typedef std::set<const char*, LessThanStr> StringSet;
    StringSet layerSet;
    // We need to map all of the channels (to avoid losing channels outside a layer)
    typedef std::multimap<const char*, Channel, LessThanStr> StringChannelMap;
    StringChannelMap layerChannelMap;
    foreach(z, channels) {
      // Note that the layer name may be null
      const char* layerName = getLayerName(z);

      // Fix up rgb to rgba (regardless of presence of alpha)
      if (layerName && !strcmp(layerName, "rgb"))
        layerName = "rgba";

      layerSet.insert(layerName);
      layerChannelMap.insert( std::pair<const char* const, Channel>(layerName, z) );
    }
    const size_t numLayers = layerSet.size();

    // The number of views
    size_t numViews  = wantViews.size();

    // The number of parts
    size_t numParts  = 0;
    switch (_multipartInterleaveMode) {
    case eInterleave_Channels_Layers_Views:
      // All in one
      numParts = 1;
      break;
    case eInterleave_Channels_Layers:
      // Part per view
      numParts = numViews;
      break;
    case eInterleave_Channels:
      // Part per view per layer
      numParts = numViews * numLayers;
      break;
    };
    mFnAssert(numParts);

    // Create an array of headers (one for each part)
    Imf::Header exrHeaderTemplate(C_dispwin, C_datawin, static_cast<float>(iop->format().pixel_aspect()),
      Imath::V2f(0, 0), 1, Imf::INCREASING_Y, compression);

    exrHeaderTemplate.setType(Imf::SCANLINEIMAGE);
    exrHeaderTemplate.setVersion(1);

    //If the compression method is either DWAA or DWAB set the value, it defaults to 45
    if ( compressionHasLevel() ) {
      Imf::addDwaCompressionLevel( exrHeaderTemplate, _dwCompressionLevel );
    }

    FloatSamples floatSamples;
    HalfSamples halfSamples;

    if (floatdepth == 32) {
      resizeBuffer(floatSamples, viewIDs.size(),
                   channels.size(), bound.area());
    }
    else {
      resizeBuffer(halfSamples, viewIDs.size(),
                   channels.size(), bound.area());
    }

    const int scanlineWidth = bound.w();

    // Create an array of frame buffers (to match)
    std::vector<Imf::FrameBuffer> fbufs(numParts);

    std::vector<Imf::Header> exrheaders(numParts, exrHeaderTemplate);

    // Set the multiview attribute if necessary
    const bool isStereoscopic = (numParts == 1 && wantViews.size() > 1);
    if (isStereoscopic) {
      // only write multi view string if a stereo file
      Imf::StringVectorAttribute multiViewAttr;

      multiViewAttr.value() = viewNames;
      exrheaders[0].insert("multiView", multiViewAttr);
    }

    if (_acesFormat) {
      // This attribute and value shall indicate that the file and
      // all attribute values are compliant with ACES specification.
      Imf::IntAttribute acesImageContainerFlagAttr = Imf::IntAttribute(1);
      exrheaders[0].insert("acesImageContainerFlag", acesImageContainerFlagAttr);

      // Add the chromaticities attribute
      Imf::ChromaticitiesAttribute chromaAttr;
      chromaAttr.value() = acesDefaultChromaticites;
      exrheaders[0].insert("chromaticities", chromaAttr);
    }

    Iop* metaInput = nullptr;
    for (size_t viewIdx = 0; viewIdx < viewIDs.size(); ++viewIdx) {
      if (wantViews.find(viewIDs[viewIdx]) == wantViews.end()) {
        continue;
      }
      if (metaInput == nullptr || viewIDs[viewIdx] == _hero) {
        const int inputIdx = inputIndex( viewIDs[viewIdx] );
        metaInput = iop->input(inputIdx);
      }
    }
    if (metaInput == nullptr) {
      metaInput = iop->input(0);
    }

    const MetaData::Bundle& metadata = metaInput->fetchMetaData(nullptr);
    Hash nodeHash = iop->getHashOfInputs();
    metadataToExrHeader( (enum ExrMetaDataMode)  _metadataMode, metadata, exrheaders[0], iop, writeHash ? &nodeHash : nullptr, _doNotWriteNukePrefix, _writeFullLayerNames );

    std::map<int, ChannelSet> channelsperview;
    // Index the channels per view to allow for missing disparity channels
    std::vector< std::map<Channel, int> > rowChannelIndices(viewIDs.size());

    int currentPart = 0;
    const int pixelbase = C_datawin.min.y * scanlineWidth + C_datawin.min.x;
    // Loop through each view
    for (int v = 0; v < int(viewIDs.size()); v++) {
      mFnAssert(static_cast<size_t>(currentPart) < numParts);

      if (wantViews.find(viewIDs[v]) == wantViews.end()) {
        continue;
      }

      // ideally to write the orders in the correct layer
      // we could encapsulate as much of this in a function as possible
      // then write out our selected layer  to the first
      // header
      int currentChannel = 0;
      // Loop through layers
      StringSet::const_iterator itLayerSet = layerSet.begin();
      StringSet::const_iterator endLayerSet = layerSet.end();
      for ( ; itLayerSet != endLayerSet; ++itLayerSet) {

        mFnAssert(static_cast<size_t>(currentPart) < numParts);

        // Get the layer name
        const char* layerName = *itLayerSet;

        // For multipart files
        if (numParts > 1) {
          // Set the view attribute
          exrheaders[currentPart].setView(viewNames[v]);

          // Set the name attribute
          std::string name;
          if (layerName && _multipartInterleaveMode == eInterleave_Channels) {
            name += layerName;
            name += '.';
          }
          name += viewNames[v];

          exrheaders[currentPart].setName(name);
        }

        // Find the channels in this layer
        // iterator to beginning of all 'layername' : channel elements
        StringChannelMap::const_iterator itLayerChannelMap =
          layerChannelMap.lower_bound(layerName);
        // iterator to next element after  of all 'layername' : channel elements
        StringChannelMap::const_iterator upperLayerChannelMap =
          layerChannelMap.upper_bound(layerName);
        ChannelSet layerChannels;
        for( ; itLayerChannelMap != upperLayerChannelMap; ++itLayerChannelMap) {
          layerChannels += itLayerChannelMap->second;
        }

        // Loop through channels
        foreach(z, layerChannels) {
          std::string channame;

          switch (z) {
            case Chan_Red: channame = "R";
              break;
            case Chan_Green: channame = "G";
              break;
            case Chan_Blue: channame = "B";
              break;
            case Chan_Alpha: channame = "A";
              break;
            default: channame = iop->channel_name(z);
              break;
          }

          // Add the view name if necessary
          if (_acesFormat && isStereoscopic) {
            if (viewIDs.size() > 1 && viewIDs[v] != _rightView) {
              channame = "left." + channame;
            }
          }
          else {
            if (executingViews().size() > 1 && viewIDs[v] != _hero && _multipartInterleaveMode == eInterleave_Channels_Layers_Views) {
              if (_followStandard) {
                size_t i = channame.find('.');
                if (i != channame.npos){
                  std::string layerName = channame.substr(0, i);
                  std::string channelName = channame.substr(i + 1);
                  channame = layerName + "." + OutputContext::viewName(viewIDs[v], iop) + "." + channelName;
                }
                else {
                  channame = OutputContext::viewName(viewIDs[v], iop) + "." + channame;
                }
              }
              else {
                channame = OutputContext::viewName(viewIDs[v], iop) + "." + channame;
              }

              // Skip disparity channels for all but the default (hero) view
              if (z == Chan_Stereo_Disp_Left_X ||
                  z == Chan_Stereo_Disp_Left_Y ||
                  z == Chan_Stereo_Disp_Right_X ||
                  z == Chan_Stereo_Disp_Right_Y) {
                continue;
              }
            }

            // Remove the layer name if necessary
            // Note that the view will not be part of the Nuke channel name.
            if (!_writeFullLayerNames && _multipartInterleaveMode == eInterleave_Channels) {
              size_t i = channame.find('.');
              if (i != channame.npos){
                 //   changing chan name from Layer.Channel to Channel
                channame = channame.substr(i + 1);
              }
            }
          }

          // truncate channel name to a maximum of 31 chars
          static const size_t MAX_CHANNEL_SIZE = 31;
          if (_truncateChannelNames && (channame.size() > MAX_CHANNEL_SIZE) ) {
              channame = channame.substr( 0, MAX_CHANNEL_SIZE );
          }


          channelsperview[v].insert(z);

          if (floatdepth == 32) {
            exrheaders[currentPart].channels().insert(channame.c_str(), Imf::Channel(Imf::FLOAT));
          }
          else {
            exrheaders[currentPart].channels().insert(channame.c_str(), Imf::Channel(Imf::HALF));
          }

          if (floatdepth == 32) {
            // pixelbase is offset in pixels between base of array and 0,0
            fbufs[currentPart].insert(channame.c_str(),
                        Imf::Slice(Imf::FLOAT,
                                   (char*)(&floatSamples[currentPart][currentChannel][0] - (pixelbase)),
                                   sizeof(float), sizeof(float) * scanlineWidth));
            rowChannelIndices[v][z] = currentChannel;
            currentChannel++;
          }
          else {
            // pixelbase is offset in pixels between base of array and 0,0
            fbufs[currentPart].insert(channame.c_str(),
                        Imf::Slice(Imf::HALF,
                                   (char*)(&halfSamples[currentPart][currentChannel][0] - (pixelbase)),
                                   sizeof(half), sizeof(half) * scanlineWidth));
            rowChannelIndices[v][z] = currentChannel;
            currentChannel++;
          }
        } // next channel

        // Move to the next layer // a new part for every layer.
        if(_multipartInterleaveMode == eInterleave_Channels) {
          currentPart++;
        }
      } // next layer

      // Move to the next view
      if(_multipartInterleaveMode == eInterleave_Channels_Layers){
        currentPart++;
      }
    } // next view

    std::string temp_name = getTempFileName();

    // Scope use of the output file
    {

      // 354277 Multi-part EXR: Add the ability to specify which channel should be written as main
      // this section of the code was a quick way to modify the existing code path, without having
      // to radically alter the code or do a first-layer-header-pass then a rest-pass over
      // the exrHeaders which may have resulted in lots of code duplication or atleast
      // refactoring.

      if(_multipartInterleaveMode == eInterleave_Channels &&  _firstPartKnob && _firstPartKnob->isEnabled() && exrheaders.size() > 1) {

        // the first part will be view selected by the heroView, and the layer selected by firstPart.
        // selected layer name
        std::string selectedLayerName = getFirstPartMenuValue();

        // because we had to "Fix up rgb to rgba (regardless of presence of alpha)"
        if(selectedLayerName == "rgb") {
          selectedLayerName = "rgba";
        }


        // we want to make sure that all the layer.view parts for the specified layer are written out
        // before the other layers.
        size_t partPos = 0;
        for(auto viewName : viewNames) {
          // exr headers store the layer in the name part of exrheaders regardless of _writeFullLayerNames
          std::string layerDotViewName = selectedLayerName;
          layerDotViewName.append(".");
          layerDotViewName.append(viewName);

          // make a list of the the parts in the desired order
          auto it = std::find_if(exrheaders.begin(), exrheaders.end(), [&](const Imf::Header& in)
          {
            return in.name() == layerDotViewName;
          });


          if(it != exrheaders.end()) {
            // reorder frame buffer vector first before 'it' becomes invalidated.
            size_t indx = static_cast<size_t>(std::distance(exrheaders.begin(), it));
            mFnAssert(fbufs.size() > indx); // there arent enough frame buffers for one per part.

            // swap header, exrheader[0] still has all the important information,
            // and should still be the first element when calling Imf::MultiPartOutputFile
            // otherwise Imf::MultiPartOutputFile will thow an exception if
            // a openEXR header shared attribute mismatch occurs
            if(partPos == 0) {
              std::swap(exrheaders[0].name(), it->name());
              std::swap(exrheaders[0].channels(), it->channels());
              std::swap(exrheaders[0].view(), it->view());
              std::swap(exrheaders[0], *it);
            }

            std::rotate(exrheaders.begin() + partPos, exrheaders.begin() + indx, exrheaders.begin() + indx + 1);
            std::rotate(fbufs.begin() + partPos, fbufs.begin() + indx, fbufs.begin() + indx + 1);
            ++partPos;
          }
        }

      }

#ifdef _DEBUG_EXR_
      std::cout << "-------------- writing out exr data --------------" << std::endl;
      for(size_t i = 0; i < numParts; ++i) {
        if(exrheaders[i].hasName()){
          std::cout << "part "<< i << " name : " <<  exrheaders[i].name() << std::endl;
        }
        else {
          std::cout << "part name : None"  << std::endl;
        }
        for(auto slice_it = fbufs[i].begin(); slice_it != fbufs[i].end(); ++slice_it) {
          std::cout << "\tchannel : "  << slice_it.name()  << std::endl;
        }
      }
#endif // _DEBUG_EX

      // When doing a terminal render use the DD::Image::Thread::numThreads
      // as the globalThreadCount gor openEXR write,
      // When in GUI mode and the global thread count is 0,
      // set the openEXR thread count to DD::Image::Thread::numThreads
      // else use the value set by UI
      if (!Application::IsGUIActive() ||
          Imf::globalThreadCount() == 0 ) {
          Imf::setGlobalThreadCount(Thread::numThreads);
      }
      // Create an output file
      Imf::MultiPartOutputFile outfile(temp_name.c_str(),
                                       &exrheaders[0], static_cast<int>(numParts));

      for (size_t i = 0; i < numParts; ++i) {
        Imf::OutputPart outpart(outfile, static_cast<int>(i));
        // set the frame buffer as the output
        outpart.setFrameBuffer(fbufs[i]);
        Row renderrow(bound.x(), bound.r());
        Row writerow(bound.x(), bound.r());

        const int yOffset = -bound.y();
        const int scanlineWidth = bound.w();
        for (int scanline = bound.t() - 1; scanline >= bound.y(); scanline--) {
          const int adjustedScanline = bound.t() - 1 - scanline + yOffset;
          for (int v = 0; v < int(viewIDs.size()); v++) {

            channels = channelsperview[v];
            if (wantViews.find(viewIDs[v]) == wantViews.end()) {
              continue;
            }

            const int inputIdx = inputIndex( viewIDs[v] );
            writerow.pre_copy(renderrow, channels);
            {
              Row rw(bound.x(), bound.r());
              iop->inputnget(inputIdx, scanline, bound.x(), bound.r(), channels,
                             rw, 1.0f/static_cast<float>(wantViews.size()));
              if (iop->aborted()) {
                break;
              }
              renderrow.copy(rw, channels, bound.x(), bound.r());
            }
            const int inputR = iop->input(inputIdx)->r();
            const int inputX = iop->input(inputIdx)->x();

            if (bound.is_constant()) {
              foreach(z, channels) {
               renderrow.erase(z);
              }
              continue;
            }

            foreach(z, channels) {
              int32_t offset = (adjustedScanline - yOffset) * scanlineWidth;
              if (_acesFormat) {
                // This applays only for ACES compliant EXR files;
                //
                // If the EXR file is ACES ('write out an ACES compliant EXR file' knob is checked by the customer)
                // in that case we've extended the channels to Mask_RGB at the top of this method, only if the
                // original requested channels are a subset of the RGB set;
                //
                // Remove the channels that haven't been requested by the customer, and
                // let them be written as black into the exported file;
                //
                // blackChannel will be Mask_None if the file is not Aces
                //
                if (blackChannels.contains(z)) {
                  renderrow.erase(z);
                }
              }
              else {
                mFnAssertMsg(blackChannels == Mask_None, "exrWriter: blackChannels must be Mask_None for none-aces files");
              }
              const float* from = renderrow[z];
              const float* alpha = renderrow[Chan_Alpha];
              float* to = writerow.writable(z);

              if (!lut()->linear() && z <= Chan_Blue) {
                to_float(z - 1, to + C_datawin.min.x,
                         from + C_datawin.min.x,
                         alpha + C_datawin.min.x,
                         C_datawin.max.x - C_datawin.min.x + 1);
                from = to;
              }

              if (bound.r() > inputR) {
                float* end = renderrow.writable(z)   + bound.r();
                float* start = renderrow.writable(z) + inputR;
                while (start < end) {
                  *start = 0;
                  start++;
                }
              }
              if (bound.x() < inputX) {
                float* end = renderrow.writable(z)   + bound.x();
                float* start = renderrow.writable(z) + inputX;
                while (start > end) {
                  *start = 0;
                  start--;
                }
              }

              // Get the row channel index for this view and channel
              const int currentChannel = rowChannelIndices[v][z];
              if (floatdepth == 32) {
                std::copy(&from[C_datawin.min.x], &from[C_datawin.max.x] + 1,
                          &floatSamples[v][currentChannel][offset]);
              }
              else {
                std::transform(from + C_datawin.min.x,
                               from + C_datawin.max.x + 1,
                               &halfSamples[v][currentChannel][offset],
                               [](float v) { return half(v); });
              }
            }
          }
          progressFraction( (double(bound.t() - scanline) / (bound.t() - bound.y()) + i) / numParts);
        }

        outpart.writePixels(bound.h());
      }
    } // Scope use of the output file

    if (!FileIop::renameFile(temp_name.c_str(), filename()))
      iop->critical("Can't rename .tmp to final, %s", strerror(errno));
  }
  catch (const std::exception& exc) {
    iop->critical("EXR: Write failed [%s]\n", exc.what());
    return;
  }
}

void exrWriter::knobs(Knob_Callback f)
{
  Bool_knob(f, &_acesFormat, "write_ACES_compliant_EXR", "write ACES compliant EXR");
  Tooltip(f, "Write out an ACES compliant EXR file");

  Bool_knob(f, &autocrop, "autocrop");
  Tooltip(f, "Reduce the bounding box to the non-zero area. This is normally "
             "not needed as the zeros will compress very small, and it is slow "
             "as the whole image must be calculated before any can be written. "
             "However this may speed up some programs reading the files.");

  Bool_knob(f, &writeHash, "write_hash", "write hash");
  SetFlags(f, Knob::INVISIBLE);
  Tooltip(f, "Write the hash of the node graph into the exr file.  Useful to see if your image is up to date when doing a precomp.");

  Knob*const dataTypeKnob = Enumeration_knob(f, &datatype, dnames, "datatype");
  if (dataTypeKnob) {
    dataTypeKnob->enable(!_acesFormat);
  }

  Knob*const compressionKnob = Enumeration_knob(f, &compression, cnames, "compression");
  if (compressionKnob) {
    compressionKnob->enable(!_acesFormat);
  }

  const bool isStereoscopic = (executingViews().size() > 1) && (_multipartInterleaveMode == eInterleave_Channels_Layers_Views);
  const bool isAcesStereo = isStereoscopic && _acesFormat;

  Knob*const multiViewKnob = iop->knob("views");
  if (multiViewKnob) {
    multiViewKnob->enable(!isAcesStereo);
    multiViewKnob->visible(!isAcesStereo);
  }

  Knob* compressionLevel = Float_knob(f, &_dwCompressionLevel, IRange(0.0f, 500.0f),"dw_compression_level","compression level");
  if (compressionLevel) {
    const bool doesCompressionHaveLevel = compressionHasLevel();
    compressionLevel->enable(doesCompressionHaveLevel);
    compressionLevel->visible(doesCompressionHaveLevel);
  }

  Obsolete_knob(f, "stereo", nullptr);

  Knob*const heroViewKnob = OneView_knob(f, &_hero, "heroview");
  Tooltip(f, "If stereo is on, this is the view that is written as the \"main\" image");
  if (heroViewKnob) {
    heroViewKnob->enable(!isAcesStereo);
    heroViewKnob->visible(!isAcesStereo);
  }

  Knob*const leftViewKnob = OneView_knob(f, &_leftView, "left_view", "Left view");
  Tooltip(f, "If stereo is on, this is the view that is written as the \"left\" image");

  if (leftViewKnob) {
    leftViewKnob->enable(isAcesStereo);
    leftViewKnob->visible(isAcesStereo);
  }

  Knob*const rightViewKnob = OneView_knob(f, &_rightView, "right_view", "Right view");
  Tooltip(f, "If stereo is on, this is the view that is written as the \"right\" image");

  if (rightViewKnob) {
    rightViewKnob->enable(isAcesStereo);
    rightViewKnob->visible(isAcesStereo);
  }

  Enumeration_knob(f, &_metadataMode, metadata_modes, "metadata");
  Tooltip(f, "Which metadata to write out to the EXR file."
             "<p>'no metadata' means that no custom attributes will be created and only metadata that fills required header fields will be written.<p>'default metadata' means that the optional timecode, edgecode, frame rate and exposure header fields will also be filled using metadata values.");
  Bool_knob(f, &_doNotWriteNukePrefix, "noprefix", "do not attach prefix" );
  Tooltip(f, "By default unknown metadata keys have the prefix 'nuke' attached to them before writing them into the file.  Enable this option to write the metadata 'as is' without the nuke prefix.");

  Newline(f);
  Enumeration_knob(f, &_multipartInterleaveMode, multipartModeLabels, "interleave");
   Tooltip(f,"Interleave strategy of channels, layers and views within the rendered .exr. A single or multi-part exr will be created as per the options below, with layers and parts sorted alphanumerically.<br><br>"
          "<u>channels, layers and views</u><br>"
          "Creates a single-part .exr and ensures backwards compatibility with applications using OpenEXR 1.x.<br><br>"
          "<u>channels and layers</u><br>"
          "Creates a multi-part .exr with one part per view. This can speed up Read performance as Nuke will only read the part pertaining to the specified view.<br><br>"
          "<u>channels</u><br>"
          "Creates a multi-part exr with one part per layer.");

  _firstPartKnob = InputOnly_Channel_knob(f, nullptr, 4, 0, kFirstPartKnobName, kFirstPartKnobLabel);
  Tooltip(f,"Enabled when the 'channels' interleave strategy is selected and the channels knob is set to 'all' <br>"
            "i.e. the output is a multi-part exr with one part per layer.<br><br>"
            "Specifies the layer that will be assigned to the first part of the multi-part .exr. All remaining parts will be stored in alphanumeric order.<br>"
            "In a multi-view setup, the layer for each view will be assigned to the topmost parts<br>"
            "i.e. part0: rgba.left, part1: rgba.right<br><br>"
            "The 'none' acts as the default behaviour where all parts will be stored in alphanumeric order.");
  SetFlags(f, Knob::NO_CHECKMARKS | Knob::NO_ALPHA_PULLDOWN | Knob::NO_ANIMATION | Knob::ALWAYS_SAVE);
  // update our First Part menu, if we are responding to changes from the write node
  updateFirstPartMenuState();

  Newline(f);
  Bool_knob(f, &_followStandard, "standard layer name format");
  Tooltip(f, "Older versions of Nuke write out channel names in the format: view.layer.channel. "
             "Check this option to follow the EXR standard format: layer.view.channel");

  Newline(f);
  Bool_knob(f, &_writeFullLayerNames, "write_full_layer_names", "write full layer names");
  Tooltip(f, "Older versions of Nuke just stored the layer name in the part "
             "name of multi-part files. Check this option to always write the "
             "layer name in the channel names following the EXR standard.");
  SetFlags(f, Knob::DISABLED);

  Newline(f);
  Bool_knob(f, &_truncateChannelNames, "truncateChannelNames", "truncate channel names");
  Tooltip(f, "Truncate channel names to a maximum of 31 characters for backwards compatibility");
}

int exrWriter::knob_changed(Knob *k)
{
  int changed = 0;

  if ( k == &Knob::showPanel || k->is("metadata") ) {
    Knob* noPrefixKnob = iop->knob( "noprefix");
    // Its possible that replaceable knobs may not exist if file_type knob is blank. This is allowed.
    if (noPrefixKnob) {
      noPrefixKnob->enable( ExrMetaDataMode(_metadataMode) >= eAllMetadataExceptInput );
    }
    changed = 1;
  }
  if ( k == &Knob::showPanel || k->is("interleave") ) {

    // set the state of the First Part knob.
    updateFirstPartMenuState();

    Knob* writeFullLayerNamesKnob = iop->knob( "write_full_layer_names");
    if (writeFullLayerNamesKnob) {
      writeFullLayerNamesKnob->enable( _multipartInterleaveMode == eInterleave_Channels );
    }
    Knob* truncateChannelNamesKnob = iop->knob( "truncateChannelNames");
    if (truncateChannelNamesKnob) {
      truncateChannelNamesKnob->enable( _multipartInterleaveMode == eInterleave_Channels_Layers_Views );
    }
    changed = 1;
  }

  Knob* compressionKnob = iop->knob("compression");
  if (k == &Knob::showPanel || k == compressionKnob) {
    Knob* compressionLevel = iop->knob("dw_compression_level");
    if( compressionLevel ) {
      if ( compressionHasLevel() ) {
        compressionLevel->enable(true);
        compressionLevel->show();
      }
      else{
        compressionLevel->enable(false);
        compressionLevel->hide();
      }
    }
    changed = 1;
  }

  const bool isStereoscopic = (executingViews().size() > 1) && (_multipartInterleaveMode == eInterleave_Channels_Layers_Views);
  const bool isAcesStereo = isStereoscopic && _acesFormat;

  Knob*const dataTypeKnob = iop->knob("datatype");
  Knob*const multiViewKnob = iop->knob("views");
  Knob*const leftViewKnob = iop->knob("left_view");
  Knob*const rightViewKnob = iop->knob("right_view");
  Knob*const heroViewKnob = iop->knob("heroview");

  if (k->is("write_ACES_compliant_EXR")) {
    compressionKnob->enable(!_acesFormat);
    dataTypeKnob->enable(!_acesFormat);

    multiViewKnob->enable(!isAcesStereo);
    multiViewKnob->visible(!isAcesStereo);

    heroViewKnob->enable(!isAcesStereo);
    heroViewKnob->visible(!isAcesStereo);

    leftViewKnob->enable(isAcesStereo);
    leftViewKnob->visible(isAcesStereo);

    rightViewKnob->enable(isAcesStereo);
    rightViewKnob->visible(isAcesStereo);

    changed = 1;
  }
  return changed;
}

void exrWriter::autocrop_tile(Tile& img, ChannelMask channels,
                              int* bx, int* by, int* br, int* bt)
{
  int xcount, ycount;

  *bx = img.r();
  *by = img.t();
  *br = img.x();
  *bt = img.y();

  foreach (z, channels) {
    for (ycount = img.y(); ycount < img.t(); ycount++) {
      for (xcount = img.x(); xcount < img.r(); xcount++) {
        if (img[z][ycount][xcount] != 0) {
          if (xcount < *bx)
            *bx = xcount;
          if (ycount < *by)
            *by = ycount;
          break;
        }
      }
    }

    for (ycount = img.t() - 1; ycount >= img.y(); ycount--) {
      for (xcount = img.r() - 1; xcount >= img.x(); xcount--) {
        if (img[z][ycount][xcount] != 0) {
          if (xcount > *br)
            *br = xcount;
          if (ycount > *bt)
            *bt = ycount;
          break;
        }
      }
    }
  }

  if (*bx > *br || *by > *bt)
    *bx = *by = *br = *bt = 0;
}


void exrWriter::updateFirstPartMenuState()
{

  const bool isAllSelected =  iop ? iop->channels().all() : false;
  const bool isChannels = _multipartInterleaveMode == eInterleave_Channels;
  const bool enabled = isAllSelected && isChannels;
  if(_firstPartKnob) {
    _firstPartKnob->enable(enabled);
  }
}

std::string exrWriter::getFirstPartMenuValue() const
{
  std::string  ret = "none";
  if(_firstPartKnob && _firstPartKnob->get_value() > 0) {
    ret = getLayerName(static_cast<DD::Image::Channel>(static_cast<size_t>(_firstPartKnob->get_value())));
  }
  return ret;
}


// TP 326656 if environment variable is set
// save the .tmp files using the 'filename'.exr.tmp format
// this code will be called once to detect the environment variable, and
// set an internal function pointer to the correct generate method.
std::string exrWriter::getTempFileName()
{
  std::string tempName = createFileHash();
  const char* const useFilenameAsTempName = std::getenv("NUKE_EXR_TEMP_NAME");
  if(useFilenameAsTempName && !std::strcmp(useFilenameAsTempName, "1")) {
    tempName = std::string(filename()).append(".");
#ifdef FN_OS_WINDOWS

    tempName.append(std::to_string(_getpid()));
#else
    tempName.append(std::to_string(getpid()));
#endif
  }
  tempName.append(".tmp");
  return tempName;
}

}
}
