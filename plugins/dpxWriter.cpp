// dpxWriter.C
// Copyright (c) 2009 The Foundry Visionmongers Ltd.  All Rights Reserved.

#include "DDImage/DDString.h"
#include "DDImage/FileWriter.h"
#include "DDImage/Row.h"
#include "DDImage/ARRAY.h"
#include "DDImage/DDMath.h"
#include "DDImage/Knobs.h"
#include "DDImage/LUT.h"
#include "DDImage/Enumeration_KnobI.h"
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include "DPXimage.h"

using namespace DD::Image;

static const char* const dnames[] = {
  "8 bit", "10 bit", "12 bit", "16 bit", nullptr
};
static int bits[] = {
  8, 10, 12, 16
};

static const char* const colourspaces[] = {
  "user-defined",
  "printing density",
  "linear",
  "log",
  "unspecified video",
  "SMPTE 240M",
  "CCIR 709-1",
  "CCIR 601-2 system B/G",
  "CCIR 601-2 system M",
  "NTSC",
  "PAL",
  "Z linear",
  "Z homogeneous", 
  "(auto detect)",
  nullptr
};

enum Colourspaces {
  eUserDefined, ePrintingDensity, eLinear, eLog, eUnspecified, eSMTP240M, 
  e709, e601BG, e601M, eNTSC, ePAL, eZLinear, eZHomogeneous, eAutoDetect 
};

class DPXWriter : public FileWriter
{
  static const Description d;
  int datatype;
  bool fill;
  bool YCbCr;
  bool bigEndian;

  int num_channels;
  int components;
  int bytes;

  int _colourspace;

public:
  const char* _timecode, * _edgecode;

  DPXWriter(Write* iop) : FileWriter(iop)
  {
    datatype = 1; // 10 bits
    fill = false;
    YCbCr = false;
    bigEndian = true; // true for back-compatibility
    _timecode = _edgecode = nullptr;

    _colourspace = eAutoDetect;
  }

  LUT* defaultLUT() const override
  {
    if (datatype == 1 && !YCbCr) {
      return LUT::GetLut(LUT::LOG, this);
    }
    else if (datatype) {
      return LUT::GetLut(LUT::INT16, this);
    }
    else {
      return LUT::GetLut(LUT::INT8, this);
    }
  }

  bool isDefaultLUTKnob(DD::Image::Knob* knob) const override
  {
    return knob->is("datatype");
  }

  void assignProp(R32& field,  const MetaData::Bundle& metadata, const char* propname);
  void assignProp(U32& field,  const MetaData::Bundle& metadata, const char* propname);
  void assignProp(U8& field,  const MetaData::Bundle& metadata, const char* propname);
  void assignProp(char* field, size_t fieldlen, const MetaData::Bundle& metadata, const char* propname);

  void knobs(Knob_Callback f) override
  {

    Enumeration_knob(f, &datatype, dnames, "datatype");
    Bool_knob(f, &fill, "fill");
    Tooltip(f, "Compress 10/12 bit data by removing unused bits.");
    //     Bool_knob(f, &YCbCr, "YCbCr", "4:2:2");
    //     Tooltip(f, "Write as YCbCr 4:2:2 data, which is 2/3 the size");
    Bool_knob(f, &bigEndian, "bigEndian", "big endian");
    Tooltip(f, "Force file to be big-endian, rather than native-endian. This is slower, but some programs will only read big-endian files");
    Enumeration_knob(f, &_colourspace, colourspaces, "transfer");
    Tooltip(f, "Set the transfer and colorimetric fields in the DPX header data to these values. By default it attempts to match it according to the used LUT but when using a custom LUT, this field can be used to override that. ");
  }

  void execute() override;

  const char* help() override 
  { 
    return "<b>Digital Picture Exchange (DPX)</b> is a common file format for digital intermediate and visual effects work and is a SMPTE standard\n"
           "Current version - SMPTE 268M-1994"; 
  }

private:
  DD::Image::LUT* findLutForColorspaceInfo();
};

static Writer* build(Write* iop)
{
  return new DPXWriter(iop);
}

const Writer::Description DPXWriter::d("dpx\0", "DPX", build);

void DPXWriter::assignProp(U8& field, const MetaData::Bundle& metadata, const char* propname)
{
  if (metadata.find(propname) == metadata.end())
    return;

  U8 value = metadata.getUnsignedChar(propname);
  field = value;
}

void DPXWriter::assignProp(R32& field, const MetaData::Bundle& metadata, const char* propname)
{
  if (metadata.find(propname) == metadata.end())
    return;

  double value = metadata.getDouble(propname);
  field = value;
  if (bigEndian)
    tomsb((U32*)&field, 1);
}

void DPXWriter::assignProp(U32& field, const MetaData::Bundle& metadata, const char* propname)
{
  if (metadata.find(propname) == metadata.end())
    return;

  unsigned value = (unsigned)metadata.getDouble(propname);
  field = value;
  if (bigEndian)
    tomsb((U32*)&field, 1);
}

void DPXWriter::assignProp(char* field, size_t fieldlen, const MetaData::Bundle& metadata, const char* propname)
{
  if (metadata.find(propname) == metadata.end())
    return;

  std::string value = metadata.getString(propname);
  strncpy(field, value.c_str(), fieldlen);
  field[fieldlen - 1] = 0;
  //if (bigEndian)
  //  tomsb((U32*)&field, 1);
}

void DPXWriter::execute()
{
  if (!open())
    return;

  unsigned int flippedInf = UNDEF_R32;
  tomsb((U32*)&flippedInf, 1);

  R32 undefR32 = bigEndian ? flippedInf : UNDEF_R32;

  MetaData::Bundle metaData = iop->fetchMetaData(nullptr);

  //Write header
  DPXHeader header;
  memset(&header, 0, sizeof(header));

  //FileInfo
  header.file.magicNumber = DPX_MAGIC;

  // Bug 23674 - DPX Writer should write the image data section on an 8k block boundary
  const unsigned int blockSize = 8*1024;
  unsigned int offsetToImageData = sizeof(header);
  unsigned int boundary = blockSize * (1 + (offsetToImageData / blockSize));

  const unsigned int imageBlockPadding = boundary - offsetToImageData;
  header.file.offsetToImageData = boundary;

  //file size = header + (rows*columns*bytes/pixel)
  header.file.totalFileSize = sizeof(header) + imageBlockPadding + (width() * height() * 4);
  header.file.dittoKey = 1; //0=same, 1=new
  header.file.genericHeaderSize = sizeof(header.file) + sizeof(header.image) + sizeof(header.orientation); //generic header length in bytes
  header.file.specificHeaderSize = sizeof(header.film) + sizeof(header.video); //industry header length in bytes
  header.file.userDataSize = 0; //user-defined data length in bytes
  if (bigEndian)
    tomsb(&header.file.magicNumber, 9);
  strcpy(header.file.version, "V1.0");
  strlcpy(header.file.imageFileName, filename(), 100);
  assignProp(header.file.imageFileName, sizeof(header.file.imageFileName),  metaData, MetaData::FILENAME);
  time_t fileClock = time(nullptr);
  struct tm* fileTime = localtime(&fileClock);
  strftime(header.file.creationTime, 24, "%Y:%m:%d:%H:%M:%S:%Z", fileTime);
  //Should add version in here
  sprintf(header.file.creator, "Nuke");
  assignProp(header.file.creator,       sizeof(header.file.creator),        metaData, MetaData::CREATOR);
  header.file.key = UNDEF_U32;
  assignProp(header.file.project,       sizeof header.file.project,         metaData, MetaData::PROJECT);
  assignProp(header.file.copyright,     sizeof header.file.copyright,       metaData, MetaData::COPYRIGHT);
  assignProp(header.file.creationTime,  sizeof header.file.creationTime,    metaData, MetaData::FILE_CREATION_TIME);

  // Film info
  assignProp(header.film.frameId,       sizeof header.film.frameId,         metaData, MetaData::DPX::FRAME_ID);
  assignProp(header.film.frameRate,     metaData, MetaData::FRAME_RATE);
  header.film.shutterAngle = undefR32;
  assignProp(header.film.shutterAngle,  metaData, MetaData::SHUTTER_ANGLE);
  assignProp(header.film.slateInfo,     sizeof header.film.slateInfo,       metaData, MetaData::SLATE_INFO);

  header.film.framePosition = undefR32;
  assignProp(header.film.framePosition, metaData, MetaData::DPX::FRAMEPOS);

  header.film.sequenceLen = UNDEF_U32;
  assignProp(header.film.sequenceLen,   metaData, MetaData::DPX::SEQUENCE_LENGTH);

  header.film.heldCount = UNDEF_U32;
  assignProp(header.film.heldCount,     metaData, MetaData::DPX::HELD_COUNT);
  assignProp(header.film.frameId,       sizeof header.film.frameId,         metaData, MetaData::DPX::FRAME_ID);

  // Television data
  header.video.timeCode = UNDEF_U32;
  assignProp(header.video.timeCode,     metaData, MetaData::DPX::TIME_CODE);
  header.video.userBits = UNDEF_U32;
  assignProp(header.video.userBits,     metaData, MetaData::DPX::USER_BITS);
  header.video.interlace = UNDEF_U8;
  assignProp(header.video.interlace,    metaData, MetaData::DPX::INTERLACE);
  header.video.fieldNumber = UNDEF_U8;
  assignProp(header.video.fieldNumber,  metaData, MetaData::DPX::FIELD_NUMBER);
  header.video.videoSignal = UNDEF_U8;
  assignProp(header.video.videoSignal,  metaData, MetaData::DPX::VIDEO_SIGNAL);
  header.video.horzSampleRate = undefR32;
  assignProp(header.video.horzSampleRate,  metaData, MetaData::DPX::HORIZ_SAMPLE);
  header.video.vertSampleRate = undefR32;
  assignProp(header.video.vertSampleRate,  metaData, MetaData::DPX::VERT_SAMPLE);
  header.video.frameRate = undefR32;
  assignProp(header.video.frameRate,    metaData, MetaData::DPX::FRAME_RATE);
  header.video.timeOffset = undefR32;
  assignProp(header.video.timeOffset,   metaData, MetaData::DPX::TIME_OFFSET);
  header.video.gamma = undefR32;
  assignProp(header.video.gamma,        metaData, MetaData::DPX::GAMMA);
  header.video.blackLevel = undefR32;
  assignProp(header.video.blackLevel,   metaData, MetaData::DPX::BLACK_LEVEL);
  header.video.blackGain = undefR32;
  assignProp(header.video.blackGain,    metaData, MetaData::DPX::BLACK_GAIN);
  header.video.breakpoint = undefR32;
  assignProp(header.video.breakpoint,   metaData, MetaData::DPX::BREAK_POINT);
  header.video.whiteLevel = undefR32;
  assignProp(header.video.whiteLevel,   metaData, MetaData::DPX::WHITE_LEVEL);
  header.video.integrationTimes = undefR32;
  assignProp(header.video.integrationTimes,  metaData, MetaData::DPX::INTEGRATION_TIMES);

  //Image data
  header.image.orientation = 0; //image orientation -- (left to right, top to bottom)
  header.image.numberElements = 1; //number of image elements
  if (bigEndian)
    tomsb(&header.image.orientation, 2);
  header.image.pixelsPerLine = width(); //x value
  header.image.linesPerImage = height(); //y value
  if (bigEndian)
    tomsb(&header.image.pixelsPerLine, 2);

  //Channel data
  for (int i = 0; i < 1; i++) {
    header.image.element[i].dataSign = 0; // data sign (0 = unsigned, 1 = signed )
    header.image.element[i].lowData = UNDEF_U32; //reference low data code value
    header.image.element[i].highData = UNDEF_U32; // reference high data code value
    header.image.element[i].lowQuantity = undefR32;
    header.image.element[i].highQuantity = undefR32;
    if (bigEndian)
      tomsb(&header.image.element[i].dataSign, 5);
    num_channels = Writer::num_channels();

    if (num_channels > 3) { // we are trying to write alpha
      num_channels = 4;
      if (YCbCr) {
        header.image.element[i].descriptor = DESCRIPTOR_CbYACrYA;
        components = 3;
      }
      else {
        header.image.element[i].descriptor = DESCRIPTOR_RGBA;
        components = 4;
      }
    }
    else if (num_channels > 1) {
      num_channels = 3;
      if (YCbCr) {
        header.image.element[i].descriptor = DESCRIPTOR_CbYCrY;
        components = 2;
      }
      else {
        header.image.element[i].descriptor = DESCRIPTOR_RGB;
        components = 3;
      }
    }
    else {
      num_channels = components = 1;
      header.image.element[i].descriptor = DESCRIPTOR_Y;
    }

    switch (datatype) {
      case 0: // 8 bits
        bytes = (width() * components + 3) & - 4;
        break;
      case 1: // 10 bits
        if (fill)
          bytes = (width() * components * 10 + 31) / 32 * 4;
        else
          bytes = (width() * components + 2) / 3 * 4;
        break;
      case 2: // 12 bits
        if (fill)
          bytes = (width() * components * 12 + 31) / 32 * 4;
        else
          bytes = (width() * components) * 2;
        break;
      case 3: // 16 bits
        bytes = (width() * components) * 2;
        break;
    }

    switch (_colourspace) {
      case eUserDefined:
          header.image.element[i].transfer = TRANSFER_USER;
          header.image.element[i].colorimetric = COLORIMETRIC_USER;
          break;
      case ePrintingDensity:
          header.image.element[i].transfer = TRANSFER_DENSITY;
          header.image.element[i].colorimetric = COLORIMETRIC_DENSITY;
          break;
      case eLinear:
          header.image.element[i].transfer = TRANSFER_LINEAR;
          header.image.element[i].colorimetric = COLORIMETRIC_USER;
          break;
      case eLog:
          header.image.element[i].transfer = TRANSFER_LOG;
          header.image.element[i].colorimetric = COLORIMETRIC_USER;
          break;
      case eUnspecified:
          header.image.element[i].transfer = TRANSFER_VIDEO;
          header.image.element[i].colorimetric = COLORIMETRIC_VIDEO;
          break;
      case eSMTP240M:
          header.image.element[i].transfer = TRANSFER_SMPTE_240M;
          header.image.element[i].colorimetric = COLORIMETRIC_SMPTE_240M;
          break;
      case e709:
          header.image.element[i].transfer = TRANSFER_CCIR_709_1;
          header.image.element[i].colorimetric = COLORIMETRIC_CCIR_709_1;
          break;
      case e601BG:
          header.image.element[i].transfer = TRANSFER_CCIR_601_2_BG;
          header.image.element[i].colorimetric = COLORIMETRIC_CCIR_601_2_BG;
          break;
      case e601M:
          header.image.element[i].transfer = TRANSFER_CCIR_601_2_M;
          header.image.element[i].colorimetric = COLORIMETRIC_CCIR_601_2_M;
          break;
      case eNTSC:
          header.image.element[i].transfer = TRANSFER_NTSC;
          header.image.element[i].colorimetric = COLORIMETRIC_NTSC;
          break;
      case ePAL:
          header.image.element[i].transfer = TRANSFER_PAL;
          header.image.element[i].colorimetric = COLORIMETRIC_PAL;
          break;
      case eZLinear:
          header.image.element[i].transfer = TRANSFER_Z_LINEAR;
          header.image.element[i].colorimetric = COLORIMETRIC_USER;
          break;
      case eZHomogeneous:
          header.image.element[i].transfer = TRANSFER_Z_HOMOGENOUS;
          header.image.element[i].colorimetric = COLORIMETRIC_USER;
          break;
      case eAutoDetect: {
        DD::Image::LUT* lutToUse = findLutForColorspaceInfo();
        if (lutToUse->linear() || lutToUse == LUT::GetDefaultLutForType(LUT::LOG)) {
          // Although there is a "logarithmic" value defined, in practice it
          // appears that other apps set/expect "user" for cineon log files.
          header.image.element[i].transfer = TRANSFER_USER;
          header.image.element[i].colorimetric = COLORIMETRIC_USER;
        } else if (lutToUse == LUT::GetBuiltinLutByName("rec709")) {
          header.image.element[i].transfer = TRANSFER_CCIR_709_1;
          header.image.element[i].colorimetric = COLORIMETRIC_CCIR_709_1;
        } else {
          header.image.element[i].transfer = TRANSFER_LINEAR;
          // There is no "linear" colorimetric; fall back on "user". Probably
          // it's any one of Panalog, REDlog, etc, etc making this a reasonable
          // value anyway.
          header.image.element[i].colorimetric = COLORIMETRIC_USER;
        }
        break;
      }
    }

    header.image.element[i].bits = bits[datatype];
    header.image.element[i].packing = fill ? 0 : 1;
    header.image.element[i].encoding = 0; // encoding for element (no run length encoding applied)
    if (bigEndian)
      tomsb(&header.image.element[i].packing, 2);

    header.image.element[i].dataOffset = sizeof(header) + imageBlockPadding;
    header.image.element[i].eolPadding = 0; // end of line padding used in element (no padding)
    header.image.element[i].eoImagePadding = 0; // end of image padding used in element (no padding)
    if (bigEndian)
      tomsb(&header.image.element[i].dataOffset, 3);

    assignProp(header.image.element[i].description, sizeof header.image.element[i].description, metaData, MetaData::ELEMENT_DESCRIPTION[i]);
  }

  //Image Orientation
  header.orientation.xOffset = 0;
  header.orientation.yOffset = 0;
  header.orientation.xCenter = R32( width() ) / 2.0f;
  header.orientation.yCenter = R32( height() ) / 2.0f;
  header.orientation.xOrigSize = width();
  header.orientation.yOrigSize = height();
  if (bigEndian)
    tomsb(&header.orientation.xOffset, 6);

  header.orientation.border[0] = 0;
  header.orientation.border[1] = 0;
  header.orientation.border[2] = 0;
  header.orientation.border[3] = 0;
  if (bigEndian)
    tomsb(&header.orientation.border[0], 4);

  header.orientation.pixelAspect[1] = 1200;
  header.orientation.pixelAspect[0] =
    (U32)(iop->format().pixel_aspect() * header.orientation.pixelAspect[1] + .5);
  if (bigEndian)
    tomsb(&header.orientation.pixelAspect[0], 2);

  assignProp(header.orientation.fileName,      sizeof header.orientation.fileName,       metaData, MetaData::DPX::FILE_NAME);
  assignProp(header.orientation.creationTime,  sizeof header.orientation.creationTime,   metaData, MetaData::DPX::CREATION_TIME);
  assignProp(header.orientation.inputName,     sizeof header.orientation.inputName,      metaData, MetaData::DPX::INPUT_DEVICE);
  assignProp(header.orientation.inputSN,       sizeof header.orientation.inputSN,        metaData, MetaData::DPX::INPUT_SN);

  std::string timecode = _timecode ? _timecode : "";

  if (timecode.length() == 0 && metaData.getData(MetaData::TIMECODE)) {
    timecode = metaData.getString(MetaData::TIMECODE);
  }

  if (timecode.length()) {
    // Skip everything except digits, encode as BCD:
    unsigned tc_bcd = 0;

    const char* timecodec = timecode.c_str();

    for (const char* q = timecodec; *q; q++) {
      if (*q >= '0' && *q <= '9') {
        tc_bcd = (tc_bcd << 4) + (*q - '0');
      }
    }

    // Work out if we are the correct fps for drop frame and that the timecode is a drop frame timecode.
    // Note that we don't support writing drop frame at 59.98fps as the timecode format is flawed for
    // this frame rate, i.e. it needs the 0x40 bit for both the drop frame flag and the frame number.
    const double fps = metaData.getDouble(MetaData::FRAME_RATE);
    const bool supportsDropFrame = DPXImage::isDropFrameSupported(fps);
    const size_t pos = timecode.find(';');
    const bool isDropFrame = (pos != std::string::npos);

    if(supportsDropFrame && isDropFrame) {
      tc_bcd |= DPXImage::kDropFrameFlag;
    }

    header.video.timeCode = tc_bcd;
    if (bigEndian)
      tomsb(&header.video.timeCode, 1);
  }

  std::string edgecode = _edgecode ? _edgecode : "";

  if (edgecode.length() == 0 && metaData.getData(MetaData::EDGECODE)) {
    edgecode = metaData.getString(MetaData::EDGECODE);
  }

  if (edgecode.length()) {
    char edgeCodeNoSpace[16];
    int i = 0;

    const char* edgecodec = edgecode.c_str();

    for (const char* src = edgecodec; *src && i < 16; src++)
      if (!isspace(*src))
        edgeCodeNoSpace[i++] = *src;

    while (i < 16)
      edgeCodeNoSpace[i++] = '0';
    DPXFilmHeader* s = &(header.film);
    memcpy( s->filmManufacturingIdCode, edgeCodeNoSpace, 2 );
    memcpy( s->filmType, edgeCodeNoSpace + 2, 2 );
    memcpy( s->prefix, edgeCodeNoSpace + 4, 6 );
    memcpy( s->count, edgeCodeNoSpace + 10, 4 );
    memcpy( s->perfsOffset, edgeCodeNoSpace + 14, 2 );
  }

  //Now we write out actual image data
  write(&header, sizeof(header));

  unsigned char *pad = new unsigned char[imageBlockPadding];
  memset(pad, 0, imageBlockPadding);
  write(pad, imageBlockPadding);
  delete[] pad;

  ChannelSet mask = channel_mask(num_channels);
  input0().request(0, 0, width(), height(), mask, 1);
  if (aborted())
    return;

  Row row(0, width());
  unsigned off = sizeof(header) + imageBlockPadding;
  int n = num_channels * width();
  ARRAY(U16, src, n + 4);
  src[n] = src[n + 1] = src[n + 2] = src[n + 3] = 0;
  ARRAY(U32, buf, bytes / 4);

  for (int y = height(); y--;) {
    if (aborted())
      return;
    iop->status(float(1.0f - float(y) / height()));
    get(y, 0, width(), mask, row);
    void* p = nullptr;
    if (!datatype) {
      // 8-bit data
      uchar* buf = (uchar*)&(src[0]);
      for (int z = 0; z < num_channels; z++)
        to_byte(z, buf + z, row[channel(z)], row[Chan_Alpha], width(), num_channels);
      p = buf;
    }
    else {
      // 10,12,16 bit data
      for (int z = 0; z < num_channels; z++)
        to_short(z, src + z, row[channel(z)], row[Chan_Alpha], width(), bits[datatype], num_channels);
      switch (datatype) {
        case 1: // 10 bits
          if (fill) {
            for (int x = 0; x < n; x++) {
              unsigned a = (x * 10) / 32;
              unsigned b = (x * 10) % 32;
              if (b > 22) {
                buf[a + 1] = src[x] >> (32 - b);
                buf[a] |= src[x] << b;
              }
              else if (b) {
                buf[a] |= src[x] << b;
              }
              else {
                buf[a] = src[x];
              }
            }
            if (bigEndian)
              tomsb(buf, bytes / 4);
            p = buf;
          }
          else {
            for (int x = 0; x < n; x += 3)
              buf[x / 3] = (src[x] << 22) + (src[x + 1] << 12) + (src[x + 2] << 2);
            if (bigEndian)
              tomsb(buf, bytes / 4);
            p = buf;
          }
          break;
        case 2: // 12 bits
          if (fill) {
            for (int x = 0; x < n; x++) {
              unsigned a = (x * 12) / 32;
              unsigned b = (x * 12) % 32;
              if (b > 20) {
                buf[a + 1] = src[x] >> (32 - b);
                buf[a] |= src[x] << b;
              }
              else if (b) {
                buf[a] |= src[x] << b;
              }
              else {
                buf[a] = src[x];
              }
            }
            if (bigEndian)
              tomsb(buf, bytes / 4);
            p = buf;
          }
          else {
            for (int x = 0; x < n; x++)
              src[x] <<= 4;
            if (bigEndian)
              tomsb(src, n);
            p = src;
          }
          break;
        case 3: // 16 bits
          if (bigEndian)
            tomsb(src, n);
          p = src;
      }
    }
    write(off, p, bytes);
    off += bytes;
  }

  close();
}

static std::string StripCascadingPrefix(const std::string& inStr)
{
  if (inStr.empty()) {
    return inStr;
  }

  size_t prefixEnd = inStr.find_last_of("/");

  if (prefixEnd == std::string::npos) {
    return inStr;
  }

  return inStr.substr(prefixEnd + 1);
}

DD::Image::LUT* DPXWriter::findLutForColorspaceInfo()
{
  static const std::string kDefaultPrefix = "default";
  DD::Image::LUT* lutToUse = lut();

  // Horrible hack: when using OCIO color management, the lut() gets reset to
  // linear in Write::_validate(), which was causing the wrong transfer values
  // to be written. In this case, try to get the name from the colorspace knob,
  // and lookup the LUT from that. This will probably only work when using the
  // nuke-default OCIO config, but that's the case this is intended to fix
  if( lutToUse->linear() ) {
    DD::Image::Knob* knob = iop->knob("colorspace");
    DD::Image::Enumeration_KnobI* enumKnob = knob ? knob->enumerationKnob() : nullptr;
    if( enumKnob ) {
      const std::string text = enumKnob->getSelectedItemString();
      DD::Image::LUT* foundLut = nullptr;
      const bool defaultSelected = text.substr(0, kDefaultPrefix.size()) == kDefaultPrefix;
      if( defaultSelected ) {
        // Knob is set to default, get the default LUT
        foundLut = defaultLUT();
      }
      else {
        // Try to lookup the LUT from the name
        const std::string colorspaceName = StripCascadingPrefix(text);
        foundLut = LUT::GetBuiltinLutByName(colorspaceName.c_str());
      }
      if( foundLut ) {
        lutToUse = foundLut;
      }
    }
  }
  return lutToUse;
}
