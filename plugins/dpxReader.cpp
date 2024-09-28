// dpxReader.C
// Copyright (c) 2009 The Foundry Visionmongers Ltd.  All Rights Reserved.

#ifndef FN_EXAMPLE_PLUGIN
#include "Base/fnThreadLocalStorage.h"
#endif // ndef FN_EXAMPLE_PLUGIN
#include "DDImage/FileReader.h"
#include "DDImage/Row.h"
#include "DDImage/DDMath.h"
#include "DDImage/Knob.h"
#include "DDImage/ARRAY.h"
#include "DDImage/Matrix3.h"
#include "DDImage/Memory.h"
#include "DDImage/MemoryHolder.h"
#include "DDImage/MemHolderFactory.h"
#include "DDImage/LUT.h"
#include "DPXimage.h"

#include <stdio.h>
#include <sstream>
#include <iomanip>
#include <limits>
#include <sys/stat.h>
#include <cmath>

using namespace DD::Image;

const float Kb = .0722f; //.114f;
const float Kr = .2126f; //.299f;

///! Sanitizes the string by replacing any non-printable characters with
///! C-style escape sequences.
inline std::string SanitizeString(const std::string& s)
{
  std::ostringstream oss;
  for (size_t i = 0; i < s.length(); i++) {
    if (isprint(s[i]))
      oss << s[i];
    else
      // double-cast here is intentional to (a) keep character in range 0-255 rather than -128-127 and
      // (b) then to print it to the stream as a number rather than as a char
      oss << "\\x" << std::setw(2) << std::hex << std::setfill('0') << (unsigned int)((unsigned char)s[i]);
  }
  return oss.str();
}

struct Count
{
  unsigned int value;

  Count()
    : value(0)
  {}
};

#ifndef FN_EXAMPLE_PLUGIN
typedef Foundry::Base::ThreadLocalStorage<Count> ThreadLocalCount;
#endif //FN_EXAMPLE_PLUGIN

#define kMaxDPXElements 8

// FileBuffer to hold data from the file for up to kMaxDPXElements DPX elements.
// Derives from MemoryHolder to provide usage information to Nuke's memory manager.
class FileBuffer : public MemoryHolder
{
public:

  static FileBuffer* create(Iop* iopOwner)
  {
    return MemHolderFactory<FileBuffer>::create(iopOwner);
  }

  ~FileBuffer() override
  {
    deleteBuffers();
  }

  void resizeBuffer(int index, size_t requestedSize)
  {
    // Resize the buffer if it is bigger or smaller than the requested size (so we have enough space,
    // but also don't keep hold of more space than we need).
    if (_bufferSize[index] != requestedSize) {

      if (_buffer[index] != nullptr) {
        Memory::deallocate(_buffer[index]);
      }

      _buffer[index] = Memory::allocate<uchar>(requestedSize);
      _bufferSize[index] = requestedSize;
    }
  }

  // Get and set the y-coordinate of the first and last lines stored in the buffer.
  void setYMin(int y) { _yMin = y; }
  int getYMin() const { return _yMin; }
  void setYMax(int y) { _yMax = y; }
  int getYMax() const { return _yMax; }
  void setYRange(int yMin, int yMax)
  {
    _yMin = yMin;
    _yMax = yMax;
  }


  // Get the buffer to use for the given element.
  uchar* getBuffer(int index)
  {
    mFnAssert(index >= 0 && index < kMaxDPXElements);
    return _buffer[index];
  }

  // Get the buffer size for the given element.
  size_t getBufferSize(int index) const
  {
    mFnAssert(index >=0 && index < kMaxDPXElements);
    return _bufferSize[index];
  }

  // Return the total amount of memory allocated across all elements.
  size_t getTotalBufferSize() const
  {
    size_t totalBufferSize = 0;

    for (unsigned int i = 0; i < kMaxDPXElements; i++) {
      totalBufferSize += _bufferSize[i];
    }

    return totalBufferSize;
  }

  // Implementation of memoryFree from MemoryHolder. Frees memory, if the file buffer
  // is not currently being accessed by any thread.
  bool memoryFree(size_t amount) override
  {
    if (getTotalBufferSize() == 0) {
      // There's nothing to free - return immediately.
      return false;
    }

    // If the buffer is not currently in use, we will try to free it.
    if (getUserCount() == 0) {
      // Lock the buffer to flag to any new users that they should not try to use it,
      // but instead read any lines they need directly from the file.
      _locked = true;

      // Check for any new users that sneaked in before we managed to lock.
      if (getUserCount() > 0) {
        // Unlock - we can't free anything as the buffer is now in use.
        _locked = false;
        return false;
      }


      // The buffer's not in use - we can free it.
      deleteBuffers();
      return true;
    }

    // The buffer was in use - we couldn't free anything.
    return false;
  }

  // Implementation of memoryInfo from MemoryHolder
  void memoryInfo(Memory::MemoryInfoArray& output, const void* restrict_to) const override
  {
    // Sum up the memory in all our buffers.
    unsigned int totalBytes = 0;
    for (unsigned int i = 0; i < kMaxDPXElements; i++) {
      totalBytes += _bufferSize[i];
    }

    output.push_back(Memory::MemoryInfo(_iopOwner, totalBytes));
  }

  // Implementation of memoryWeight from MemoryHolder
  int memoryWeight() const override
  {
    // getUserCount() provides a snapshot of whether any threads are currently accessing the
    // buffer. If it returns 0, it doesn't mean a thread might not be just about to access
    // the buffer, but we can't know one way or the other. Our best guess if that if the
    // buffer is currently in use, it would not be efficient to free it. If it's not in use,
    // we will always allow it to be freed, and any thread which was about to access it will
    // be forced to fall back to reading from the file instead. (This will still work, it
    // just might be slower.)
    if (getUserCount() > 0) {
      // The buffer is in use - prefer not to free it.
      return 1000;
    }
    else {
      // Nothing's using the buffer - it's probably OK to free it.
      return 0;
    }
  }

  // Increment the user count for the current thread.
  void addUser()
  {
#ifndef FN_EXAMPLE_PLUGIN
    _bufferUsers.local().value++;
#endif // FN_EXAMPLE_PLUGIN
  }

  // Decrement the user count for the current thread.
  void removeUser()
  {
#ifndef FN_EXAMPLE_PLUGIN
    _bufferUsers.local().value--;
#endif // FN_EXAMPLE_PLUGIN
  }

  // Get the total user count over all threads which have accessed the file buffer.
  unsigned int getUserCount() const
  {
    unsigned int totalUsers = 0;
#ifndef FN_EXAMPLE_PLUGIN
    for (ThreadLocalCount::iterator it = _bufferUsers.begin(); it != _bufferUsers.end(); ++it) {
      totalUsers += it->value;
    }
#endif // FN_EXAMPLE_PLUGIN

    return totalUsers;
  }

  // If the buffer is locked, you should not attempt to use it.
  bool locked() { return _locked; }


protected:
  // Don't call this directly; use FileBuffer::create() instead, to register
  // the cache with Nuke's memory manager.
  FileBuffer(Iop* iopOwner)
    : _iopOwner(iopOwner)
    , _locked(false)
  {
    // Initialise file buffers
    for (unsigned int i = 0; i < kMaxDPXElements; i++) {
      _buffer[i] = nullptr;
      _bufferSize[i] = 0;
    }
  }

  // Clear buffers and return whether or not any buffers were in use (used by memoryFree).
  bool deleteBuffers()
  {
    bool wasCleared = false;
    for (unsigned int i = 0; i < kMaxDPXElements; i++) {
      wasCleared |= clearBuffer(i);
    }
    return wasCleared;
  }

  // Clear buffer and return whether or not the buffer was in use (used by memoryFree).
  bool clearBuffer(int index)
  {
    bool wasCleared = false;
    if (_buffer[index] != nullptr) {
      Memory::deallocate(_buffer[index]);
      _buffer[index] = nullptr;
      wasCleared = true;
    }
    _bufferSize[index] = 0;
    return wasCleared;
  }

private:
  uchar* _buffer[kMaxDPXElements];  //!< Internal file buffer to hold up to kMaxDPXElements DPX elements.
  size_t _bufferSize[kMaxDPXElements];  //!< The current size of the file buffer for each of the kMaxDPXElements DPX elements.

  int _yMin;  //!< The y-coordinate of the first line stored in the buffer.
  int _yMax;  //!< The y-coordinate of the last line stored in the buffer.

  Iop* _iopOwner; //!< The Iop that owns the thing that created this file buffer. For memory-tracking purposes.

#ifndef FN_EXAMPLE_PLUGIN
  mutable ThreadLocalCount _bufferUsers;  //!< Thread-local counter to keep track of whether or not the buffer is in use.
#endif //FN_EXAMPLE_PLUGIN

  bool _locked;  //!< Lock the buffer.
};

// Scoped file buffer guard. Flag that we are using the buffer on construction,
// and remove the flag on destruction.
class FileBufferGuard
{
public:
  FileBufferGuard(FileBuffer* buffer)
    : _buffer(buffer)
  {
    _buffer->addUser();
  }

  ~FileBufferGuard()
  {
    _buffer->removeUser();
  }

private:

  FileBuffer* _buffer;

};


class dpxReader : public FileReader
{

  // this is the parts of the header we keep:
  bool _flipEndian;
  bool ycbcr_hack;
  unsigned orientation;
  unsigned width;
  unsigned height;
  struct Element
  {
    U8 descriptor;
    U8 transfer;    //!< The transfer characteristics used to transform the data when writing the file.
    U8 bits;
    bool is_nl_matte; // Set if channel contains a Northlight matte
    U16 packing;
    U32 dataOffset;
    U32 bytes; // bytes per line
    int components; // descriptor decoded into # of components
    ChannelSet channels; // which Nuke channels it supplies
  }
  element[kMaxDPXElements];

  int _fileSize; //!< The size of the dpx file.
  int _numElements; //!< The number of elements with data.

  static const Description description;

  MetaData::Bundle meta;

  // Storage for whether or not we should read all lines at once.
  bool _readAllLines;
  // Internal file buffer controls. This buffer is used to store the entire contents of the file when
  // reading full frames (where possible) has been requested on the command line.
  FileBuffer* _requestedLinesPreloadBuffer;

  bool testFileBuffer() const
  {
    // Test returns true if we have a file buffer and it is not locked.
    return (_requestedLinesPreloadBuffer != nullptr && !_requestedLinesPreloadBuffer->locked());
  }

  // Whether we need to invert y when reading from the file.
  bool invertY() const { return !(orientation & 2); }

  void readAllLines()
  {
    // If we have a file buffer, and it hasn't been locked due to low memory, allocate
    // space for reading the requested lines into.
    if (testFileBuffer()) {
      ChannelSet remaining(info_.channels());
      if (ycbcr_hack && (info_.channels() & Mask_RGB))
        remaining += Mask_RGB;

      // Determine the first and last lines we need to read from the file.
      const Box& requestBox = iop->requestedBox();
      const int yMin = invertY() ? height - requestBox.t() : requestBox.y();
      const int yMax = invertY() ? height - requestBox.y() - 1 : requestBox.t();

      // Guard to prevent memory being freed while we are trying to allocate it.
      FileBufferGuard guard(_requestedLinesPreloadBuffer);

      // Test again to make sure no memory is being freed.
      if (!_requestedLinesPreloadBuffer->locked()) {
        for (unsigned i = 0; i < kMaxDPXElements; i++) {
          if (element[i].channels & remaining) {

            // Calculate the buffer size required to read all requested lines for
            // the current element from the file.
            const unsigned int nLines = yMax - yMin + 1;
            const size_t bufferSize = element[i].bytes * nLines;

            // This will reallocate the buffer if the size has changed.
            _requestedLinesPreloadBuffer->resizeBuffer(i, bufferSize);
            _requestedLinesPreloadBuffer->setYRange(yMin, yMax);

            // Read all requested lines from the file into our internal buffer.
            read((void *)_requestedLinesPreloadBuffer->getBuffer(i), element[i].dataOffset + yMin * element[i].bytes, static_cast<unsigned int>(bufferSize));

            remaining -= element[i].channels;
            if (!remaining)
              break;
          }
        }
      }
    }
  }

public:

  const MetaData::Bundle& fetchMetaData(const char* key) override
  {
    return meta;
  }

  dpxReader(Read* iop, int fd, const unsigned char* block, int len)
    : FileReader(iop, fd, block, len)
  {
    _readAllLines = readAllLinesRequested();
    if (_readAllLines) {
      // Use an internal file buffer to cache the requested lines from the file
      _requestedLinesPreloadBuffer = FileBuffer::create(iop);
    }
    else {
      _requestedLinesPreloadBuffer = nullptr;
    }

    // #rick: Maybe this sort of thing should by moved to FileReader?

    // Find out how large the whole file is for use by planarReadPass().
    struct stat statBuffer;
    fstat(fd, &statBuffer);
    _fileSize = statBuffer.st_size;

    DPXHeader header;

    // Copy the header chunk raw:
    read(&header, 0, int(sizeof(header)));

    // Put the header into native-endianess:
    if (header.file.magicNumber != DPX_MAGIC) {
      _flipEndian = true;
      flip(&header.file.magicNumber, 2); // magicNumber & offsetToImageData
      flip(&header.file.totalFileSize, 5); // totalFileSize thru userDataSize
      flip(&header.image.orientation, 2); // orientation & numberElements
      flip(&header.image.pixelsPerLine, 2); // pixelsPerLine & linesPerImage
      for (int i = 0; i < header.image.numberElements; i++) {
        flip(&header.image.element[i].dataSign, 5); // dataSign, low/high stuff
        flip(&header.image.element[i].packing, 2); // packing & encoding
        flip(&header.image.element[i].dataOffset, 3); // dataOffset, eol/imagePadding
      }
      flip((U32*)(&header.film.frameRate), 1);
      flip((U32*)(&header.film.framePosition), 1);
      flip((U32*)(&header.film.sequenceLen), 1);
      flip((U32*)(&header.film.heldCount), 1);
      flip((U32*)(&header.film.shutterAngle), 1);
      flip((U32*)(&header.film.frameId), 1);
      flip((U32*)(&header.video.horzSampleRate), 10);
      flip(&header.video.timeCode, 2);
      flip((U32*)(&header.orientation.pixelAspect), 2);
    }
    else {
      _flipEndian = false;
    }

    width = header.image.pixelsPerLine;
    height = header.image.linesPerImage;
    _numElements = header.image.numberElements;

    // Figure out the pixel aspect ratio. We recognize two possible
    // 'invalid' values -- all 0s or all 1s.
    // Equality is also invalid because Shake writes that for all images
    // Another bug version writes the image size as the pixel aspect,
    // ignore that.
    // Note that in set_info, pixel aspect ratio will eventually be defaulted
    // to 1 or 2 depending on -a (anamorphic) Nuke option.
    // Note that equality is removed from invalid cases to support square pixels.
    // Based on a line above, if any customers are using Shake written anamorphic png
    // files and are relying on pre created format with correct aspect ratio, that format
    // won't be picked up now. The solution would be to set the format after loading. 
    // This probably is a very unlikely case. 
    double pixel_aspect = 0;
    if ( header.orientation.pixelAspect[0] != 0 &&
         header.orientation.pixelAspect[1] != 0 &&
         header.orientation.pixelAspect[0] != 0xffffffff &&
         header.orientation.pixelAspect[1] != 0xffffffff &&
         (header.orientation.pixelAspect[0] != width ||
          header.orientation.pixelAspect[1] != height)) {
      pixel_aspect = (double)header.orientation.pixelAspect[0] /
                     (double)header.orientation.pixelAspect[1];
    }

    // Set the image size. We will figure out the channels later from the elements, use rgb here:
    set_info(width, height, 3, pixel_aspect);

#define DUMP_HEADER 0
#if DUMP_HEADER
    printf("%s:", header.file.imageFileName);
    if (_flipEndian)
      printf(" flipEndian");
    printf("\n");
    // printf(" size=%dx%dx, ", header.image.pixelsPerLine,
    //                         header.image.linesPerImage);
    // printf(" orientation=%x\n", header.image.orientation);
#endif

    int bitdepth = 0;
    for (int i = 0; i < header.image.numberElements; i++) {
      if (header.image.element[i].bits > bitdepth)
        bitdepth = header.image.element[i].bits;
    }

    ycbcr_hack = false;
    info_.channels(Mask_None);
    for (int i = 0; i < header.image.numberElements; i++) {
#if DUMP_HEADER
      printf(" %d: ", i);
      //    printf(" lowData=%d\n", header.image.element[i].lowData);
      //    printf(" lowQuantity=%f\n", header.image.element[i].lowQuantity);
      //    printf(" highData=%d\n", header.image.element[i].highData);
      //    printf(" highQuantity=%f\n", header.image.element[i].highQuantity);
      printf("d %d, ", header.image.element[i].descriptor);
      printf("t %d, ", header.image.element[i].transfer);
      printf("c %d, ", header.image.element[i].colorimetric);
      printf("%d bits, ", header.image.element[i].bits);
      if (header.image.element[i].dataSign)
        printf("signed, ");
      if (header.image.element[i].packing)
        printf("filled=%d, ", header.image.element[i].packing);
      if (header.image.element[i].encoding)
        printf("rle=%d, ", header.image.element[i].encoding);
      //    printf(" dataOffset=%d\n", header.image.element[i].dataOffset);
      if (header.image.element[i].eolPadding)
        printf("eolPadding=%d, ", header.image.element[i].eolPadding);
      //    printf(" eoImagePadding=%d\n", header.image.element[i].eoImagePadding);
      printf("\"%s\"\n", header.image.element[i].description);
#endif
      element[i].descriptor = header.image.element[i].descriptor;
      switch (element[i].descriptor) {
        case DESCRIPTOR_R:
          element[i].channels = Mask_Red;
          element[i].components = 1;
          break;
        case DESCRIPTOR_G:
          element[i].channels = Mask_Green;
          element[i].components = 1;
          break;
        case DESCRIPTOR_B:
          element[i].channels = Mask_Blue;
          element[i].components = 1;
          break;
        case DESCRIPTOR_A:
          element[i].channels = Mask_Alpha;
          element[i].components = 1;
          break;
        default:
          printf("Unknown DPX element descriptor %d\n", element[i].descriptor);
        case DESCRIPTOR_Y:
          element[i].channels = Mask_RGB;
          element[i].components = 1;
          break;
        case DESCRIPTOR_CbCr:
          element[i].channels = ChannelSetInit(6); // blue+green
          element[i].components = 1;
          if (i && element[0].descriptor == DESCRIPTOR_Y) {
            element[0].channels = Mask_Red;
            ycbcr_hack = true;
          }
          break;
        case DESCRIPTOR_Z:
          element[i].channels = Mask_Z;
          element[i].components = 1;
          break;
        case DESCRIPTOR_RGB:
          element[i].channels = Mask_RGB;
          element[i].components = 3;
          break;
        case DESCRIPTOR_RGBA:
          element[i].channels = Mask_RGBA;
          element[i].components = 4;
          break;
        case DESCRIPTOR_ABGR:
          element[i].channels = Mask_RGBA;
          element[i].components = 4;
          break;
        case DESCRIPTOR_CbYCrY:
          element[i].channels = Mask_RGB;
          element[i].components = 2;
          break;
        case DESCRIPTOR_CbYACrYA:
          element[i].channels = Mask_RGBA;
          element[i].components = 3;
          break;
        case DESCRIPTOR_CbYCr:
          element[i].channels = Mask_RGB;
          element[i].components = 3;
          break;
        case DESCRIPTOR_CbYCrA:
          element[i].channels = Mask_RGBA;
          element[i].components = 4;
          break;
        case DESCRIPTOR_USER_2:
          element[i].channels = ChannelSetInit(3); // red+green
          element[i].components = 2;
          break;
        case DESCRIPTOR_USER_3:
          element[i].channels = Mask_RGB;
          element[i].components = 3;
          break;
        case DESCRIPTOR_USER_4:
          element[i].channels = Mask_RGBA;
          element[i].components = 4;
          break;
        case DESCRIPTOR_USER_5:
          element[i].channels = Mask_RGBA;
          element[i].components = 5;
          break;
        case DESCRIPTOR_USER_6:
          element[i].channels = Mask_RGBA;
          element[i].components = 6;
          break;
        case DESCRIPTOR_USER_7:
          element[i].channels = Mask_RGBA;
          element[i].components = 7;
          break;
        case DESCRIPTOR_USER_8:
          element[i].channels = Mask_RGBA;
          element[i].components = 8;
          break;
      }

      element[i].bits = header.image.element[i].bits;
      element[i].packing = header.image.element[i].packing;
      element[i].is_nl_matte = false;
      //element[i].encoding = header.image.element[i].encoding;
      element[i].dataOffset = header.image.element[i].dataOffset;
      element[i].transfer = header.image.element[i].transfer;

      switch (element[i].bits) {
        case 1:
          // Northlight mattes use 8-bit words
          if(!strcmp(header.image.element[i].description, "NL CLEAN MATTE")) {
            element[i].bytes = (width * element[i].components + 7) / 8;
            element[i].is_nl_matte = true;
          } else
            element[i].bytes = (width * element[i].components + 31) / 32 * 4;
          break;
        case 8:
          element[i].bytes = (width * element[i].components + 3) & - 4;
          break;
        case 10:
          if (element[i].packing) {
            element[i].bytes = (width * element[i].components + 2) / 3 * 4;
            // detect stupid writers that did the math wrong
            struct stat s;
            fstat(fd, &s);
            if (element[i].dataOffset + element[i].bytes * height > size_t(s.st_size))
              element[i].bytes = (width * element[i].components) / 3 * 4;
          }
          else
            element[i].bytes = (width * element[i].components * 10 + 31) / 32 * 4;
          break;
        case 12:
          if (element[i].packing)
            element[i].bytes = (width * element[i].components) * 2;
          else
            element[i].bytes = (width * element[i].components * 12 + 31) / 32 * 4;
          break;
        case 16:
          element[i].bytes = (width * element[i].components) * 2;
          break;
        case 32: // no sample files available for this
        case 64: // no sample files available for this
        default:
          printf("Unhandled DPX number of bits %d\n", element[i].bits);
          element[i].channels = Mask_None;
          break;
      }

      meta.setDataIfNotEmpty(MetaData::ELEMENT_DESCRIPTION[i], header.image.element[i].description);

      if (header.image.element[i].eolPadding != 0xffffffff) {

        // Add the end-of-line padding value to the number of bytes per line.
        // Note: Some exporters write garbage to this, so only add the padding if won't cause the image
        // data to overlap the next element or go past the end of the file if this is the last element. We
        // could still have a small garbage offset that isn't detected here but at least it won't cause
        // the reader to crash.

        size_t maxAllowedEndOfImage = ((i + 1) < header.image.numberElements) ? header.image.element[i + 1].dataOffset: static_cast<size_t>(_fileSize);

        size_t imageSizeWithEolPadding = (element[i].bytes + header.image.element[i].eolPadding) * height;
        size_t endOfImageWithEolPadding = element[i].dataOffset + imageSizeWithEolPadding;

        if (endOfImageWithEolPadding <= maxAllowedEndOfImage)
          element[i].bytes += header.image.element[i].eolPadding;
      }

      info_.turn_on(element[i].channels);
    }

    std::string edgecode;

    DPXFilmHeader* s = &(header.film);
    if ( s->filmManufacturingIdCode[0] &&
         s->filmType[0] &&
         s->perfsOffset[0] &&
         s->prefix[0] &&
         s->count[0] ) {
      char buffer[22];
      sprintf( buffer, "%c%c %c%c %c%c%c%c%c%c %c%c%c%c %c%c",
               s->filmManufacturingIdCode[0],
               s->filmManufacturingIdCode[1],
               s->filmType[0], s->filmType[1],
               s->prefix[0], s->prefix[1],
               s->prefix[2], s->prefix[3], s->prefix[4], s->prefix[5],
               s->count[0], s->count[1], s->count[2], s->count[3],
               s->perfsOffset[0], s->perfsOffset[1] );
      edgecode = buffer;

      sprintf( buffer, "%c%c %c%c %c%c %c%c%c%c %c%c%c%c %c%c",
               s->filmManufacturingIdCode[0],
               s->filmManufacturingIdCode[1],
               s->filmType[0], s->filmType[1],
               s->prefix[0], s->prefix[1],
               s->prefix[2], s->prefix[3], s->prefix[4], s->prefix[5],
               s->count[0], s->count[1], s->count[2], s->count[3],
               s->perfsOffset[0], s->perfsOffset[1] );
      meta.setData( MetaData::EDGECODE, SanitizeString(buffer) );
    }

    orientation = header.image.orientation;
    info_.ydirection((orientation & 2) ? 1 : -1);

    switch (header.image.element[0].transfer) {
      case TRANSFER_USER: // seems to be used by some log files
      case TRANSFER_DENSITY:
      case TRANSFER_LOG:
        lut_ = LUT::GetLut(LUT::LOG, this);
        meta.setData(MetaData::DPX::TRANSFER, "log");
        break;
      case TRANSFER_CCIR_709_1:
        lut_ = LUT::Builtin("rec709", this);
        meta.setData(MetaData::DPX::TRANSFER, "rec709");
        break;
      case TRANSFER_LINEAR: // unfortunatly too much software writes this for sRGB...
      default:
        lut_ = LUT::GetLut(header.image.element[0].bits <= 8 ? LUT::INT8 : LUT::INT16, this);
        meta.setData(MetaData::DPX::TRANSFER, "sRGB");
        break;
    }

    // XXXX
    meta.setDataIfNotEmpty(MetaData::COPYRIGHT, header.file.copyright);

    meta.setDataIfNotEmpty(MetaData::DPX::FILE_NAME, header.orientation.fileName);
    meta.setDataIfNotEmpty(MetaData::DPX::CREATION_TIME, header.orientation.creationTime);
    meta.setDataIfNotEmpty(MetaData::DPX::INPUT_DEVICE, header.orientation.inputName);
    meta.setDataIfNotEmpty(MetaData::DPX::INPUT_SN, header.orientation.inputSN);

    const R32 filmFrameRate = header.film.frameRate;
    const U32 timecode = header.video.timeCode;

    if(DPXValid(timecode)) {
      // The timecode is encoded as 0xHHMMSSFF where the drop frame flag is also encoded into the FF part as 0x40 if enabled.

      char buffer[12];
      U32 frames = timecode & 0xFF;
      U32 flags = 0x0;

      // We only support reading drop frames for 29.97fps footage.
      const bool supportsDropFrame = DPXImage::isDropFrameSupported(filmFrameRate);

      // Note on 59.94fps drop frame footage: Because of the way in which the drop frame flag is encoded into the 0x40 bit of the
      // frame number, this creates a problem as the frame number itself also requires the 0x40 bit for frames above 40....
      // i.e. the dropframe timecode for frame 11 and the non-dropframe timecode for frame 51 are both 0x51.
      // For this reason we can't tell if a 59.94fps dpx is drop frame or not just from the header timecode.

      // Only 29.97
      if (supportsDropFrame) {
        frames = timecode & 0x3F;
        flags = timecode & 0xC0;
      }
      const bool isDropFrame = 0 != (flags & DPXImage::kDropFrameFlag);

      // Use ';' for dropframe, else ':'
      const char* timecodeFormat = isDropFrame ? "%02x;%02x;%02x;%02x" : "%02x:%02x:%02x:%02x";
      sprintf(buffer, timecodeFormat, timecode >> 24, (timecode >> 16) & 255,
              (timecode >> 8) & 255, frames);
      const std::string timecodeStr = buffer;

      meta.setData(MetaData::TIMECODE, timecodeStr);

      // Television
      if (timecode != 0) {
        meta.setData(MetaData::DPX::TIME_CODE, timecode);
      }
    }

    if (pixel_aspect != 0) {
      meta.setData(MetaData::PIXEL_ASPECT, pixel_aspect);
    }

    meta.setData(MetaData::DEPTH, MetaData::DEPTH_FIXED(bitdepth));

    if ((header.video.userBits != 0) && (std::isfinite(static_cast<double>(header.video.userBits)))) {
      meta.setData(MetaData::DPX::USER_BITS, header.video.userBits);
    }

    if ((header.video.interlace != 0) && (std::isfinite(static_cast<double>(header.video.interlace)))) {
      meta.setData(MetaData::DPX::INTERLACE, header.video.interlace);
    }

    if ((header.video.fieldNumber != 0) && (std::isfinite(static_cast<double>(header.video.fieldNumber)))) {
      meta.setData(MetaData::DPX::FIELD_NUMBER, header.video.fieldNumber);
    }

    if ((header.video.videoSignal != 0) && (std::isfinite(static_cast<double>(header.video.videoSignal)))) {
      meta.setData(MetaData::DPX::VIDEO_SIGNAL, header.video.videoSignal);
    }

    if ((header.video.horzSampleRate != 0) && (std::isfinite(static_cast<double>(header.video.horzSampleRate)))) {
      meta.setData(MetaData::DPX::HORIZ_SAMPLE, header.video.horzSampleRate);
    }

    if ((header.video.vertSampleRate != 0) && (std::isfinite(static_cast<double>(header.video.vertSampleRate)))) {
      meta.setData(MetaData::DPX::VERT_SAMPLE, header.video.vertSampleRate);
    }

    if ((filmFrameRate > 0.f) && (std::isfinite(static_cast<double>(filmFrameRate)))) {
      meta.setData(MetaData::FRAME_RATE, filmFrameRate);
    }

    if ((header.video.frameRate > 0.f) && (std::isfinite(static_cast<double>(header.video.frameRate)))) {
      meta.setData(MetaData::DPX::FRAME_RATE, header.video.frameRate);
    }

    if ((header.video.timeOffset != 0) && (std::isfinite(static_cast<double>(header.video.timeOffset)))) {
      meta.setData(MetaData::DPX::TIME_OFFSET, header.video.timeOffset);
    }

    if ((header.video.gamma != 0) && (std::isfinite(static_cast<double>(header.video.gamma)))) {
      meta.setData(MetaData::DPX::GAMMA, header.video.gamma);
    }

    if ((header.video.blackLevel != 0) && (std::isfinite(static_cast<double>(header.video.blackLevel)))) {
      meta.setData(MetaData::DPX::BLACK_LEVEL, header.video.blackLevel);
    }

    if ((header.video.blackGain != 0) && (std::isfinite(static_cast<double>(header.video.blackGain)))) {
      meta.setData(MetaData::DPX::BLACK_GAIN, header.video.blackGain);
    }

    if ((header.video.breakpoint != 0) && (std::isfinite(static_cast<double>(header.video.breakpoint)))) {
      meta.setData(MetaData::DPX::BREAK_POINT, header.video.breakpoint);
    }

    if ((header.video.whiteLevel != 0) && (std::isfinite(static_cast<double>(header.video.whiteLevel)))) {
      meta.setData(MetaData::DPX::WHITE_LEVEL, header.video.whiteLevel);
    }

    if ((header.video.integrationTimes != 0) && (std::isfinite(static_cast<double>(header.video.integrationTimes)))) {
      meta.setData(MetaData::DPX::INTEGRATION_TIMES, header.video.integrationTimes);
    }

    //    if  (header.film.framePosition != UNDEF_R32) {
    meta.setData(MetaData::DPX::FRAMEPOS,        header.film.framePosition);
    //    }

    if  (header.film.sequenceLen != UNDEF_U32) {
      meta.setData(MetaData::DPX::SEQUENCE_LENGTH, header.film.sequenceLen);
    }

    if  (header.film.heldCount != UNDEF_U32) {
      meta.setData(MetaData::DPX::HELD_COUNT, header.film.heldCount);
    }

    if  (header.film.shutterAngle != UNDEF_R32) {
      meta.setData(MetaData::SHUTTER_ANGLE, header.film.shutterAngle);
    }

    meta.setDataIfNotEmpty(MetaData::DPX::FRAME_ID, header.film.frameId);
    meta.setDataIfNotEmpty(MetaData::SLATE_INFO, header.film.slateInfo);

    if (edgecode.length() && edgecode != "00 00 000000 0000 00") {
      meta.setData(MetaData::EDGECODE, edgecode);
    }

    meta.setTimeStamp(MetaData::FILE_CREATION_TIME, header.file.creationTime);
    meta.setDataIfNotEmpty(MetaData::CREATOR, header.file.creator);
    meta.setDataIfNotEmpty(MetaData::PROJECT, header.file.project);
    meta.setDataIfNotEmpty(MetaData::COPYRIGHT, header.file.copyright);
    meta.setDataIfNotEmpty(MetaData::FILENAME, header.file.imageFileName);
  }

  ~dpxReader() override
  {
    delete _requestedLinesPreloadBuffer;
  }

  void CConvert(float* dest, const uchar* src, int x, int r, int delta)
  {
    const float m = 1.0f / 255;
    const float off = .5f - 0x80 * m;
    for (; x < r; x++)
      dest[x] = src[x * delta] * m + off;
  }

  void CConvert(float* dest, const U16* src, int x, int r, int delta, int bits)
  {
    const float m = 1.0f / ((1 << bits) - 1);
    const float off = .5f - (1 << (bits - 1)) * m;
    for (; x < r; x++)
      dest[x] = src[x * delta] * m + off;
  }

  void CbConvert(float* dest, const uchar* src, int x, int r, int delta)
  {
    const float m = 1.0f / 255;
    const float m2 = m / 2;
    const float off = .5f - 0x80 * m;
    if (!(r & 1) && r >= int(width)) {
      dest[r - 1] = src[(r - 2) * delta] * m + off;
      r--;
    }
    for (; x < r; x++) {
      if (x & 1)
        dest[x] = (src[(x - 1) * delta] + src[(x + 1) * delta]) * m2 + off;
      else
        dest[x] = src[x * delta] * m + off;
    }
  }

  void CbConvert(float* dest, const U16* src, int x, int r, int delta, int bits)
  {
    const float m = 1.0f / ((1 << bits) - 1);
    const float m2 = m / 2;
    const float off = .5f - (1 << (bits - 1)) * m;
    if (!(r & 1) && r >= int(width)) {
      dest[r - 1] = src[(r - 2) * delta] * m + off;
      r--;
    }
    for (; x < r; x++) {
      if (x & 1)
        dest[x] = (src[(x - 1) * delta] + src[(x + 1) * delta]) * m2 + off;
      else
        dest[x] = src[x * delta] * m + off;
    }
  }

  void CrConvert(float* dest, const uchar* src, int x, int r, int delta)
  {
    const float m = 1.0f / 255;
    const float m2 = m / 2;
    const float off = .5f - 0x80 * m;
    if (!x) {
      dest[x] = src[(x + 1) * delta] * m + off;
      x++;
    }
    for (; x < r; x++) {
      if (x & 1)
        dest[x] = src[x * delta] * m + off;
      else
        dest[x] = (src[(x - 1) * delta] + src[(x + 1) * delta]) * m2 + off;
    }
  }

  void CrConvert(float* dest, const U16* src, int x, int r, int delta, int bits)
  {
    const float m = 1.0f / ((1 << bits) - 1);
    const float m2 = m / 2;
    const float off = .5f - (1 << (bits - 1)) * m;
    if (!x) {
      dest[x] = src[(x + 1) * delta] * m + off;
      x++;
    }
    for (; x < r; x++) {
      if (x & 1)
        dest[x] = src[x * delta] * m + off;
      else
        dest[x] = (src[(x - 1) * delta] + src[(x + 1) * delta]) * m2 + off;
    }
  }

  void YConvert(float* dest, const uchar* src, int x, int r, int delta)
  {
    Linear::from_byte(dest + x, src + x * delta, r - x, delta);
  }

  void YConvert(float* dest, const U16* src, int x, int r, int delta, int bits)
  {
    Linear::from_short(dest + x, src + x * delta, r - x, bits, delta);
  }

  void AConvert(float* dest, const uchar* src, int x, int r, int delta)
  {
    Linear::from_byte(dest + x, src + x * delta, r - x, delta);
  }

  void AConvert(float* dest, const U16* src, int x, int r, int delta, int bits)
  {
    Linear::from_short(dest + x, src + x * delta, r - x, bits, delta);
  }

  void fixYCbCr(int x, int r, bool alpha, Row& row)
  {
    if (iop->raw())
      return;
    float* R = row.writable(Chan_Red);
    float* G = row.writable(Chan_Green);
    float* B = row.writable(Chan_Blue);
    for (int X = x; X < r; X++) {
      float y = (R[X] - float(16 / 255.0f)) * float(255.0f / 219);
      float u = (G[X] - .5f) * float(255.0f / 224);
      float v = (B[X] - .5f) * float(255.0f / 224);
      R[X] = v * (2 - 2 * Kr) + y;
      G[X] = y - v * ((2 - 2 * Kr) * Kr / (1 - Kr - Kb)) - u * ((2 - 2 * Kb) * Kb / (1 - Kr - Kb));
      B[X] = u * (2 - 2 * Kb) + y;
    }
    from_float(Chan_Red, R + x, R + x, alpha ? row[Chan_Alpha] + x : nullptr, r - x);
    from_float(Chan_Green, G + x, G + x, alpha ? row[Chan_Alpha] + x : nullptr, r - x);
    from_float(Chan_Blue, B + x, B + x, alpha ? row[Chan_Alpha] + x : nullptr, r - x);
  }

  // Read a line from the file, or from our internal buffer for the file, if the latter exists
  // and is unlocked.
  void get_line_from_file(void* destination,
                          unsigned int elementIndex,
                          unsigned int dataOffsetInFile,
                          unsigned int lineSize,
                          int y,
                          size_t bytesToRead)
  {
    // If we have an internal file buffer, try to read the line from it.
    if (testFileBuffer()) {
      // Make sure the internal file buffer won't be freed while we are using it.
      FileBufferGuard guard(_requestedLinesPreloadBuffer);

      // Check again that nothing is trying to free memory.
      if (!_requestedLinesPreloadBuffer->locked()) {

        mFnAssertMsg(y >= _requestedLinesPreloadBuffer->getYMin() && y <= _requestedLinesPreloadBuffer->getYMax(),
                     "dpxReader: out-of-bounds access to internal file buffer.");

        // Read the line from our internal frame buffer.
        const int lineOffsetInBuffer= y - _requestedLinesPreloadBuffer->getYMin();
        memcpy(destination, _requestedLinesPreloadBuffer->getBuffer(elementIndex) + lineOffsetInBuffer * lineSize, bytesToRead);
        return;
      }
    }

    // Read the line from the file.
    read(destination, dataOffsetInFile + y * lineSize, static_cast<unsigned int>(bytesToRead));
  }

  void read_element8(const Element& e, unsigned int index, int y, int x, int r, Row& row)
  {
    // uncompress into an array of bytes:
    ARRAY(uchar, buf, width * e.components);

    if (e.bits == 1) {
      if (e.is_nl_matte) {
        ARRAY(U8, src, e.bytes);
        get_line_from_file(src, index, e.dataOffset, e.bytes, y, e.bytes);
        for (unsigned x = 0; x < width * e.components; x++)
          buf[x] = (src[x / 8] & (1 << (x & 7))) ? 255 : 0;
      }
      else {
        unsigned n = (e.bytes + 3) / 4;
        ARRAY(U32, src, n);
        get_line_from_file(src, index, e.dataOffset, e.bytes, y, e.bytes);
        if (_flipEndian)
          flip(src, n);
        for (unsigned x = 0; x < width * e.components; x++)
          buf[x] = (src[x / 32] & (1 << (x & 31))) ? 255 : 0;
      }
    }
    else {
      get_line_from_file(buf, index, e.dataOffset, e.bytes, y, width * e.components);
    }
    // now convert to rgb
    switch (e.descriptor) {

      case DESCRIPTOR_CbCr:
        // to actually get rgb we need the Y from the other element. NYI
        CbConvert(row.writable(Chan_Green), buf, x, r, 1);
        CrConvert(row.writable(Chan_Blue), buf, x, r, 1);
        if (ycbcr_hack)
          fixYCbCr(x, r, false, row);
        break;
      case DESCRIPTOR_RGBA: {
        const uchar* alpha = buf + x * 4 + 3;
        foreach(z, e.channels)
        from_byte(z, row.writable(z) + x, buf + x * 4 + (z - 1), alpha, r - x, 4);
        break;
      }
      case DESCRIPTOR_ABGR: {
        const uchar* alpha = buf + x * 4;
        foreach(z, e.channels)
        from_byte(z, row.writable(z) + x, buf + x * 4 + (4 - z), alpha, r - x, 4);
        break;
      }
      case DESCRIPTOR_CbYCrY:
        YConvert(row.writable(Chan_Red), buf + 1, x, r, 2);
        CbConvert(row.writable(Chan_Green), buf, x, r, 2);
        CrConvert(row.writable(Chan_Blue), buf, x, r, 2);
        fixYCbCr(x, r, false, row);
        break;
      case DESCRIPTOR_CbYACrYA:
        CbConvert(row.writable(Chan_Green), buf, x, r, 3);
        CrConvert(row.writable(Chan_Blue), buf, x, r, 3);
        YConvert(row.writable(Chan_Red), buf + 1, x, r, 3);
        AConvert(row.writable(Chan_Alpha), buf + 2, x, r, 3);
        fixYCbCr(x, r, true, row);
        break;
      case DESCRIPTOR_CbYCr:
        CConvert(row.writable(Chan_Green), buf, x, r, 3);
        YConvert(row.writable(Chan_Red), buf + 1, x, r, 3);
        CConvert(row.writable(Chan_Blue), buf + 2, x, r, 3);
        fixYCbCr(x, r, false, row);
        break;
      case DESCRIPTOR_CbYCrA:
        CConvert(row.writable(Chan_Green), buf, x, r, 4);
        YConvert(row.writable(Chan_Red), buf + 1, x, r, 4);
        CConvert(row.writable(Chan_Blue), buf + 2, x, r, 4);
        AConvert(row.writable(Chan_Alpha), buf + 3, x, r, 4);
        fixYCbCr(x, r, true, row);
        break;
      case DESCRIPTOR_Y:
        if (ycbcr_hack) {
          YConvert(row.writable(Chan_Red), buf, x, r, 1);
          break;
        } // else fall through:
      default: {
        int Z = 0;
        foreach(z, e.channels) {
          from_byte(z, row.writable(z) + x, buf + Z + x * e.components, nullptr /*alpha*/, r - x, e.components);
          if (Z + 1 < e.components)
            Z++;
        }
        break;
      }
    }
  }

  void read_element16(const Element& e, unsigned int index, int y, int x, int r, Row& row)
  {
    ARRAY(U16, buf, width * e.components + 2);

    switch (e.bits) {
      case 10: {
        unsigned n = (e.bytes + 3) / 4;
        unsigned x;
        ARRAY(U32, src, n);
        get_line_from_file(src, index, e.dataOffset, e.bytes, y, e.bytes);
        if (_flipEndian)
          flip(src, n);
        switch (e.packing) {
          case 0:
            for (x = 0; x < width * e.components; x++) {
              unsigned a = (x * 10) / 32;
              unsigned b = (x * 10) % 32;
              if (b > 22)
                buf[x] = ((src[a + 1] << (32 - b)) + (src[a] >> b)) & 0x3ff;
              else
                buf[x] = (src[a] >> b) & 0x3ff;
            }
            break;
          case 1:
            for (x = 0; x < n; x++) {
              buf[3 * x + 0] = (src[x] >> 22) & 0x3ff;
              buf[3 * x + 1] = (src[x] >> 12) & 0x3ff;
              buf[3 * x + 2] = (src[x] >> 02) & 0x3ff;
            }
            break;
          case 2:
            for (x = 0; x < n; x++) {
              buf[3 * x + 0] = (src[x] >> 20) & 0x3ff;
              buf[3 * x + 1] = (src[x] >> 10) & 0x3ff;
              buf[3 * x + 2] = (src[x] >> 00) & 0x3ff;
            }
            break;
        }
        break;
      }
      case 12:
        switch (e.packing) {
          case 0: {
            unsigned n = (e.bytes + 3) / 4;
            ARRAY(U32, src, n);
            get_line_from_file(src, index, e.dataOffset, e.bytes, y, e.bytes);
            if (_flipEndian)
              flip(src, n);
            for (unsigned x = 0; x < width * e.components; x++) {
              unsigned a = (x * 12) / 32;
              unsigned b = (x * 12) % 32;
              if (b > 20)
                buf[x] = ((src[a + 1] << (32 - b)) + (src[a] >> b)) & 0xfff;
              else
                buf[x] = (src[a] >> b) & 0xfff;
            }
            break;
          }
          case 1: {
            unsigned n = width * e.components;
            get_line_from_file(buf, index, e.dataOffset, e.bytes, y, n * 2);
            if (_flipEndian)
              flip(buf, n);
            for (unsigned x = 0; x < n; x++)
              buf[x] >>= 4;
            break;
          }
          case 2: {
            unsigned n = width * e.components;
            get_line_from_file(buf, index, e.dataOffset, e.bytes, y, n * 2);
            if (_flipEndian)
              flip(buf, n);
            for (unsigned x = 0; x < n; x++)
              buf[x] &= 0xfff;
            break;
          }
        }
        break;
      case 16:
        get_line_from_file(buf, index, e.dataOffset, e.bytes, y, width * e.components * 2);
        if (_flipEndian)
          flip(buf, width * e.components);
        break;
    }
    // now convert to rgb
    switch (e.descriptor) {

      case DESCRIPTOR_CbCr:
        // to actually get rgb we need the Y from the other element. NYI
        CbConvert(row.writable(Chan_Green), buf, x, r, 1, e.bits);
        CrConvert(row.writable(Chan_Blue), buf, x, r, 1, e.bits);
        if (ycbcr_hack)
          fixYCbCr(x, r, false, row);
        break;
      case DESCRIPTOR_RGBA: {
        const U16* alpha = buf + x * 4 + 3;
        foreach(z, e.channels)
        from_short(z, row.writable(z) + x, buf + x * 4 + (z - 1), alpha, r - x, e.bits, 4);
        break;
      }
      case DESCRIPTOR_ABGR: {
        const U16* alpha = buf + x * 4;
        foreach(z, e.channels)
        from_short(z, row.writable(z) + x, buf + x * 4 + (4 - z), alpha, r - x, e.bits, 4);
        break;
      }
      case DESCRIPTOR_CbYCrY:
        YConvert(row.writable(Chan_Red), buf + 1, x, r, 2, e.bits);
        CbConvert(row.writable(Chan_Green), buf, x, r, 2, e.bits);
        CrConvert(row.writable(Chan_Blue), buf, x, r, 2, e.bits);
        fixYCbCr(x, r, false, row);
        break;
      case DESCRIPTOR_CbYACrYA:
        CbConvert(row.writable(Chan_Green), buf, x, r, 3, e.bits);
        CrConvert(row.writable(Chan_Blue), buf, x, r, 3, e.bits);
        YConvert(row.writable(Chan_Red), buf + 1, x, r, 3, e.bits);
        AConvert(row.writable(Chan_Alpha), buf + 2, x, r, 3, e.bits);
        fixYCbCr(x, r, true, row);
        break;
      case DESCRIPTOR_CbYCr:
        YConvert(row.writable(Chan_Red), buf + 1, x, r, 3, e.bits);
        if (e.bits == 10) {
          // The "flowers" 10-bit sample image has the Cr/Cb reversed. This may be
          // a mistake, and other small test images have them the other way.
          // However I am leaving this reversed as that is how previous versions
          // of Nuke read this file.
          CConvert(row.writable(Chan_Blue), buf, x, r, 3, e.bits);
          CConvert(row.writable(Chan_Green), buf + 2, x, r, 3, e.bits);
        }
        else {
          CConvert(row.writable(Chan_Green), buf, x, r, 3, e.bits);
          CConvert(row.writable(Chan_Blue), buf + 2, x, r, 3, e.bits);
        }
        fixYCbCr(x, r, false, row);
        break;
      case DESCRIPTOR_CbYCrA:
        CConvert(row.writable(Chan_Green), buf, x, r, 4, e.bits);
        YConvert(row.writable(Chan_Red), buf + 1, x, r, 4, e.bits);
        CConvert(row.writable(Chan_Blue), buf + 2, x, r, 4, e.bits);
        AConvert(row.writable(Chan_Alpha), buf + 3, x, r, 4, e.bits);
        fixYCbCr(x, r, true, row);
        break;
      case DESCRIPTOR_Y:
        if (ycbcr_hack) {
          YConvert(row.writable(Chan_Red), buf, x, r, 1, e.bits);
          break;
        } // else fall through:
      default: {
        int Z = 0;
        foreach(z, e.channels) {
          from_short(z, row.writable(z) + x, buf + Z + x * e.components, nullptr /*alpha*/, r - x, e.bits, e.components);
          if (Z + 1 < e.components)
            Z++;
        }
        break;
      }
    }
  }

  void read_element(const Element& e, unsigned int index, int y, int x, int r, Row& row)
  {
    if (e.bits <= 8)
      read_element8(e, index, y, x, r, row);
    else
      read_element16(e, index, y, x, r, row);
  }

  void open() override
  {
    FileReader::open();


    if (_readAllLines) {
      mFnAssert(_requestedLinesPreloadBuffer);
      readAllLines();
    }
  }


  void engine(int y, int x, int r, ChannelMask channels, Row& row) override
  {
    if (invertY()) {
      y = height - y - 1;
    }

    ChannelSet remaining(channels);
    if (ycbcr_hack && (channels & Mask_RGB))
      remaining += Mask_RGB;
    for (unsigned i = 0; i < kMaxDPXElements; i++) {
      if (element[i].channels & remaining) {
        read_element(element[i], i, y, x, r, row);
        remaining -= element[i].channels;
        if (!remaining)
          break;
      }
    }
  }

  //! Checks that the file size is large enough to contain the data the header claims is there.
  //! The file could still be corrupted in some way but this function can at least be used for some
  //! very basic error checking.
  bool checkFileSizeConsistency()
  {
    // It doens't make much sense to have no elements but the file would at least be big enough to hold them!
    if (_numElements == 0)
      return true;

    const Element& lastElement = element[_numElements - 1];
    int expectedFileSize = lastElement.dataOffset + lastElement.bytes * h();

    return _fileSize >= expectedFileSize;
  }

  // A 'temporary' hack to allow proper viewing of 1, 10 and 12 bit images, in the absence
  // of rendering code that can handle this itself. Without this the images will appear
  // essentially black.
  // Also rescales floating point output to the range [0, 1].
  #define SCALE_BITS_TO_DEST_RANGE

  //! Finds the first dpx element which has channels matching the requested channels,
  //! returning the index of that element, or -1 if no match is found. If @a matchRGBandRGBA
  //! is true then RGBA and RGB are considered a match (with the alpha channel in either
  //! requestedChannels or the dpx element). The exception is if @a requestedChannels is RGBA
  //! and the source has RGB in one element but A in another (which means we can't fetch
  //! the requested channels from a singel element).
  int FindElement(const ChannelSet& requestedChannels, bool matchRGBandRGBA)
  {
    int foundElementIndex = -1;

    if (matchRGBandRGBA)
    {
      bool rgbRequested = (requestedChannels == Mask_RGB);
      bool rgbaRequested = (requestedChannels == Mask_RGBA);
      bool sourceHasAlpha = channels() & Mask_Alpha;

      for (int elementIndex = 0; elementIndex < _numElements; ++elementIndex) {
        if ((requestedChannels == element[elementIndex].channels) ||
            (rgbRequested && (element[elementIndex].channels == Mask_RGBA)) ||
            (rgbaRequested && (element[elementIndex].channels == Mask_RGB) && !sourceHasAlpha)) {
          foundElementIndex = elementIndex;
          break;
        }
      }
    }
    else {
      for (int elementIndex = 0; elementIndex < _numElements; ++elementIndex) {
        if (requestedChannels == element[elementIndex].channels) {
          foundElementIndex = elementIndex;
          break;
        }
      }
    }

    return foundElementIndex;
  }

  PlanarReadInfo planarReadInfo(const ChannelSet& channels) override
  {
    // #rick: Currently, we also require the image bounds to match the format.
    mFnAssert(x() == 0);
    mFnAssert(y() == 0);
    mFnAssert(w() == format().width());
    mFnAssert(h() == format().height());

    // If the file size and header are inconsistent then the planar image decoding is likely
    // to cause a crash by trying to read beyond the end of the read buffer (it's possible that
    // it's only the final element that's affected and we might not be interested in that but
    // we don't bother making that distinction here - the file is screwed!).
    if (!checkFileSizeConsistency())
      return PlanarReadInfo();

    // The planar reading API is designed for whole images.
    DD::Image::Box bounds(0, 0, w(), h());

    // Only support packed channels, e.g., RGBRGBRGB... rather than RRR...GGG...BBB. The dpx format uses
    // packed channels within an element, which is all that concerns us here.
    bool packed = true;

    // We're expecting the user to ask for a typical 'layer' (in OpenEXR speak) of channels,
    // which we also expect to be found within a single dpx element. Bit length, colour curve, etc, etc,
    // can differ between elements so it only makes sense to pick 'ideal' values for these if we're
    // only pulling image data from a single element.
    // So, search the element(s) present until we find an exact match or if we're asking for RGBA and
    // the element has RGB, or vice versa.
    int srcElementIndex = FindElement(channels, true);

    // If no suitable element was found return an invalid PlanarReadInfo object.
    if (srcElementIndex == -1)
      return PlanarReadInfo();

    const Element& srcElement = element[srcElementIndex];

    bool isValid = true;

    DataTypeEnum dataType = eDataTypeNone;
    bool useClamps = false;
    int minValue = 0;
    int maxValue = 1;
    int whitePoint = 1; // Should we be setting this to something specific?

    ChannelSet srcChannels = srcElement.channels;

    switch (srcElement.bits) {

      case 0:
        dataType = eDataTypeNone;
        break;

      case 1:
        // We're going to unpack 1 bit to 8 bit.
      case 8:

        dataType = eDataTypeUInt8;  // Don't suggest signed 8 bit, even though dpx allows it.
        // Not sure about this.
        // I think I should use lowQuantity/highQuantity but our DPXImageInfoHeader treats them as R32, hmmm?
        useClamps = true;
        minValue = 0;
        maxValue = (1 << srcElement.bits) - 1;
        break;

      case 10:

        // #mat: We only want to mark "filled" dpx frames for the optimised path - this is indicated by packing = 1
        // Yes, 'packed' is confusingly indicated by packing = 0 (1 means 'filled').
        if (srcElement.channels == Mask_RGB && srcElement.packing == 1)
        {
          // #mat: Only do the 10-bit read optimisation for 10/10/10/2 RGB data. Specialised formats such as
          // 10-bit dpx files with alpha channels won't have been tagged as a single 32-bit red channel and so
          // we should treat them as 16-bit shorts
          dataType = eDataTypeUInt32_10bit;
          srcChannels = ChannelSet(Mask_Red);
        }
        else
        {
          dataType = eDataTypeUInt16;
        }
        useClamps = true;
        minValue = 0;
        maxValue = (1 << srcElement.bits) - 1;
        break;

      case 12:
        // We're going to unpack and align 10 and 12 bit formats to 16 bit alignment.
      case 16:

        // We assume the data is always in integer rather than floating point format, so don't suggest
        // half floats.
        dataType = eDataTypeUInt16; // Don't suggest signed 16 bit, even though dpx allows it - the decoder doesn't!

        // Not sure about this.
        // I think I should use lowQuantity/highQuantity but our DPXImageInfoHeader treats them as R32, hmmm?
        useClamps = true;
        minValue = 0;
        maxValue = (1 << srcElement.bits) - 1;
        break;

      case 32:

        // We assume the data is always in integer rather than floating point format, so don't suggest
        // floats.
        dataType = eDataTypeUInt32; // Don't suggest signed 32 bit, even though dpx allows it. Nuke doesn't support 32 bit files of any type anyway.
        // Not sure about this.
        // I think I should use lowQuantity/highQuantity but our DPXImageInfoHeader treats them as R32, hmmm?
        useClamps = true;
        minValue = 0;
        maxValue = (1 << srcElement.bits) - 1;
        break;

      case 64:
      default:  // #rick: assert? throw? flag for ignoring? eDataTypeNone?
        isValid = false;
        mFnAssert(false);
        break;
    }

    // Only report the channels common to those requested and the dpx element we're using - FindElement()
    // won't necessary select an element with an _exact_ channel match.
    ChannelSet commonChannels = channels;
    commonChannels &= srcChannels;
    int nComponents = commonChannels.size();

    // #rick:
    //  Need to decide how or whether to handle this 'better'.
    //  We don't really want to duplicate Y data across all three channels in the destination image plane - one of the motivations
    //  for all this generic image plane stuff is to use data as close to the source as possible and let the MUCH faster GPU handle
    //  as much as possible.
    //  Currently, if this case happens, Cyclone will fallback to using the default Reader implementation.
    //
    // Require the number of components in the source element to match the number of channels, e.g., 3 components for RGB channels.
    // This will not be the case for, say, Y data that has one component in the element but the ctor sets the element channels to RGB.
    isValid = (nComponents == srcElement.components);

    // #mat: Special case for 10-bit data - we read 3 channels from Nuke, but we output a single channel 32-bit word with packed 10-bit
    // data. This is perfectly fine so we don't need to go down the error path
    if (!isValid)
    {
      isValid = (dataType == eDataTypeUInt32_10bit && nComponents == 1);
    }

    // #rick: Would be nice to remove these exclusions, but we don't want to do the necessary processing in the dpxReader - the
    // whole point of this is to do as little as possible and let the GPU do what it's much faster at.
    // If this list is modified, remember to update the asserts in planarDecodePass().
    //
    // Check the source element isn't one of the formats the planar decoding still doesn't support. The above tests won't exclude
    // everything we can't handle - for example, YCbCr with equal bits depths for the colour and luminance/luma channels can
    // be placed in a single 3 component, 3 channel element (like standard RGB).
    if ((srcElement.descriptor == DESCRIPTOR_Y) ||
        (srcElement.descriptor == DESCRIPTOR_CbCr) ||
        (srcElement.descriptor == DESCRIPTOR_ABGR) ||  // #rick: Think this will work but haven't been able to test. <<<<<<<<<<
        (srcElement.descriptor == DESCRIPTOR_CbYCrY) ||
        (srcElement.descriptor == DESCRIPTOR_CbYACrYA) ||
        (srcElement.descriptor == DESCRIPTOR_CbYCr) ||
        (srcElement.descriptor == DESCRIPTOR_CbYCrA)) {
      return PlanarReadInfo();
    }

    ImagePlaneDescriptor baseDesc(bounds, packed, commonChannels, nComponents);
    DataInfo dataInfo(dataType, useClamps, minValue, maxValue, whitePoint);

    ColorCurveEnum colorCurve;
    switch (srcElement.transfer) {
      case TRANSFER_USER:             // From original comment in ctor: seems to be used by some log files
      case TRANSFER_DENSITY:
      case TRANSFER_LOG:
        colorCurve = eColorCurveLog;
        break;
      case TRANSFER_CCIR_709_1:
        colorCurve = eColorCurveRec709;
        break;
      case TRANSFER_LINEAR:
        // The original code uses the sRGB LUT for this. The comment in the ctor says "unfortunatly too
        // much software writes this for sRGB..."
        // Confusingly (to me), the LUT code then decides that a request for an INT8 or INT16 LUT should result
        // in an sRGB response.
        // Under Bruno's direction the code here 'does what Nuke does'.
      default:
        colorCurve = eColorCurveSRGB;
        break;
    }

    GenericImagePlaneDescriptor desc(baseDesc, dataInfo, colorCurve);

    // #rick:
    // It's possible that there are more than one elements with data, in which case we'd be reading potentially
    // much more than is required. Is this ever likely to happen in practice?

    // For simplicity, just specify sufficient memory for the whole file.
    size_t readPassBufferSize = _fileSize;

    // The decode step can be easily multithreaded as each pixel, line or region of the image can be decoded in isolation.
    bool isDecodeThreadable = true;

    return PlanarReadInfo(desc, readPassBufferSize, isDecodeThreadable, isValid);
  }

  //! Reads and decodes the dpx in a single call, filling the specified channels of image. Note that this is single threaded,
  //! it's almost certainly more optimal for the user code to call planarReadPass() followed by multiple calls to planarDecodePass()
  //! within separate concurrent threads.
  void planarReadAndDecode(GenericImagePlane& image, const ChannelSet& channels, int priority) override
  {
    // Local class (well, struct) to ensure the temporary read buffer gets freed, even if an exception is thrown.
    // If we weren't prohibited from introducing a dependency on boost in this part of the code I'd use a boost::scoped_array.
    struct AutoBuffer
    {
      AutoBuffer(int bufferSize) { _buffer = new char[bufferSize]; }
      ~AutoBuffer() { delete [] _buffer; }
      char* _buffer;
    };

    AutoBuffer readBuffer(_fileSize);

    planarReadPass(static_cast<void*>(readBuffer._buffer), channels);
    planarDecodePass(static_cast<void*>(readBuffer._buffer), image, channels, 0, 1, priority);
  }

  //! Reads the dpx file into the specified buffer. In this case, channels is ignored - the whole file is read into the buffer.
  //! It's up to the user to ensure buffer points to enough memory - that can be determined with a previous call to planarReadInfo()
  //! and then using the value returned by calling readPassBufferSize() on the PlanarReadInfo object.
  int planarReadPass(void* buffer, const ChannelSet& channels) override
  {
    return read(buffer, 0, _fileSize);
  }

  //! Decodes the source buffer, obtained from a dpx file via an earlier call to planarReadPass(), into the specified
  //! GenericImagePlane, for the specified channels.
  //!
  //! The 'decode' is actually little more than a copy, no colour response LUT is applied. However, 10 and 12 bit source
  //! data is unpacked to be 16 bit aligned.
  //!
  //! The thread parameters indicate which part of the image to decode. It's up to the calling code to set this up
  //! appropriately, this function has no thread safety locks or checks.
  //!
  //! NOTE: Currently only supports cases where the specified channels _exactly_ match those of one of the dpx's elements.
  //!
  void planarDecodePass(void* srcBuffer, GenericImagePlane& image, const ChannelSet& channels,
                        int threadIndex, int nDecodeThreads, int priority) override
  {
    mFnAssert(threadIndex >= 0);
    mFnAssert(threadIndex < nDecodeThreads);
    mFnAssert(nDecodeThreads > 0);

    // We can't handle files that have somehow been truncated and are missing data.
    // The planarReadInfo() function will have returned a default PlanarReadInfo which is marked
    // as invalid to indicate to the user that planar reading/decoding is not possible.
    mFnAssert(checkFileSizeConsistency());

    // Assert that the image plane we're filling in is configured to hold the whole image, not a sub area of it.
    // Although the dpx format supports specifying the offset and size of the 'original' image (in the orientation
    // header) we don't bother with that, either in this new planar read/decode code or in the original.
    mFnAssert(image.desc().bounds().x() == 0);
    mFnAssert(image.desc().bounds().y() == 0);
    mFnAssert(image.desc().bounds().w() == w());
    mFnAssert(image.desc().bounds().h() == h());

    // Determine which rows this thread should decode. The end row of the last thread is always set to the
    // final row of the image to cope with the remainder of numRowsPerThread if the image height is not an
    // integer multiple of the number of threads. Note that the rows are specified _inclusively_, i.e., each thread
    // should loop from yStart to <= yEnd.
    int numRowsPerThread = height / nDecodeThreads;
    int yStart = threadIndex * numRowsPerThread;
    int yEnd = (threadIndex != (nDecodeThreads - 1)) ? yStart + numRowsPerThread - 1 : height - 1;

    // Get the element containing the required channels.
    // We require an exact match, except for RGB and RGBA, it's up to the caller to ask for the right thing,
    // they should have used planarReadInfo() to work set stuff up appropriately.
    int srcElementIndex = FindElement(channels, true);

    if (srcElementIndex == -1) {
      mFnAssert(false); // #rick: What should we really do? Throw?
      return;
    }

    const Element& srcElement = element[srcElementIndex];

    // See whether we're decoding from RGB source into an RGBA destination.
    bool rgbToRgba = ((channels == Mask_RGBA) && (srcElement.channels == Mask_RGB));

    // Unless we're doing the RGB-to-RGBA decode, require the number of channels to match, which
    // as well as excluding cases we can't currently handle also excludes RGBA source to RGB destination,
    // which would be permitted by the FindElement() call above.
    if (!rgbToRgba && (channels.size() != srcElement.channels.size())) {
      mFnAssert(false);  // #rick: What should we really do? Throw?
      return;
    }


    // #rick: Should match the exclusion list in planarReadInfo().
    mFnAssert(srcElement.descriptor != DESCRIPTOR_Y);
    mFnAssert(srcElement.descriptor != DESCRIPTOR_CbCr);
    mFnAssert(srcElement.descriptor != DESCRIPTOR_ABGR); // #rick: Think this will work but haven't been able to test.
    mFnAssert(srcElement.descriptor != DESCRIPTOR_CbYCrY);
    mFnAssert(srcElement.descriptor != DESCRIPTOR_CbYACrYA);
    //    mFnAssert(srcElement.descriptor != DESCRIPTOR_CbYCr);
    mFnAssert(srcElement.descriptor != DESCRIPTOR_CbYCrA);

  #if FN_HIERO_DDIMAGE_INVERT_Y
    // Temp hack neede while Hiero is not using the DDImage Reader objects via Core/Media.
    const bool flipY = (orientation & 2);
  #else
    // If bit 2 is not set then the dpx was marked as top to bottom and we need to vertically flip, otherwise it's bottom to top.
    const bool flipY = !(orientation & 2);
  #endif

    switch (srcElement.bits)
    {
      case 1:

        mFnAssert(false);  // #rick: Add support for 1 bit source, if we really need to.
        break;

      case 8:

        decodeFrom8(flipY, rgbToRgba, srcElement, srcBuffer, yStart, yEnd, image);
        break;

      case 10:

        switch(srcElement.packing) {
          case 0:
            // The packed case (packed has packing == 0, confusingly), i.e., no padding.
            decodeFromPacked10or12(flipY, rgbToRgba, srcElement, srcBuffer, yStart, yEnd, image);
            break;
          case 1:
          case 2:
            decodeFromFilled10(flipY, rgbToRgba, srcElement, srcBuffer, yStart, yEnd, image);
            break;
          default:
            mFnAssert(false);  // Invalid packing value, something's gone horribly wrong.
            break;
        }
        break;

      case 12:

        switch(srcElement.packing) {
          case 0:
            // The packed case (packed has packing == 0, confusingly), i.e., no padding.
            decodeFromPacked10or12(flipY, rgbToRgba, srcElement, srcBuffer, yStart, yEnd, image);
            break;
          case 1:
          case 2:
            decodeFromFilled12(flipY, rgbToRgba, srcElement, srcBuffer, yStart, yEnd, image);
            break;
          default:
            mFnAssert(false);  // Invalid packing value, something's gone horribly wrong.
            break;
        }
        break;

      case 16:

        decodeFrom16(flipY, rgbToRgba, srcElement, srcBuffer, yStart, yEnd, image);
        break;

      case 32:
      case 64:

        // Valid dpx bit depths but we don't support them (yet).
        mFnAssert(false);
        break;

      default:

        // Invalid dpx bit depth - something's gone horribly wrong.
        mFnAssert(false);
        break;
    }
  }

  //! Wrapper to select the appropriate instantiation of the decodeFrom8() template function.
  void decodeFrom8(bool flipY, bool rgbToRgba, const Element& e, const void* srcBuffer, int yStart, int yEnd, GenericImagePlane& image)
  {
    if (flipY && rgbToRgba)
      decodeFrom8<true, true>(e, srcBuffer, yStart, yEnd, image);
    else if (flipY)
      decodeFrom8<true, false>(e, srcBuffer, yStart, yEnd, image);
    else if (rgbToRgba)
      decodeFrom8<false, true>(e, srcBuffer, yStart, yEnd, image);
    else
      decodeFrom8<false, false>(e, srcBuffer, yStart, yEnd, image);
  }

  //! Decodes 8 bit per channel source data for the specified rows (inclusively) into the desination image, assuming the source
  //! element and image contain exactly the same channels if RgbToRgba is false. If RgbToRgba is true then
  //! the code assumes the source contains groups of RGB channels in the source and the destination also has an alpha channel, into
  //! which 1s are written to the bit depth of the channels.
  //! The 'decode' is really just a copy - no colour curve LUT is applied.
  //! The source image data should be in the specified buffer, srcBuffer, which is assumed to be the start address of the
  //! entire dpx file, buffered in memory. The e argument indicates which dpx element holds the required image data.
  //! The function is templated on whether vertical flipping of the image is required, thus removing the inner loop branch
  //! otehrwise needed to select the approriate row of the destination image data.
  //!
  //! Warning: Currently hard-coded to write to U8 image channels.
  template<bool FlipY, bool RgbToRgba>
  void decodeFrom8(const Element& e, const void* srcBuffer, int yStart, int yEnd, GenericImagePlane& image)
  {
    mFnAssert(e.bits == 8);
    mFnAssert((!RgbToRgba && (e.channels == image.desc().channels())) ||
              (RgbToRgba && (e.channels == Mask_RGB) && (image.desc().channels() == Mask_RGBA)));

    U8 alphaValue =  iop->autoAlpha() ? 0xff : 0; // Hard-coded to dest U8.

    for (int y = yStart; y <= yEnd; ++y) {

      int yImage = y;
      if (FlipY)
        yImage = height - y - 1;

      // #rick: Hard-coded to 8 bit destination image plane.
      mFnAssert(image.desc().dataInfo().dataType() == eDataTypeUInt8);
      U8* destBuffer = &(image.writableAt<U8>(0, yImage, 0));

      const U8* srcAddr = static_cast<const U8*>(srcBuffer) + e.dataOffset + y * e.bytes; // Note: The line may have padding, which e.bytes includes.

      if (RgbToRgba) {
        for (unsigned int column = 0; column < width; ++column) {
          *destBuffer++ = *srcAddr++;
          *destBuffer++ = *srcAddr++;
          *destBuffer++ = *srcAddr++;
          *destBuffer++ = alphaValue;
        }
      }
      else {

        int numComponentsPerRow = width * e.components;
        for (int componentCount = 0; componentCount < numComponentsPerRow; ++componentCount) {
          *destBuffer++ = *srcAddr++;
        }
      }
    }

  }

  //! Wrapper to select the appropriate instantiation of the decodeFromPacked10or12() template function.
  void decodeFromPacked10or12(bool flipY, bool rgbToRgba, const Element& e, const void* srcBuffer, int yStart, int yEnd, GenericImagePlane& image)
  {
    if (flipY && _flipEndian && rgbToRgba)
      decodeFromPacked10or12<true, true, true>(e, srcBuffer, yStart, yEnd, image);
    else if (flipY && _flipEndian)
      decodeFromPacked10or12<true, true, false>(e, srcBuffer, yStart, yEnd, image);
    else if (flipY && rgbToRgba)
      decodeFromPacked10or12<true, false, true>(e, srcBuffer, yStart, yEnd, image);
    else if (flipY)
      decodeFromPacked10or12<true, false, false>(e, srcBuffer, yStart, yEnd, image);
    else if (_flipEndian && rgbToRgba)
      decodeFromPacked10or12<false, true, true>(e, srcBuffer, yStart, yEnd, image);
    else if (_flipEndian)
      decodeFromPacked10or12<false, true, false>(e, srcBuffer, yStart, yEnd, image);
    else if (rgbToRgba)
      decodeFromPacked10or12<false, false, true>(e, srcBuffer, yStart, yEnd, image);
    else
      decodeFromPacked10or12<false, false, false>(e, srcBuffer, yStart, yEnd, image);
  }

  //! Decodes _packed_ 10 or 12 bit per channel source data for the speicified rows (inclusively) into the destination image, assuming the
  //! source element and destination image contain exactly the same channels if RgbToRgba is false. If RgbToRgba is true then
  //! the code assumes the source contains groups of RGB channels in the source and the destination also has an alpha channel, into
  //! which 1s are written to the bit depth of the channels.
  //! In this dpx context, packed means that there is no padding between components, i.e., only some components are aligned
  //! to 8, 12 or 32 bit boundaries. Apart from the unpacking/aligning, the 'decode' is really just a copy - no colour curve LUT is applied.
  //! The source image data should be in the specified buffer, srcBuffer, which is assumed to be the start address of the
  //! entire dpx file, buffered in memory. The e argument indicates which dpx element holds the required image data.
  //! The function is templated on whether vertical flipping of the image is required, thus removing the inner loop branch
  //! otehrwise needed to select the approriate row of the destination image data.
  //! It's also templated on whether endian flipping is required, thus removing the inner loop branch to check whether to endian
  //! flip that would otherwise be required (and the flip itself, of course, if not required).
  //!
  //! Warning: Currently hard-coded to write to U16 image channels.
  template<bool FlipY, bool EndianFlip, bool RgbToRgba>
  void decodeFromPacked10or12(const Element& e, const void* srcBuffer, int yStart, int yEnd, GenericImagePlane& image)
  {
    mFnAssert(e.packing == 0); // Yes, 'packed' is confusingly indicated by packing = 0 (1 means 'filled').
    mFnAssert((e.bits == 10) || (e.bits == 12));
    mFnAssert((!RgbToRgba && (e.channels == image.desc().channels())) ||
              (RgbToRgba && (e.channels == Mask_RGB) && (image.desc().channels() == Mask_RGBA)));

    U32 bitMask = (1 << e.bits) - 1;
    U16 alphaValue =  iop->autoAlpha() ? bitMask : 0; // Hard-coded to dest U16.

  #ifdef SCALE_BITS_TO_DEST_RANGE

    int numDestBits = sizeof(U16) * 8;  // Hard-coded to U16 destination.

    // This assumes the source bit count is no greater than the dest bit count. Otherwise we'd have to use
    // a right rather than left shift as shifting by a negative number gives undefined behaviour.
    // It also assumes this bit count difference is not greater than the source bit count, otherwise the
    // right shift will be undefined.
    // In summary: Only safe for the 10 or 12 to 16 bit scaling!
    mFnAssert((numDestBits == 16) && ((e.bits == 10) || (e.bits == 12)));

    // For efficiency, use bit shifting rather than scaling via floating point maths.
    // We can't just bit shift left by diffBits because we need the maximum source value (10 or 12 1s)
    // to scale to the maximum destination value (16 1s).
    // The left shift alone leaves either 6 or 4 zero lsbs. Jerry suggested we fill them with the 6 or
    // 4 msbs from the source value. I can't decide whether that's genuinely mathematically justifiable
    // but it doesn't differ from the float scaled version much and crucially we get the correct
    // maximum value.
    int diffBits = numDestBits - e.bits;
    int rangeScaleLeftShift = diffBits;
    int rangeScaleRightShift = (e.bits - diffBits);

    // The inserted alpha value should either be 0 or all of the dest bits.
    alphaValue = iop->autoAlpha() ? ((1 << numDestBits) - 1) : 0;
  #endif

    for (int y = yStart; y <= yEnd; ++y) {

      int yImage = y;
      if (FlipY)
        yImage = height - y - 1;

      U16* destBuffer = &(image.writableAt<U16>(0, yImage, 0)); // Hard-coded to U16 destination.
      const U32* srcWordAddr = reinterpret_cast<const U32*>(static_cast<const char*>(srcBuffer) + e.dataOffset + y * e.bytes);

      // #rick: Try to rewrite this to avoid the local buffer. It'll be fiddly because channels can straddle 32 bit words,
      //        which might need flipping. The problem is to avoid flipping any words twice - I think this can only be
      //        done with an if statement.

      int numWords = (e.bytes + 3) / 4;
      ARRAY(U32, rawBuffer, numWords);

      memcpy(rawBuffer, srcWordAddr, e.bytes);
      if (EndianFlip)
        flip(rawBuffer, numWords);

      // The first bit, within a 32 bit word, at which the component must straddle the boundary with the next 32 bit word.
      int firstStraddlingBit = 32 - e.bits;

      int numComponentsPerRow = width * e.components;
      for (int componentCount = 0; componentCount < numComponentsPerRow; ++componentCount) {

        int startBitCount = componentCount * e.bits;
        int relatativeStartBitCount = startBitCount % 32; // Bit index from the start of the current 32 bit word.
        int srcWordIndex = startBitCount / 32;

        if (relatativeStartBitCount > firstStraddlingBit) {
          // This component is close enough to the end of the current word that it must straddle the boundary into the
          // next word, so we need the usual right shift for the part that's in the this word and then shift the next
          // word to just after the bits present in the current word (then apply the e.bits mask).
  #ifdef SCALE_BITS_TO_DEST_RANGE
          U32 unscaledValue = ((rawBuffer[srcWordIndex + 1] << (32 - relatativeStartBitCount)) + (rawBuffer[srcWordIndex] >> relatativeStartBitCount)) & bitMask;
          *destBuffer++ = static_cast<U16>((unscaledValue << rangeScaleLeftShift) | (unscaledValue >> rangeScaleRightShift));
  #else
          *destBuffer++ = ((rawBuffer[srcWordIndex + 1] << (32 - relatativeStartBitCount)) + (rawBuffer[srcWordIndex] >> relatativeStartBitCount))
          & bitMask;
  #endif
        }
        else {
          // All the bits of this component fit in the current word so we just need to right shift them to the start
          // (of the current dest buffer component) and apply the e.bits mask.
  #ifdef SCALE_BITS_TO_DEST_RANGE
          U32 unscaledValue = (rawBuffer[srcWordIndex] >> relatativeStartBitCount) & bitMask;
          *destBuffer++ = static_cast<U16>((unscaledValue << rangeScaleLeftShift) | (unscaledValue >> rangeScaleRightShift));
  #else
          *destBuffer++ = (rawBuffer[srcWordIndex] >> relatativeStartBitCount) & bitMask;
  #endif
        }

        // #rick: Consider re-writing this code in terms of rgb so I can remove this branch. Mind you, we can't get rid of the
        // 'straddling bit' test above anyway, and I'm told that the packed format isn't widely used.
        if (RgbToRgba && ((componentCount + 1) % 3 == 0))
          *destBuffer++ = alphaValue; // Already scaled if SCALE_BITS_TO_DEST_RANGE defined.
      }
    }
  }

  //! Wrapper to select the appropriate instantiation of the decodeFromFilled10() template function.
  void decodeFromFilled10(bool flipY, bool rgbToRgba, const Element& e, const void* srcBuffer, int yStart, int yEnd, GenericImagePlane& image)
  {
    if (flipY && _flipEndian && rgbToRgba)
      decodeFromFilled10<true, true, true>(e, srcBuffer, yStart, yEnd, image);
    else if (flipY && _flipEndian)
      decodeFromFilled10<true, true, false>(e, srcBuffer, yStart, yEnd, image);
    else if (flipY && rgbToRgba)
      decodeFromFilled10<true, false, true>(e, srcBuffer, yStart, yEnd, image);
    else if (flipY)
      decodeFromFilled10<true, false, false>(e, srcBuffer, yStart, yEnd, image);
    else if (_flipEndian && rgbToRgba)
      decodeFromFilled10<false, true, true>(e, srcBuffer, yStart, yEnd, image);
    else if (_flipEndian)
      decodeFromFilled10<false, true, false>(e, srcBuffer, yStart, yEnd, image);
    else if (rgbToRgba)
      decodeFromFilled10<false, false, true>(e, srcBuffer, yStart, yEnd, image);
    else
      decodeFromFilled10<false, false, false>(e, srcBuffer, yStart, yEnd, image);
  }

  //! #mat: An optimised version of decodeFromFilled10 for 10-bit filled data. Instead of expanding to 16-bit RGBA,
  //! we output a single 32-bit word per pixel which like the source data contains three 10-bit rgb channels.
  //! We reverse the order of the channels here because it appears to be more optimal to upload data to the card in
  //! reversed channel format (GL_UNSIGNED_INT_2_10_10_10_REV)
  template<bool FlipY, bool EndianFlip>
  void decodeFromFilled10Optimised(const Element& e, const void* srcBuffer, int yStart, int yEnd, GenericImagePlane& image)
  {
    mFnAssert(e.bits == 10);
    mFnAssert(image.desc().dataInfo().dataType() == eDataTypeUInt32_10bit);
    mFnAssert((e.packing == 1) || (e.packing == 2)); // #mat: untested with e.packing == 2 - should work though!

    U32 bitMask = (1 << e.bits) - 1;
    U8 alphaValue = iop->autoAlpha() ? 0x3 : 0;

    int bitShift1;
    int bitShift2;
    int bitShift3;

    if (e.packing == 1) {
      bitShift1 = 22;
      bitShift2 = 12;
      bitShift3 =  2;
    }
    else {
      bitShift1 = 20;
      bitShift2 = 10;
      bitShift3 =  0;
    }

    for (int y = yStart; y <= yEnd; ++y) {

      int yImage = y;
      if (FlipY)
        yImage = height - y - 1;

      const U8*   srcWordChar = reinterpret_cast<const U8*>(srcBuffer);
      const U8*   srcWordCharOffset = srcWordChar + e.dataOffset + (y * width * sizeof(unsigned int));
      const U32*  srcWordAddr = reinterpret_cast<const U32*>(srcWordCharOffset);

      U32* destBuffer = &(image.writableAt<U32>(0, yImage, 0));

      for (int x = 0; x < width; x++)
      {
        U32 srcWord = *srcWordAddr++;

        if (EndianFlip)
          srcWord = (srcWord >> 24) | (srcWord >> 8 & 0xff00) | (srcWord << 8 & 0xff0000) | (srcWord << 24);

        // #mat: So I've found that GL_UNSIGNED_INT_2_10_10_10_REV is much faster to upload than GL_UNSIGNED_INT_10_10_10_2 - at least on Windows and Linux on the Quadro Q4000 card.
        // Thus I'm changing the order of the fetched channels here.
        // This should be tested on other platforms to determine whether this is platform/device/driver specific.
        *destBuffer++ = ((srcWord >> bitShift1) & bitMask) | (((srcWord >> bitShift2) & bitMask) << 10) | (((srcWord >> bitShift3) & bitMask) << 20) | alphaValue << 30;
      }
    }
  }

  //! Decodes _filled_ 10 bit per channel source data for the speicified rows (inclusively) into the destination image, assuming the
  //! source element and destination image contain exactly the same channels if RgbToRgba is false. If RgbToRgba is true then
  //! the code assumes the source contains groups of RGB channels in the source and the destination also has an alpha channel, into
  //! which 1s are written to the bit depth of the channels.
  //! In this dpx context, filled means that 3 components are stored in each 32 bit word with the remaining 2 bits padded.
  //! Apart from the unpacking/aligning, the 'decode' is really just a copy - no colour curve LUT is applied.
  //! The source image data should be in the specified buffer, srcBuffer, which is assumed to be the start address of the
  //! entire dpx file, buffered in memory. The e argument indicates which dpx element holds the required image data.
  //! The function is templated on whether vertical flipping of the image is required, thus removing the inner loop branch
  //! otehrwise needed to select the approriate row of the destination image data.
  //! It's also templated on whether endian flipping is required, thus removing the inner loop branch to check whether to endian
  //! flip that would otherwise be required (and the flip itself, of course, if not required).
  //!
  //! Warning: Currently hard-coded to write to U16 image channels.
  template<bool FlipY, bool EndianFlip, bool RgbToRgba>
  void decodeFromFilled10(const Element& e, const void* srcBuffer, int yStart, int yEnd, GenericImagePlane& image)
  {
    mFnAssert(e.bits == 10);
    mFnAssert((e.packing == 1) || (e.packing == 2));

    // #mat: Special case for 10-bit RGBA DPX - let's read the data but keep it in a 32-bit word. We need to deal with endian-ness issues and possibly reverse the channels however
    // #mat: Also let's only deal with normal packing - I haven't been able to find any footage that has (e.packing == 2) so this is untested. However we definitely don't want it to
    // go down the original path as the client may have only allocated enough memory for packed 10-bit data which would result in buffer overrun :-/
    if (image.desc().dataInfo().dataType() == eDataTypeUInt32_10bit && (e.packing == 1 || e.packing == 2))
    {
      decodeFromFilled10Optimised<FlipY, EndianFlip>(e, srcBuffer, yStart, yEnd, image);
      return;
    }

    mFnAssert((!RgbToRgba && (e.channels == image.desc().channels())) ||
              (RgbToRgba && (e.channels + Mask_Alpha == image.desc().channels())));

    U32 bitMask = (1 << e.bits) - 1;
    U16 alphaValue = iop->autoAlpha() ? bitMask : 0;  // Hard-coded to U16.

  #ifdef SCALE_BITS_TO_DEST_RANGE

    int numDestBits = sizeof(U16) * 8;  // Hard-coded to U16 destination.

    // This assumes the source bit count is no greater than the dest bit count. Otherwise we'd have to use
    // a right rather than left shift as shifting by a negative number gives undefined behaviour.
    // It also assumes this bit count difference is not greater than the source bit count, otherwise the
    // right shift will be undefined.
    // In summary: Only safe for the 10 or 12 to 16 bit scaling!
    mFnAssert((numDestBits == 16) && ((e.bits == 10) || (e.bits == 12)));

    // For efficiency, use bit shifting rather than scaling via floating point maths.
    // We can't just bit shift left by diffBits because we need the maximum source value (10 or 12 1s)
    // to scale to the maximum destination value (16 1s).
    // The left shift alone leaves either 6 or 4 zero lsbs. Jerry suggested we fill them with the 6 or
    // 4 msbs from the source value. I can't decide whether that's genuinely mathematically justifiable
    // but it doesn't differ from the float scaled version much and crucially we get the correct
    // maximum value.
    int diffBits = numDestBits - e.bits;
    int rangeScaleLeftShift = diffBits;
    int rangeScaleRightShift = (e.bits - diffBits);

    // The inserted alpha value should either be 0 or all of the dest bits.
    alphaValue = iop->autoAlpha() ? ((1 << numDestBits) - 1) : 0;
  #endif

    int bitShift1;
    int bitShift2;
    int bitShift3;

    if (e.packing == 1) {
      bitShift1 = 22;
      bitShift2 = 12;
      bitShift3 =  2;
    }
    else {
      bitShift1 = 20;
      bitShift2 = 10;
      bitShift3 =  0;
    }

    for (int y = yStart; y <= yEnd; ++y) {

      int yImage = y;
      if (FlipY)
        yImage = height - y - 1;

      // #rick:
      // Hard-coded to a U16 image buffer.
      // I _think_ I can just convert this to a template function which will work for U16, U32, F32 (and U64, F64). I should check.
      mFnAssert(image.desc().dataInfo().dataType() == eDataTypeUInt16);
      U16* destBuffer = &(image.writableAt<U16>(0, yImage, 0));

      const U32* srcWordAddr = reinterpret_cast<const U32*>(static_cast<const char*>(srcBuffer) + e.dataOffset + y * e.bytes);
      const int numComponentsPerRow = width * e.components;
      const int numFilledWords = numComponentsPerRow / 3;   // 10 bit filled stores 3 components per word, plus any remainder in the last word.

      for (int filledWordCount = 0; filledWordCount < numFilledWords; ++filledWordCount) {
        U32 srcWord = *srcWordAddr++;

        if (EndianFlip)
          srcWord = (srcWord >> 24) | (srcWord >> 8 & 0xff00) | (srcWord << 8 & 0xff0000) | (srcWord << 24);

  #ifdef SCALE_BITS_TO_DEST_RANGE
        U32 unscaledValue = (srcWord >> bitShift1) & bitMask;
        *destBuffer++ = static_cast<U16>((unscaledValue << rangeScaleLeftShift) | (unscaledValue >> rangeScaleRightShift));

        unscaledValue = (srcWord >> bitShift2) & bitMask;
        *destBuffer++ = static_cast<U16>((unscaledValue << rangeScaleLeftShift) | (unscaledValue >> rangeScaleRightShift));

        unscaledValue = ((srcWord >> bitShift3) & bitMask);
        *destBuffer++ = static_cast<U16>((unscaledValue << rangeScaleLeftShift) | (unscaledValue >> rangeScaleRightShift));
  #else
        *destBuffer++ = (srcWord >> bitShift1) & bitMask;
        *destBuffer++ = (srcWord >> bitShift2) & bitMask;
        *destBuffer++ = (srcWord >> bitShift3) & bitMask;
  #endif

        if (RgbToRgba)
          *destBuffer++ = alphaValue; // Already scaled if SCALE_BITS_TO_DEST_RANGE defined.
      }

      int numRemainderComponents = numComponentsPerRow - numFilledWords * 3;

      // #rick: I'm not sure about this. It's not used for RGB, so I haven't managed to test it yet.
      if (numRemainderComponents > 0) {
        U32 srcWord = *srcWordAddr++;

        if (EndianFlip)
          srcWord = (srcWord >> 24) | (srcWord >> 8 & 0xff00) | (srcWord << 8 & 0xff0000) | (srcWord << 24);

  #ifdef SCALE_BITS_TO_DEST_RANGE
        U32 unscaledValue = (srcWord >> bitShift1) & ((1 << e.bits) - 1);
        *destBuffer++ = static_cast<U16>((unscaledValue << rangeScaleLeftShift) | (unscaledValue >> rangeScaleRightShift));
        if (numRemainderComponents > 1) {
          unscaledValue = (srcWord >> bitShift2) & ((1 << e.bits) - 1);
          *destBuffer++ = static_cast<U16>((unscaledValue << rangeScaleLeftShift) | (unscaledValue >> rangeScaleRightShift));
        }
        // #rick: Need to handle RgbToRgba here?
  #else
        *destBuffer++ = (srcWord >> bitShift1) & ((1 << e.bits) - 1);
        if (numRemainderComponents > 1)
          *destBuffer++ = (srcWord >> bitShift2) & ((1 << e.bits) - 1);
        // #rick: Need to handle RgbToRgba here?
  #endif
      }
    }
  }

  //! Wrapper to select the appropriate instantiation of the decodeFromFilled12() template function.
  void decodeFromFilled12(bool flipY, bool rgbToRgba, const Element& e, const void* srcBuffer, int yStart, int yEnd, GenericImagePlane& image)
  {
    if (flipY && _flipEndian && rgbToRgba)
      decodeFromFilled12<true, true, true>(e, srcBuffer, yStart, yEnd, image);
    else if (flipY && _flipEndian)
      decodeFromFilled12<true, true, false>(e, srcBuffer, yStart, yEnd, image);
    else if (flipY && rgbToRgba)
      decodeFromFilled12<true, false, true>(e, srcBuffer, yStart, yEnd, image);
    else if (flipY)
      decodeFromFilled12<true, false, false>(e, srcBuffer, yStart, yEnd, image);
    else if (_flipEndian && rgbToRgba)
      decodeFromFilled12<false, true, true>(e, srcBuffer, yStart, yEnd, image);
    else if (_flipEndian)
      decodeFromFilled12<false, true, false>(e, srcBuffer, yStart, yEnd, image);
    else if (rgbToRgba)
      decodeFromFilled12<false, false, true>(e, srcBuffer, yStart, yEnd, image);
    else
      decodeFromFilled12<false, false, false>(e, srcBuffer, yStart, yEnd, image);
  }

  //! Decodes _filled_ 12 bit per channel source data for the speicified rows (inclusively) into the destination image, assuming the
  //! source element and destination image contain exactly the same channels if RgbToRgba is false. If RgbToRgba is true then
  //! the code assumes the source contains groups of RGB channels in the source and the destination also has an alpha channel, into
  //! which 1s are written to the bit depth of the channels.
  //! In this dpx context, filled means that each 12 bit component is stored on a 16 bit boundary, with the remaining 4 bits being padding.
  //! Apart from the unpacking/aligning, the 'decode' is really just a copy - no colour curve LUT is applied.
  //! The source image data should be in the specified buffer, srcBuffer, which is assumed to be the start address of the
  //! entire dpx file, buffered in memory. The e argument indicates which dpx element holds the required image data.
  //! The function is templated on whether vertical flipping of the image is required, thus removing the inner loop branch
  //! otehrwise needed to select the approriate row of the destination image data.
  //! It's also templated on whether endian flipping is required, thus removing the inner loop branch to check whether to endian
  //! flip that would otherwise be required (and the flip itself, of course, if not required).
  //!
  //! Warning: Currently hard-coded to write to U16 image channels.
  template<bool FlipY, bool EndianFlip, bool RgbToRgba>
  void decodeFromFilled12(const Element& e, const void* srcBuffer, int yStart, int yEnd, GenericImagePlane& image)
  {
    mFnAssert(e.bits == 12);
    mFnAssert((!RgbToRgba && (e.channels == image.desc().channels())) ||
              (RgbToRgba && (e.channels + Mask_Alpha == image.desc().channels())));
    mFnAssert((e.packing == 1) || (e.packing == 2));

    // Each component is aligned to 16 bits, i.e., there's 4 bits of padding added per component.
    // For packing = 1 the padding is in the 4 least significant bits, while for packing = 2 the
    // padding is in the 4 most significant bits (both post endian flip). These seem to be referred
    // to as type a and b respectively.
    const int bitShift = (e.packing == 1) ? 4 : 0;

    // The mask to apply after we may or may not have actually shifted the 12 bits of channel data.
    const U16 mask = (1 << 12) - 1;

    // Set the alpha value we'll insert, if RgbToRgba is true, to the maximum value or 0 according to the
    // current autoAlpha setting.
    U16 alphaValue = iop->autoAlpha() ? mask : 0;

  #ifdef SCALE_BITS_TO_DEST_RANGE

    int numDestBits = sizeof(U16) * 8;  // Hard-coded to U16 destination.

    // This assumes the source bit count is no greater than the dest bit count. Otherwise we'd have to use
    // a right rather than left shift as shifting by a negative number gives undefined behaviour.
    // It also assumes this bit count difference is not greater than the source bit count, otherwise the
    // right shift will be undefined.
    // In summary: Only safe for the 10 or 12 to 16 bit scaling!
    mFnAssert((numDestBits == 16) && ((e.bits == 10) || (e.bits == 12)));

    // For efficiency, use bit shifting rather than scaling via floating point maths.
    // We can't just bit shift left by diffBits because we need the maximum source value (10 or 12 1s)
    // to scale to the maximum destination value (16 1s).
    // The left shift alone leaves either 6 or 4 zero lsbs. Jerry suggested we fill them with the 6 or
    // 4 msbs from the source value. I can't decide whether that's genuinely mathematically justifiable
    // but it doesn't differ from the float scaled version much and crucially we get the correct
    // maximum value.
    int diffBits = numDestBits - e.bits;
    int rangeScaleLeftShift = diffBits;
    int rangeScaleRightShift = (e.bits - diffBits);

    // The inserted alpha value should either be 0 or all of the dest bits.
    alphaValue = iop->autoAlpha() ? ((1 << numDestBits) - 1) : 0;
  #endif

    for (int y = yStart; y <= yEnd; ++y) {

      int yImage = y;
      if (FlipY)
        yImage = height - y - 1;

      // #rick:
      // Hard-coded to a U16 image buffer.
      // I _think_ I can just convert this to a template function which will work for U16, U32, F32 (and U64, F64). I should check.
      mFnAssert(image.desc().dataInfo().dataType() == eDataTypeUInt16);
      U16* destBuffer = &(image.writableAt<U16>(0, yImage, 0));

      // We're assuming the file was written a component at a time, i.e., 12+4 bits, hence we loop over each 16 byte half
      // word, endian flipping each in isolatation (if necessary), before the appropriate bit shift and mask.
      const U16* srcHalfWordAddr = reinterpret_cast<const U16*>(static_cast<const char*>(srcBuffer) + e.dataOffset + y * e.bytes);

      if (RgbToRgba) {

        for (unsigned int column = 0; column < width; ++column) {
          // Decode the three RGB channels from the source.
          U16 srcHalfWord = *srcHalfWordAddr++;
          if (EndianFlip)
            srcHalfWord = (srcHalfWord >> 8) | (srcHalfWord << 8);
  #ifdef SCALE_BITS_TO_DEST_RANGE
          U32 unscaledValue = (srcHalfWord >> bitShift) & mask;
          *destBuffer++ = static_cast<U16>((unscaledValue << rangeScaleLeftShift) | (unscaledValue >> rangeScaleRightShift));
  #else
          *destBuffer++ = (srcHalfWord >> bitShift) & mask;
  #endif
          srcHalfWord = *srcHalfWordAddr++;
          if (EndianFlip)
            srcHalfWord = (srcHalfWord >> 8) | (srcHalfWord << 8);
  #ifdef SCALE_BITS_TO_DEST_RANGE
          unscaledValue = (srcHalfWord >> bitShift) & mask;
          *destBuffer++ = static_cast<U16>((unscaledValue << rangeScaleLeftShift) | (unscaledValue >> rangeScaleRightShift));
  #else
          *destBuffer++ = (srcHalfWord >> bitShift) & mask;
  #endif
          srcHalfWord = *srcHalfWordAddr++;
          if (EndianFlip)
            srcHalfWord = (srcHalfWord >> 8) | (srcHalfWord << 8);
  #ifdef SCALE_BITS_TO_DEST_RANGE
          unscaledValue = (srcHalfWord >> bitShift) & mask;
          *destBuffer++ = static_cast<U16>((unscaledValue << rangeScaleLeftShift) | (unscaledValue >> rangeScaleRightShift));
  #else
          *destBuffer++ = (srcHalfWord >> bitShift) & mask;
  #endif
          // Now we've decoded the three RGB values we add the alpha value.
          *destBuffer++ = alphaValue; // Already scaled if SCALE_BITS_TO_DEST_RANGE defined.
        }
      }
      else {

        int numComponentsPerRow = width * e.components;

        for (int component = 0; component < numComponentsPerRow; ++component) {
          U16 srcHalfWord = *srcHalfWordAddr++;
          if (EndianFlip)
            srcHalfWord = (srcHalfWord >> 8) | (srcHalfWord << 8);

  #ifdef SCALE_BITS_TO_DEST_RANGE
          U32 unscaledValue = (srcHalfWord >> bitShift) & mask;
          *destBuffer++ = static_cast<U16>((unscaledValue << rangeScaleLeftShift) | (unscaledValue >> rangeScaleRightShift));
  #else
          *destBuffer++ = (srcHalfWord >> bitShift) & mask;
  #endif
        }
      }
    }
  }

  //! Wrapper to select the appropriate instantiation of the decodeFrom16() template function.
  void decodeFrom16(bool flipY, bool rgbToRgba, const Element& e, const void* srcBuffer, int yStart, int yEnd, GenericImagePlane& image)
  {
    if (flipY && _flipEndian && rgbToRgba)
      decodeFrom16<true, true, true>(e, srcBuffer, yStart, yEnd, image);
    else if (flipY && _flipEndian)
      decodeFrom16<true, true, false>(e, srcBuffer, yStart, yEnd, image);
    else if (flipY && rgbToRgba)
      decodeFrom16<true, false, true>(e, srcBuffer, yStart, yEnd, image);
    else if (flipY)
      decodeFrom16<true, false, false>(e, srcBuffer, yStart, yEnd, image);
    else if (_flipEndian && rgbToRgba)
      decodeFrom16<false, true, true>(e, srcBuffer, yStart, yEnd, image);
    else if (_flipEndian)
      decodeFrom16<false, true, false>(e, srcBuffer, yStart, yEnd, image);
    else if (rgbToRgba)
      decodeFrom16<false, false, true>(e, srcBuffer, yStart, yEnd, image);
    else
      decodeFrom16<false, false, false>(e, srcBuffer, yStart, yEnd, image);
  }

  //! Decodes 16 bit per channel source data for the specified rows (inclusively) into the desination image, assuming the source
  //! element and image contain exactly the same channels if RgbToRgba is false. If RgbToRgba is true then
  //! the code assumes the source contains groups of RGB channels in the source and the destination also has an alpha channel, into
  //! which 1s are written to the bit depth of the channels.
  //! The 'decode' is really just a copy - no colour curve LUT is applied.
  //! The source image data should be in the specified buffer, srcBuffer, which is assumed to be the start address of the
  //! entire dpx file, buffered in memory. The e argument indicates which dpx element holds the required image data.
  //! The function is templated on whether vertical flipping of the image is required, thus removing the inner loop branch
  //! otehrwise needed to select the approriate row of the destination image data.
  //! It's also templated on whether endian flipping is required, thus removing the inner loop branch to check whether to endian
  //! flip that would otherwise be required (and the flip itself, of course, if not required).
  //!
  //! Warning: Currently hard-coded to write to U16 image channels.
  template<bool FlipY, bool EndianFlip, bool RgbToRgba>
  void decodeFrom16(const Element& e, const void* srcBuffer, int yStart, int yEnd, GenericImagePlane& image)
  {
    mFnAssert(e.bits == 16);
    mFnAssert((!RgbToRgba && (e.channels == image.desc().channels())) ||
              (RgbToRgba && (e.channels + Mask_Alpha == image.desc().channels())));

    U16 alphaValue = iop->autoAlpha() ? 0xffff : 0; // Hard-coded to dest U16.

    for (int y = yStart; y <= yEnd; ++y) {

      int yImage = y;
      if (FlipY)
        yImage = height - y - 1;

      // #rick:
      // Hard-coded to a U16 image buffer.
      // I _think_ I can just convert this to a template function which will work for U16, U32, F32 (and U64, F64). I should check.
      mFnAssert(image.desc().dataInfo().dataType() == eDataTypeUInt16);
      U16* destBuffer = &(image.writableAt<U16>(0, yImage, 0));

      const U16* srcAddr = reinterpret_cast<const U16*> (static_cast<const char*>(srcBuffer) + e.dataOffset + y * e.bytes);

      if (RgbToRgba) {

        for (unsigned int column = 0; column < width; ++column) {

          // Note: We're assuming the data has been written out in 16 bit half word, so we (potentially) endian flip each
          // half word rather than 32 bit word.

          U16 srcComponent = *srcAddr++;
          if (EndianFlip)
            srcComponent = (srcComponent >> 8) | (srcComponent << 8);
          *destBuffer++ = srcComponent;

          srcComponent = *srcAddr++;
          if (EndianFlip)
            srcComponent = (srcComponent >> 8) | (srcComponent << 8);
          *destBuffer++ = srcComponent;

          srcComponent = *srcAddr++;
          if (EndianFlip)
            srcComponent = (srcComponent >> 8) | (srcComponent << 8);
          *destBuffer++ = srcComponent;

          *destBuffer++ = alphaValue;
        }
      }
      else {

        int numComponentsPerRow = width * e.components;
        for (int componentCount = 0; componentCount < numComponentsPerRow; ++componentCount) {

          U16 srcComponent = *srcAddr++;

          // Note: We're assuming the data has been written out in 16 bit half word, so we (potentially) endian flip each
          // half word rather than 32 bit word.
          if (EndianFlip)
            srcComponent = (srcComponent >> 8) | (srcComponent << 8);

          *destBuffer++ = srcComponent;
        }
      }
    }
  }
};

static bool test(int fd, const unsigned char* block, int length)
{
  DPXHeader* header = (DPXHeader*)block;
  U32 m = header->file.magicNumber;
  if (m == DPX_MAGIC || m == DPX_MAGIC_FLIPPED)
    return true;
  return false;
}

static Reader* build(Read* iop, int fd, const unsigned char* b, int n)
{
  return new dpxReader(iop, fd, b, n);
}

const Reader::Description dpxReader::description("dpx\0", build, test);

// end dpxReader.C
