// psdReader.cpp
// Copyright (c) 2011 The Founlry Visionmongers Ltd.  All Rights Reserved.

#include "DDImage/DDString.h"
#include "DDImage/Iop.h"
#include "DDImage/Reader.h"
#include "DDImage/Row.h"
#include "DDImage/ARRAY.h"
#include "DDImage/Thread.h"
#include "DDImage/Knob.h"
#include "DDImage/Memory.h"

#include "boost/scoped_array.hpp"

#include <ctype.h>
#include <math.h>
#include <zlib.h>

#ifndef FN_OS_WINDOWS
  #include <arpa/inet.h>
#endif

#define CHAR_TO_SHORT(str)      ((*(str) << 8) | *((str) + 1))

// #cristian
// This is not currently used anywhere in code, but it can be of use when we decide to improve this plugin
// #define CHAR_TO_INT(str)        ((*(str) << 24) | (*((str) + 1) << 16) | (*((str) + 2) << 8) | *((str) + 3))
//

//////////////////////////////////////
//
// ADOBE FILE format - http://www.adobe.com/devnet-apps/photoshop/fileformatashtml/PhotoshopFileFormats.htm
//

using namespace DD::Image;

static unsigned readShort(FILE* file)
{
  U16 v;
  fread(&v, 2, 1, file);
  DD::Image::Reader::frommsb(&v, 1);
  return v;
}

static long readLong(FILE* file)
{
  U32 v;
  fread(&v, 4, 1, file);
  DD::Image::Reader::frommsb(&v, 1);
  return v;
}

static long readLongRound4(FILE* file)
{
  U32 v;
  fread(&v, 4, 1, file);
  DD::Image::Reader::frommsb(&v, 1);
  v = (v + 3 ) & ~3;
  return v;
}

static float readFloat(FILE* file)
{
  static_assert(sizeof(U32) == sizeof(float), "float is not of expected size");
  U32 v;
  fread(&v, 4, 1, file);
  DD::Image::Reader::frommsb(&v, 1);
  return reinterpret_cast<float&>(v);
}

static double readDouble(FILE* file)
{
  static_assert(sizeof(U64) == sizeof(double), "double is not of expected size");
  U64 v;
  fread(&v, 8, 1, file);
  DD::Image::Reader::frommsb(&v, 1);
  return reinterpret_cast<double&>(v);
}

static std::string readFourChar(FILE* file)
{
  char c[5];
  memset(c, 0, 5);
  fread(c, 1, 4, file);
  return std::string(c);
}

class PSDLayer
{
public:
  char* name;
  char* nukeName;
  long nChan;
  short chanID[32];
  long chanSize[32];
  long channelStart[32];
  Channel channelMap[32];
  int x, y, r, t;
  unsigned char* array[32];

  struct Mask {
    bool used;
    int x,y,r,t;
    char flags;
    char color;

    bool positionRelative() {
      // bit 0
      return flags & 0x01;
    }

    bool disabled() {
      // bit 1
      return flags & 0x02;
    }

    bool invert() {
      // bit 3
      return flags & 0x04;
    }

    Mask() { used = false; }
  } mask;


  PSDLayer()  {
    for ( int i = 0; i < 32 ; i++)
      array[i] = nullptr;
  }

  ~PSDLayer() {
    for ( int i = 0; i < 32 ; i++) {
      if ( array[i] )
        Memory::deallocate_void( array[i] );
    }
    free( name );
    free( nukeName );
  }

  void print()
  {
#if 0
    printf("----- Layer %s -----\n", name);
    printf("x: %d, y: %d, r: %d, t: %d, w: %d, h:%d\n", x, y, r, t, r - x, t - y);
    for (int i = 0; i < nChan; i++) {
      printf("  psd chan %d, Nuke chan %d, start %d\n", i, channelMap[i], channelStart[i]);
    }
#endif
  }
};

class psdReaderFormat : public ReaderFormat
{
public:

  psdReaderFormat()
  {
  }

  void knobs(Knob_Callback cb) override
  {
    PyScript_knob(cb, "nukescripts.psd.doReaderBreakout()", "breakout", "Breakout Layers" );
    Tooltip(cb, "Breaks out the PSD file into seperate layers and re-combines them together with merges. "
            "The blending modes are approximated and do not match Photoshop exactly. "
            "It is recommended that all masks and adjustment layers are rasterized in Photoshop before importing into NUKE.");
  }

  void append(Hash& hash) override
  {
  }
};


static ReaderFormat* buildformat(Read* iop)
{
  return new psdReaderFormat();
}

class psdReader : public Reader
{
  FILE* file;
  int depth, width, height;
  double pixelAspect;
  int bpc;
  char* layername;
  long image_start;
  unsigned char* array;
  PSDLayer* layer;
  int nLayer;
  ChannelSet mask;
  Lock lock;

  MetaData::Bundle _meta;
  const MetaData::Bundle& fetchMetaData(const char* key) override
  {
    return _meta;
  }

  bool getFileHeader()
  {
    fseek(file, 0, SEEK_SET);
    char buf[5] = { 0 };
    fread(buf, 1, 4, file);
    if (strncmp (buf, "8BPS", 4)) {
      iop->internalError("Not a psd file (needs \"8BPS\" in header)");
      return false;
    }
    short version = readShort(file);
    if (version != 1) {    // the only version that is documented by Adobe
      iop->internalError("psd version %d is not supported", version);
      return false;
    }
    fseek(file, 6, SEEK_CUR);
    depth  = readShort(file);
    height = readLong(file);
    width  = readLong(file);
    bpc = readShort(file);
    if (bpc != 8 && bpc != 16) {
      iop->internalError("psd bit depth of %d is not supported", bpc);
      return false;
    }
    short mode = readShort(file);

    _meta.setData(MetaData::DEPTH, MetaData::DEPTH_FIXED(bpc));

    if (mode == 1 && depth >= 1)
      return true;                            // grayscale
    if (mode == 3 && depth >= 3)
      return true;                            // RGB w/ or w/o alpha
    iop->internalError("psd mode %d with depth %d is not supported", mode, depth);
    // Other modes:
    // 0=bitmap, 2=indexed color, 4=cmyk, 7=multichannel, 8=duotone, 9=lab
    // 6 may also be duotone

    return false;
  }

  // ignore the color mode. No support for LUTs
  // If data was there, it would be a 768 byte lookup table for 256 colors
  bool getColorModeData()
  {
    long size = readLong(file);
    fseek(file, size, SEEK_CUR);
    return true;
  }

  // Assumes that the file pointer is at the beginning of a resource block.
  // Reads and checks the signature for an image resource block.
  bool getResourceBlockSignature()
  {
    char buf[4] = {0};
    fread(buf, 1, 4, file);
    return strncmp(buf, "8BIM", 4) == 0;
  }

  void skipResourceBlockName()
  {
    // Name is Pascal string meaning the first byte is the size.
    // It is padded to the multiple of two, so adjust size accordingly.
    unsigned char nameSize = fgetc(file);
    fseek(file, nameSize, SEEK_CUR);
    // If the name (including the size byte) had odd bytes, advance file pointer for padding.
    if ((nameSize + 1) & 0x01) {
      fgetc(file);
    }
  }

  // Assumes that file pointer is at the beginning of Name field in the block.
  // Advances file pointer to the beginning of the signature for the next block.
  // The remaining parts in the block to jump over are:
  // - Name (pascal string, meaning the first byte is the size), padded to the multiple of 2,
  // - 4 bytes specifying the size of resource data,
  // - the resource data.
  void skipResourceBlock()
  {
    // Jump over the name block
    skipResourceBlockName();

    // Jump over the data block (takes to the next block or outside resource section)
    unsigned int dataSize = readLong(file); // this function reads 4 bytes
    fseek(file, dataSize, SEEK_CUR);
  } 

  // Jumps over the name field (similar to skipResourceBlock), but reads the data value.
  // According to psd spec, the value is expected to be 4 or 8 byte double.
  double getPixelAspectRatio()
  {
    skipResourceBlockName();

    // The size should be 4 or 8. For anything other than that, just skip
    unsigned int dataSize = readLong(file);
    if (dataSize == 4) {
      pixelAspect = static_cast<double>(readFloat(file));
    }
    else if (dataSize == 8) {
      pixelAspect = readDouble(file);
    }
    else {
      fseek(file, dataSize, SEEK_CUR);
    }
    return pixelAspect;
  } 

  // Ignore these blocks for now
  bool getImageResources()
  {
    long size = readLong(file);
    long start = ftell(file);

    static constexpr unsigned short kPixelAspectID = 1064;
    long current = start;
    while (current < size + start) {

      const bool isResourceBlock = getResourceBlockSignature();
      // Break the loop if unexpected signature is read
      if (!isResourceBlock) {
        break;
      }

      const unsigned short blockId = readShort(file);
      
      // If this is the pixel aspect ratio block, read and exit the loop
      if (blockId == kPixelAspectID) {
        getPixelAspectRatio();
        break;
      }
      // For any other resource, jump over to the next block
      skipResourceBlock();
      current = ftell(file);
    }

    // Jump to the end of resources section based on block size and starting point
    // since the loop above may have teminated mid point (e.g. if unexpected value is encountered).
    fseek(file, start + size, SEEK_SET);
    return true;
  }

  bool getAdditionalLayerInfoSignature();
  bool getAdditionalLayerInfo(long endSection);
  bool getLayerAndMaskInfo();
  bool getLayerInfo();
  bool readLayers();

  bool getMaskInfo ()
  {
    long size = readLong(file);
    fseek(file, size, SEEK_CUR);
    return true;
  }

  void rleDecode(unsigned char* d);
  void rleDecode(unsigned char* d, long len);
  void copyDecode(unsigned char* d, long len);
  void rleDecode(U16* d, long len);
  void zipDecode(U16* d, int len, int srcSize);
  void zipDiff(U16* d, int w, int h);
  void copyDecode(U16* d, long len);
  void getImageData();

public:
  psdReader (Read*, int fd);
  ~psdReader () override;
  void engine (int y, int x, int r, ChannelMask, Row &) override;
  void open () override;
  static const Description d;
};

static Reader* build (Read* iop, int fd, const unsigned char* b, int n)
{
  return new psdReader (iop, fd);
}

static bool test (int fd, const unsigned char* block, int length)
{
  return strncmp((char*)block, "8BPS", 4) == 0;
}

const Reader::Description psdReader::d ("psd\0", build, test, buildformat);

psdReader::psdReader (Read* r, int fd) : Reader (r), array(nullptr), nLayer(0)
{

  file = fdopen(fd, "rb");
  depth = width = height = 0;
  pixelAspect = 0.0;
  layername = nullptr;
  layer = nullptr;

  // defaults for an error
  mask = Mask_RGB;
  set_info(1, 1, 3);
  info_.channels(mask);
  info_.ydirection(-1);

  if (!getFileHeader())
    return;
  if (!getColorModeData())
    return;
  if (!getImageResources())
    return;
  if (!getLayerAndMaskInfo())
    return;

  // add the base channels
  switch (depth) {
    case 1: mask = Mask_Red;
      break;
    case 2: mask = Mask_Red | Mask_Alpha;
      break;
    case 3: mask = Mask_RGB;
      break;
    default: mask = Mask_RGBA;
      break;
  }

  // now add channels for all extra layer in our image
  for (int i = 0; i < nLayer; i++) {
    PSDLayer& l = layer[i];
    if (l.nChan < 1)
      continue;
    // skip empty layers that seem to be common:
    if (l.r <= l.x || l.t <= l.y)
      continue;

    char name[280];
    strlcpy( name, l.nukeName, sizeof(name));
    char *p = name + strlen(name);

    for ( int pass = 0; pass < 3 ; pass++ ) {
        for (int j = 0; j < l.nChan; j++) {
          // re-order so RGB comes first then alpha, then the rest
          bool isRGB = ( l.chanID[j] >= 0 && l.chanID[j] <= 2 );
          bool isAlpha = l.chanID[j] == -1;

          if ( pass == 0 && ! isRGB )
            continue;
          else if ( pass == 1 && ! isAlpha )
            continue;
          else if ( pass == 2 && ( isRGB || isAlpha ) )
            continue;

          switch (l.chanID[j]) {
            case 0: strcpy(p, ".red");
              break;
            case 1: strcpy(p, ".green");
              break;
            case 2: strcpy(p, ".blue");
              break;
            case - 1: strcpy(p, ".alpha");
              break;
            case - 2: strcpy(p, ".mask");
              break;
            default:
              if (l.chanID[j] < 0)
                sprintf(p, ".c%d_idn%d", j, -l.chanID[j]);
              else
                sprintf(p, ".c%d_id%d", j, l.chanID[j]);
              break;
          }

          Channel ch = DD::Image::getChannel(name, false );
          l.channelMap[j] = ch;
          mask += (ch);
        }
      }
    l.print();
  }

  set_info(width, height, 3, pixelAspect);
  info_.channels(mask);
  info_.ydirection(-1);
}

psdReader::~psdReader ()
{
  fclose(file);
  delete[] array;
}

void psdReader::open()
{
}

bool psdReader::getLayerAndMaskInfo()
{
  long size = readLong(file);
  long here = ftell(file);

  // Don't start reading layer & mask info if there isn't supposed to be any
  bool ret = true;
  if (size > 0) {
    ret = getLayerInfo();

    if (ret)
      ret = getMaskInfo();

    getAdditionalLayerInfo( here + size );
  }

  fseek(file, here + size, SEEK_SET);
  image_start = here + size;
  return ret;
}

bool psdReader::getLayerInfo()
{
  long size = readLong(file);
  long here = ftell(file);
  if ( bpc == 16 && size != 0 ) {
    iop->internalError( "error reading 16 bit psd, unexpected 8 bit layer info");
    return false;
  }

  if ( size != 0 )
    readLayers();

  fseek(file, here + size, SEEK_SET);
  return true;
}

bool psdReader::getAdditionalLayerInfoSignature()
{
  std::string key = readFourChar(file);
  if ( key != "8BIM" && key != "8B64" ) {
    std::stringstream ss;
    ss << "Bad psd additional info layer signature " << key;
    iop->warning ( ss.str().c_str() );
    return false;
  }
  return true;
}

bool psdReader::getAdditionalLayerInfo( long end )
{
  long here = ftell(file);
  if ( here > end ) {
    // we got bad data and will seek beyond the end of the section, skip and ignore
    return false;
  }

  while ( here < end ) {
    if ( ! getAdditionalLayerInfoSignature() )
      return false;
    std::string key = readFourChar(file);
    // round up to 4 byte border
    // this appears to be correct with the files that have been tested, although the PSD spec
    // says 'Length data below, rounded up to an even byte count.'
    long len = readLongRound4(file);
    here = ftell( file );

    if ( key == "Lr16" ) {
      readLayers();
      break;
    } else {
      fseek( file, len, SEEK_CUR);
      here += len + 4 /*4CHAR*/ + 4 /*4CHAR*/ + 4 /*LONG*/;
    }
  }
  return true;
}

bool psdReader::readLayers()
{
  short nLayer = readShort(file);
  if (nLayer < 0)
    nLayer = -nLayer;

  this->nLayer = nLayer;
  layer = new PSDLayer[nLayer];
  memset(layer, 0, sizeof(PSDLayer) * nLayer);

  // now read the data for each layer (name, size, compositing scheme)

  std::string layerBase = "input/psd/layers/";

  int ln;
  for (ln = 0; ln < nLayer; ln++) {
    std::stringstream layerKey;
    layerKey << layerBase << ln << "/";
    PSDLayer& l = layer[ln];
    l.y = readLong(file);
    l.x = readLong(file);
    l.t = readLong(file);
    l.r = readLong(file);

    _meta.setData( layerKey.str() + "y", height - l.y );
    _meta.setData( layerKey.str() + "x", l.x );
    _meta.setData( layerKey.str() + "t", height - l.t );
    _meta.setData( layerKey.str() + "r", l.r );

    layer[ln].nChan = readShort(file);
    short nc   = short(layer[ln].nChan);
    for (int c = 0; c < nc; c++) {
      layer[ln].chanID[c]   = readShort(file);
      layer[ln].chanSize[c] = readLong(file);
    }

    char buf[5];
    fread(buf, 1, 4, file); // should read '8BIM'
    fread(buf, 1, 4, file); // blend mode key
    buf[4] = 0;
    _meta.setData( layerKey.str() + "blendmode", buf );

    unsigned char lData  = fgetc(file); // opacity
    _meta.setData( layerKey.str() + "opacity", lData );

    fgetc(file); // clipping

    lData = fgetc(file); // flags
    _meta.setData( layerKey.str() + "flags", lData );

    fgetc(file); // filler

    long extraSize = readLong(file);    // remember the extra data size
  long extraHere = ftell(file);    
  long extraRead = 0;

    // layer mask info
    long layerMaskSize = readLong(file);
    long here = ftell(file);

    if ( layerMaskSize != 0 ) {
      l.mask.used = true;
      l.mask.y = readLong(file);
      l.mask.x = readLong(file);
      l.mask.t = readLong(file);
      l.mask.r = readLong(file);
      l.mask.color = fgetc(file);
      l.mask.flags = fgetc(file);

      _meta.setData( layerKey.str() + "mask/y", height - l.y );
      _meta.setData( layerKey.str() + "mask/x", l.x );
      _meta.setData( layerKey.str() + "mask/t", height - l.t );
      _meta.setData( layerKey.str() + "mask/r", l.r );

      _meta.setData( layerKey.str() + "mask/color", l.mask.color );
      _meta.setData( layerKey.str() + "mask/flags", l.mask.flags );
      _meta.setData( layerKey.str() + "mask/disable", l.mask.disabled() );
      _meta.setData( layerKey.str() + "mask/invert", l.mask.invert() );
      _meta.setData( layerKey.str() + "mask/positionRelative", l.mask.positionRelative() );
    }

    fseek(file, here + layerMaskSize, SEEK_SET);
    extraRead += 4 + layerMaskSize;

    long layerBlendingSize = readLong(file);    // skip the layer blending
    fseek(file, layerBlendingSize, SEEK_CUR);
    extraRead += 4 + layerBlendingSize;

    unsigned char nameSize = fgetc(file);
    char name[257];
    fread(name, 1, nameSize, file);
    name[nameSize] = 0;

    // skip to 4 byte boundary
    fseek(file, (3-(nameSize%4)), SEEK_CUR );
    extraRead  += nameSize + 4 - nameSize%4;

    if (name[0] == 0)
      strcpy(name, "background");
    layer[ln].name = strdup(name);

    // Make the name nuke-friendly:
    char nukeName[280];
    // copy leaving enough room at end for channel name:
    if (isdigit(layer[ln].name[0])) {
      name[0] = '_';
      strlcpy(nukeName + 1, layer[ln].name, 279 - 10);
    }
    else {
      strlcpy(nukeName, layer[ln].name, 280 - 10);
    }

    char* p = nukeName;
    int junk = 0;
    for (; *p; ++p) {
      if (!isalnum(*p)) {
        *p = '_';
        junk++;
      }
    }
    if (p - nukeName > 30 || junk > (p - nukeName) / 3)
      p = name + sprintf(name, "layer%d", ln);

    layer[ln].nukeName = strdup(nukeName);

    _meta.setData( layerKey.str() + "name", layer[ln].name );
    _meta.setData( layerKey.str() + "nukeName", layer[ln].nukeName );

    here = ftell(file);

    while ( extraRead < extraSize) {
      if ( ! getAdditionalLayerInfoSignature() )
        break;
      std::string key = readFourChar(file);
      long len = readLongRound4(file);
      here = ftell( file );
      extraRead += 12 + len;

   if ( key == "lsct") {
        long dividerType = readLong(file);
        _meta.setData( layerKey.str() + "divider/type", (int)dividerType );
      }
      fseek( file, here + len, SEEK_SET );
    }
    fseek(file, extraHere + extraSize, SEEK_SET); // skip unknown data
  }


  // find the file offset for the pixel data of each layer and channel
  long img_data = ftell(file);
  for (ln = 0; ln < nLayer; ln++) {
    PSDLayer& l = layer[ln];
    for (int c = 0; c < l.nChan; c++) {
      l.channelStart[c] = img_data;
      img_data += l.chanSize[c];
    }
  }
  return true;
}


// This is a guess! Not tested!
void psdReader::rleDecode(U16* d, long len)
{
  for (; len > 0;) {
    int k = readShort(file);
    if (k >= 0) {
      int n = k + 1;
      if (n > len)
        n = len;
      fread(d, 2, n, file);
      frommsb(d, n);
      d += n;
      len -= n;
    }
    else {
      int n = -k + 1;
      if (n > len)
        n = len;
      int c = readShort(file); // get high byte
      for (int i = 0; i < n; i++)
        *d++ = c;
      len -= n;
      fgetc(file); // ignore low byte
    }
  }
}

void psdReader::rleDecode(unsigned char* d, long len)
{
  for (; len > 0;) {
    signed char k = fgetc(file);
    if (k >= 0) {
      int n = k + 1;
      if (n > len)
        n = len;
      fread(d, 1, n, file);
      d += n;
      len -= n;
    }
    else {
      int n = -k + 1;
      if (n > len)
        n = len;
      memset(d, fgetc(file), n);
      d += n;
      len -= n;
    }
  }
}

////////////////////////////////////////////////////////////////////////////////
// RLE Algorithm - Decodes the blended image PSD image.
// Do not use for other layers
//
// @param uncompressedData - an allocated unsigned char* array to store the 
// decompressed image information required for decoding a frame:
//    - is in your responsibility to provide a large enough buffer to store 
//      the decompressed data
//
void psdReader::rleDecode(unsigned char* uncompressedData)
{
  const int8_t srcDepth = (depth < 4) ? depth : 4;

  const uint64_t resolution = width * height;
  const uint64_t mFnUnusedVariable pixelsCount = (bpc > 8) ? resolution * 2 : resolution; // if bpc > 8 then we have a 16-bit image so each pixel will be twice the size of an 8 bit pixel
  const uint64_t compressedDataSize = height * depth * sizeof(U16);
  const uint64_t currentStreamAddress = ftell(file);

  fseek(file, 0, SEEK_END);
  const uint64_t endStreamAddress = ftell(file);
  fseek(file, currentStreamAddress, SEEK_SET);

  const uint64_t leftSizeToRead = endStreamAddress - currentStreamAddress;
  boost::scoped_array<uint8_t> compressedDataBuffer(new uint8_t[leftSizeToRead]);
  
  fread(compressedDataBuffer.get(), sizeof(uint8_t), leftSizeToRead, file);

  const uint8_t* countDataPtr = compressedDataBuffer.get();
  const uint8_t* pixelDataPtr = compressedDataBuffer.get() + compressedDataSize;

  // RLE compressed image data starts with the byte counts for all the
  // scan lines (rows * color_channels), with each count stored as a two-byte value.
  // The RLE compressed data follows, with each scan line compressed
  // separately. The RLE compression is the same compression algorithm
  // used by the Macintosh ROM routine PackBits, and the TIFF standard.
  int i, j, k;
  for(i = 0; i < srcDepth; ++i) {
    uint64_t pixelCountSC = 0;
    for(j = 0; j < height; ++j) {
      const U16 byteCount = CHAR_TO_SHORT(countDataPtr);
      for(k = 0; k < byteCount;) {
        int16_t len = *pixelDataPtr;
        ++pixelDataPtr;
        ++k;

        if(len < 128) {
          ++len;
          pixelCountSC += len;
          memcpy(static_cast<void*>(uncompressedData), static_cast<const void*>(pixelDataPtr), len);
          uncompressedData += len;
          pixelDataPtr += len;
          k += len;
        }
        else if(len > 128) {
          // Next -len+1 bytes in the dest are replicated from next source byte.
          // (Interpret len as a negative 8-bit int.)
          len ^= 0xff;
          len += sizeof(U16);
          pixelCountSC += len;
          memset(static_cast<void*>(uncompressedData), *pixelDataPtr, len);
          uncompressedData += len;
          ++pixelDataPtr;
          ++k;
        }
        // else len == 128, do nothing
      }
      countDataPtr += sizeof(U16);
    }

    mFnAssertMsg((pixelCountSC == pixelsCount), "Invalid number of pixels have been read!");
  }
}

void psdReader::copyDecode(U16* d, long len)
{
  fread(d, 2, len, file);
  frommsb(d, len);
}

void psdReader::copyDecode(unsigned char* d, long len)
{
  fread(d, 1, len, file);
}

void psdReader::zipDecode(U16* dst, int len, int srcLen )
{
  unsigned char* srcBuf = new unsigned char[srcLen];
  fread( srcBuf, 1, srcLen, file);

  uLongf destLen = len;
  uncompress( (Bytef*)dst, &destLen, srcBuf, srcLen );

  delete[] srcBuf;
}

void psdReader::zipDiff(U16* dst, int w, int h)
{
  U16* p = dst;
  for (int i = 0; i < h; i++) {
  U16 val = 0;
  for (U16* end = p + w; p != end; p++) {
    val += htons(*p);
    *p = val;
    }
  }
}


/* The entire non-layer image is stored as a single compressed block.
   It appears the array of sizes before this is useless, as the compression
   goes right across the boundaries between the lines. So instead this
   allocates the entire buffer and reads it all in at once.
 */
void psdReader::getImageData()
{
  Guard guard(lock);
  if (array)
    return;          // another thread did it
  fseek(file, image_start, SEEK_SET);
  short cp = readShort(file); // compression type
  int srcDepth = depth;
  if (depth > 4)
    srcDepth = 4;
  if (bpc > 8) {
    uLongf dstSize = width * height * srcDepth;
    U16* dst = new U16[dstSize];
    if (cp == 0) {        // uncompressed
      copyDecode(dst, dstSize);
    }
    else if (cp == 1) {          // run length encoding
      rleDecode(reinterpret_cast<uint8_t*>(dst));
    }
    else {
      iop->internalError("psd compression type %d is not supported", cp);
    }
    array = (unsigned char*)dst;
  }
  else {
    unsigned char* dst = new unsigned char[width * height * srcDepth];
    if (cp == 0) {        // uncompressed
      copyDecode(dst, width * height * srcDepth);
    }
    else if (cp == 1) { // run length encoding
      rleDecode(dst);
    }
    else {
      iop->internalError("psd compression type %d is not supported", cp);
    }
    array = dst;
  }
}

void psdReader::engine (int y, int x, int r, ChannelMask c1, Row& row)
{
  ChannelSet channels(c1);
  int py = height - y - 1;
  for (Channel z = Chan_Red; z <= Chan_Alpha; incr(z))
    if (intersect(channels, z)) {
      if (!array)
        getImageData();
      int i = z - 1;
      if (i >= depth)
        i = depth - 1;
      if (bpc > 8)
        from_short(z, row.writable(z) + x, (U16*)array + (i * height + py) * width + x, nullptr, r - x, 16);
      else
        from_byte(z, row.writable(z) + x, array + (i * height + py) * width + x, nullptr, r - x, 1);
    }

  channels -= (Mask_RGBA);
  if (!channels)
    return;

  for (int lnum = 0; lnum < nLayer; lnum++) {
    PSDLayer& l = layer[lnum];
    for (int cnum = 0; cnum < l.nChan; cnum++) {
      Channel z = l.channelMap[cnum];
      int ly, lr, lx, lt;

      if ( l.mask.used && l.chanID[cnum] == -2 ) {
        ly = l.mask.y;
        lr = l.mask.r;
        lx = l.mask.x;
        lt = l.mask.t;
      } else {
        ly = l.y;
        lr = l.r;
        lx = l.x;
        lt = l.t;
      }

      if (intersect(channels, z)) {
        if (py < ly || py >= lt) {
          row.erase(z);
          continue;
        }
        long cstart = l.channelStart[cnum];
        if (!cstart) {
          row.erase(z);
          continue;
        }
        ARRAY(U16, src, lr - lx);
        unsigned char* srcPtr = (unsigned char*)(&(src[0]));
        {
          Guard guard(lock);
          fseek(file, cstart, SEEK_SET);
          short cp = readShort(file); // compression type?!

          if (cp == 1) {
            long off = 0;
            for (int yy = py - ly; yy > 0; yy--)
              off += readShort(file);
            fseek(file, cstart + off + 2 + 2 * (lt - ly), SEEK_SET);
            if (bpc > 8) {
              rleDecode(src, lr - lx);
              srcPtr = (unsigned char*)&src;
            }
            else {
              rleDecode(srcPtr, lr - lx);
            }
          }
          else if (cp == 0) {
            if (bpc > 8) {
              fseek(file, cstart + 2 + 2 * (lr - lx) * (py - ly), SEEK_SET);
              copyDecode(src, lr - lx);
              srcPtr = (unsigned char*)&src;
            }
            else {
              fseek(file, cstart + 2 + (lr - lx) * (py - ly), SEEK_SET);
              copyDecode(srcPtr, lr - lx);
            }
          } else if ( (cp == 2 || cp == 3)  && bpc == 16) {
            //  PSD format stores whole layer compressed in one big zip block
            if ( ! l.array[cnum] ) {
              uLongf dstSize =  ( lr - lx ) * ( lt - ly ) * sizeof(U16);
              l.array[cnum] = (unsigned char*)Memory::allocate_void( dstSize );
              int srcSize = l.chanSize[cnum];
              srcSize -= sizeof(U16);
              zipDecode( (U16*)l.array[cnum], dstSize, srcSize );
              if ( cp == 3 )
                zipDiff( (U16*)l.array[cnum], lr - lx, lt - ly );
            }
            // move charsrc to right spot in 'array'
            int rowsize = (lr - lx) * sizeof(U16);
            srcPtr =  (unsigned char*)(&(l.array[cnum][0])) + (rowsize) * (py - ly);
          }
          else {
            iop->internalError("psd layer compression type %d is not supported", cp);
            row.erase(z);
            continue;
          }
        }
        float* dst = row.writable(z);
        int px = x;
        int pr = r;
        while (px < lx && px < pr)
          dst[px++] = 0;
        while (pr > lr && pr > px)
          dst[--pr] = 0;

        if (bpc > 8)
          from_short(l.chanID[cnum] >= 0 ? Chan_Red : Chan_Alpha,
                     dst + px, (U16*)srcPtr + px - lx , nullptr, pr - px, 16);
        else
          from_byte(l.chanID[cnum] >= 0 ? Chan_Red : Chan_Alpha,
                    dst + px, srcPtr + px - lx, nullptr, pr - px, 1);
      }
    }
  }
}

// end of psdReader.C
