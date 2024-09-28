// exrReader.C
// Copyright (c) 2009 The Foundry Visionmongers Ltd.  All Rights Reserved.

/* Reads exr files using libexr.

   04/14/03     Initial Release                Charles Henrich (henrich@d2.com)
   10/14/03     Added channel name conforming  Charles Henrich (henrich@d2.com)
   10/16/04    Lots of channel changes        Bill Spitzak
   03/27/05    Single frame buffer        Bill Spitzak
   01/17/08     all channel sorting done by Nuke spitzak
 */

#include <sstream>
#include <memory>
#include <mutex>
#include <fcntl.h>
#include <sys/stat.h>

#ifndef _WIN32
  #include <sys/mman.h>
#endif

#include "DDImage/DDMath.h"
#include "DDImage/DDWindows.h"
#include "DDImage/Reader.h"
#include "DDImage/Row.h"
#include "DDImage/Knob.h"
#include "DDImage/Knobs.h"
#include "DDImage/Thread.h"
#include "DDImage/Memory.h"
#include "DDImage/LUT.h"
#include "DDImage/ImagePlane.h"
#include "DDImage/TimelineRead.h"
#include "DDImage/Application.h"
#include "DDImage/NukePreferences.h"

#ifdef _WIN32
  #define OPENEXR_DLL
  #include <io.h>
#endif
#include <OpenEXR/ImfMultiPartInputFile.h>
#include <OpenEXR/ImfInputPart.h>
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
#include <OpenEXR/ImfFrameBuffer.h>
#include <OpenEXR/ImfMisc.h>
#include <OpenEXR/ImfCompressor.h>
#include <OpenEXR/ImfStdIO.h>
#include <OpenEXR/ImathFun.h>
#include <OpenEXR/ImathBox.h>

#include <OpenEXR/half.h>

#include <stdexcept>
#ifndef FN_OS_WINDOWS
  #include <unistd.h>
#endif

#include "exrGeneral.h"
#include "ExrChannelNameToNuke.h"

#ifndef MADV_HUGEPAGE
  #define MADV_HUGEPAGE 14
#endif

// Whether to print the EXR file info to the tty.
//#define ENABLE_EXR_INFO_TTY

//-------------------------------------------------------------------------------------------------

// Implements an OpenEXR IStream that reads the entire file in one go to a memory buffer,
// servicing the individual read/stream operations from that buffer.
// This is dramatically more performant than using the 'regular' OpenEXR implementation
// for at least some systems (e.g. specific macOS and SAN combination).
class BufferedFileStream : public OPENEXR_IMF_NAMESPACE::IStream
{
public:
  // Creates the memory buffer, reads the file contents into it then closes the file.
  // The file is assumed to have already been opened by the Read op that is creating the exrReader that
  // creates the BufferedFileStream. The file descriptor for that open file should be passed as fd.
  //
  // The Read op will also have read in a small part of the beginning of the file, which is specified
  // with preReadBuffer and preReadBufferSize.
  //
  // The filename is only required as part of the API for the OpenEXR IStream parent class and
  // in this case not actually used (there isn't a cross-platform way to determine this from a
  // file descriptor so it's easier to take it as an argument, since the Read op knows what it is).
  BufferedFileStream(const char* fileName, int fd, const unsigned char* preReadBuffer, int preReadBufferSize);

  ~BufferedFileStream() = default;

  bool isMemoryMapped() const override;
  bool read(char c[/*n*/], int n) override;
  char* readMemoryMapped(int n) override;
  OPENEXR_IMF_NAMESPACE::Int64 tellg() override;
  void seekg(OPENEXR_IMF_NAMESPACE::Int64 pos) override;
  void clear() override;

private:

  OPENEXR_IMF_NAMESPACE::Int64 _fileSize { 0 };
  OPENEXR_IMF_NAMESPACE::Int64 _readPos { 0 };

  std::unique_ptr<char[]> _buffer;  // Buffer used for storing the entire file.
};

// Helper class to close a file, specified by a file descriptor, when some scope is left.
class ScopedFileCloser
{
public:
  ScopedFileCloser(int fd) : _fd(fd) {}
  ~ScopedFileCloser() { close(_fd); }
private:
  const int _fd;
};

BufferedFileStream::BufferedFileStream(const char* fileName, int fd, const unsigned char* preReadBuffer, int preReadBufferSize)
  : OPENEXR_IMF_NAMESPACE::IStream(fileName)
{
  // Ensure the fd file gets closed when we leave this function regardless of the various possible exceptions.
  ScopedFileCloser fileCloser(fd);

  // Use the standard approach of lseeking to the end.
  _fileSize = lseek(fd, 0, SEEK_END);
  if (_fileSize == -1) {
    throw IEX_NAMESPACE::InputExc("File tell failed.");
  }

  if (_fileSize < preReadBufferSize) {
    throw IEX_NAMESPACE::InputExc("Invalid pre-read buffer size.");
  }

  try {
    // Don't use make_unique as it will default initialise the buffer, which is a pointless expense we don't need.
    _buffer.reset(new char[_fileSize]);   // TODO: Consider specific alignment.
  }
  catch (const std::bad_alloc& exc) {
    // Note that Memory::initialize() specifies its new_handler for std::set_new_handler but that still
    // just throws std::bad_alloc if it can't free any memory, which in the context of timeline usage
    // it almost certianly won't be able to do since it only knows about memory allocated through Memory,
    // which is only done by the node graph.
    throw IEX_NAMESPACE::InputExc("Failed to allocate file buffer.");
  }

  // Copy any pre-read part of the file into the start of our buffer.
  if (preReadBufferSize > 0) {
    memcpy(_buffer.get(), preReadBuffer, preReadBufferSize);
  }

  // Read the remainder of the file into the remainder of our buffer.
  const OPENEXR_IMF_NAMESPACE::Int64 numUnreadBytes = _fileSize - preReadBufferSize;
  if (numUnreadBytes > 0) {
    int result = lseek(fd, preReadBufferSize, SEEK_SET);
    if (result != preReadBufferSize) {
      throw IEX_NAMESPACE::InputExc("File seek to end of pre-read failed.");
    }

    const size_t bytesRead = ::read(fd, _buffer.get() + preReadBufferSize, numUnreadBytes);
    if (bytesRead != numUnreadBytes) {
      throw IEX_NAMESPACE::InputExc("Error reading file.");
    }
  }
}

bool BufferedFileStream::isMemoryMapped() const
{
  return true;  // NOTE: We're  not implementing the equivalent of MemoryMappedIStream's _lieAboutMemoryMapped.
}

bool BufferedFileStream::read(char c[/*n*/], int n)
{
  const auto newReadPos = _readPos + n;
  if (newReadPos > _fileSize) {
    throw Iex::InputExc("Attempt to read past eof.");
  }

  auto bufferAddr = _buffer.get() + _readPos;
  memcpy(c, bufferAddr, n);
  _readPos = newReadPos;

  return (_readPos < _fileSize);  // Return true if we haven't yet read the last byte.
}

char* BufferedFileStream::readMemoryMapped(int n)
{
  const auto newReadPos = _readPos + n;
  if (newReadPos > _fileSize) {
    throw Iex::InputExc("Attempt to read past eof.");
  }

  auto bufferAddr = _buffer.get() + _readPos;
  _readPos = newReadPos;

  return bufferAddr;
}

OPENEXR_IMF_NAMESPACE::Int64 BufferedFileStream::tellg()
{
  return _readPos;
}

void BufferedFileStream::seekg(OPENEXR_IMF_NAMESPACE::Int64 pos)
{
  _readPos = pos;
}

void BufferedFileStream::clear()
{
}

//-------------------------------------------------------------------------------------------------

class MemoryMappedIStream: public OPENEXR_IMF_NAMESPACE::IStream
{
  public:
    MemoryMappedIStream (const char *fileName, bool lieAboutMemoryMapped); // See comment on _lieAboutMemoryMapped for more details about this argument.
    virtual ~MemoryMappedIStream ();

    virtual bool isMemoryMapped() const override;
    virtual char* readMemoryMapped(int n) override;
    virtual bool read(char *c, int n) override;
    virtual OPENEXR_IMF_NAMESPACE::Int64 tellg() override;
    virtual void seekg(OPENEXR_IMF_NAMESPACE::Int64 pos) override;

  private:
    // The DWAA and DWAB decompressors make loads on unaligned offsets into the binary file. This is not a problem when "fread()-ing" data from disk into aligned ram buffers
    // as the CPU load instructions are then aligned regardless of where the binary data was originally positioned inside the file. Using mmap(), the file data is
    // becomes directly accessable via virtualised addresses. This means DWAA/B's access of unaligned offsets into the file causes load instructions into unaligned virtual addresses.
    // To solve this problem, this variable may be set to true when decompressing DWA/B files and MemoryMappedIStream will lie to the EXR library saying the IStream is not mmap()-ed.
    // This will then cause the EXR library to use IStream::read() in the place of IStream::readMemoryMapped(), where we will have the chance to memcpy the unaligned data into an
    // aligned buffer that has been provided by the OpenEXR library. This extra memcpy() adds some overhead but solves the unaligned access problem.
    // This should only be enabled for DWA/B compressed files in order to not pay the cost of the extra memcpy() on decode paths that do not make unaligned access.
    bool _lieAboutMemoryMapped;

    char* _buffer;

    OPENEXR_IMF_NAMESPACE::Int64 _fileLength;
    OPENEXR_IMF_NAMESPACE::Int64 _readPosition;
};

#ifdef _WIN32

class WideCharWrapper
{
public:
  WideCharWrapper(const char* str)  : _array(0)
  {
    int length = str ? static_cast<int>(strlen(str)) : 0;
    int size = MultiByteToWideChar(CP_UTF8, 0, str, length, nullptr, 0);

    _array = new wchar_t[size+1];

    MultiByteToWideChar(CP_UTF8, 0, str, length,_array, size);

    _array[size] = 0;
  }

  operator const unsigned short* () const
  {
    return reinterpret_cast<const unsigned short*>(_array);
  }

  const wchar_t* data() const { return _array; }
  
  ~WideCharWrapper()
  {
    delete [] _array;
  }
  
private:
  wchar_t* _array;
};

MemoryMappedIStream::MemoryMappedIStream(const char *fileName, bool lieAboutMemoryMapped) :
  IStream(fileName),
  _buffer(nullptr),
  _fileLength(0),
  _readPosition(0),
  _lieAboutMemoryMapped(lieAboutMemoryMapped)
{
  WideCharWrapper wideName(fileName);
  int file = (int)_wopen(wideName.data(), _O_RDONLY | _O_SEQUENTIAL);

   if (file < 0) {
    throw IEX_NAMESPACE::InputExc("No such file or directory");
  }

  struct stat stat;
  fstat (file, &stat);
  _fileLength = stat.st_size;

  HANDLE hmap;
  hmap = CreateFileMapping((HANDLE)_get_osfhandle(file), 0, PAGE_WRITECOPY, 0, 0, 0);

  if(!hmap) {
    throw IEX_NAMESPACE::InputExc("mmap() of file failed");
  }

  _buffer = (char*)MapViewOfFileEx(hmap, FILE_MAP_COPY, 0, 0, _fileLength, 0);
  close (file);

  if(!CloseHandle(hmap) || _buffer == nullptr) {
    throw IEX_NAMESPACE::InputExc("mmap() of file failed");
  }
}

MemoryMappedIStream::~MemoryMappedIStream ()
{
  UnmapViewOfFile(_buffer);
}

#else

MemoryMappedIStream::MemoryMappedIStream(const char *fileName, bool lieAboutMemoryMapped) :
  IStream(fileName),
  _buffer(nullptr),
  _fileLength(0),
  _readPosition(0),
  _lieAboutMemoryMapped(lieAboutMemoryMapped)
{
  int file = open(fileName, O_RDONLY);

  if (file < 0) {
    throw IEX_NAMESPACE::InputExc("No such file or directory");
  }

  struct stat stat;
  fstat (file, &stat);
  _fileLength = stat.st_size;

  _buffer = (char*)mmap(nullptr, _fileLength, PROT_READ, MAP_PRIVATE, file, 0);
  madvise(_buffer, _fileLength, MADV_SEQUENTIAL);

  close (file);

  if (_buffer == MAP_FAILED) {
    throw IEX_NAMESPACE::InputExc("mmap() of file failed");
  }
}

MemoryMappedIStream::~MemoryMappedIStream ()
{
  munmap(_buffer, _fileLength);
}

#endif // _WIN32


bool MemoryMappedIStream::isMemoryMapped () const
{
  return _lieAboutMemoryMapped == false;
}

char* MemoryMappedIStream::readMemoryMapped (int n)
{
  if (_readPosition >= _fileLength) {
    throw Iex::InputExc("Unexpected end of file.");
  }

  if (_readPosition + n > _fileLength) {
    throw Iex::InputExc("Reading past end of file.");
  }

  char *data = _buffer + _readPosition;
  _readPosition += n;

  return data;
}

bool MemoryMappedIStream::read(char *c, int n)
{
  if (_readPosition >= _fileLength) {
    throw Iex::InputExc("Unexpected end of file.");
  }

  if (_readPosition + n > _fileLength) {
    throw Iex::InputExc("Reading past end of file.");
  }

  memcpy(c, _buffer +_readPosition, n);
  _readPosition += n;

  return _readPosition < _fileLength;
}

OPENEXR_IMF_NAMESPACE::Int64 MemoryMappedIStream::tellg()
{
  return _readPosition;
}

void MemoryMappedIStream::seekg(OPENEXR_IMF_NAMESPACE::Int64 pos)
{
  _readPosition = pos;
}

//-------------------------------------------------------------------------------------------------

// Enumerates the alternative approaches to reading the EXR files.
enum class FileReadMode
{
  // Just pass the filename to the OpenEXR MultiPartInputFile and let it handle everything.
  // More strictly, for convenience we actually pass a std::ifstream wrapped in a StdIFStream,
  // which is what MultiPartInputFile currently does if given a filename.
  eNormal,

  // We mmap the file and pass that, wrapped in an IStream sub-class, to the MultiPartInputFile.
  eMmap,

  // Allocate a system memory buffer and read the entire contents of the file into that, then
  // pass that, wrapped in an IStream sub-class, to the MultiPartInputFile.
  // In addition, open and read the file just once - avoid opening the file with temporary
  // MultiPartInputFile objects to get header info.
  eBuffer,

  // Do whichever of the above the code previously did, for a given combination of OS and compression
  // type of the current EXR file.
  // This is for maintaining exact behaviour given that these changes are going into 12.2 and 13.0
  // maintenance releases, hence the code should only map this to either eNormal or eMmap, which
  // correspond to pre-existing code paths.
  eDefault
};

FileReadMode GetFileReadModeFromEnvVar()
{
  FileReadMode readMode = FileReadMode::eDefault;

  constexpr const char* envVarName = "FN_EXR_FILE_READ_MODE";
  const auto envVarValue = getenv(envVarName);

  if (envVarValue) {

    std::string readModeName(envVarValue);
    std::transform(readModeName.begin(), readModeName.end(), readModeName.begin(), ::tolower);

    const auto printValidMode = [envVarName, envVarValue] ()
                                { std::cout << envVarName << ": using specified mode, " << envVarValue << std::endl; };

    if (readModeName == "normal") {
      readMode = FileReadMode::eNormal;
      printValidMode();
    }
    else if (readModeName == "mmap") {
      readMode = FileReadMode::eMmap;
      printValidMode();
    }
    else if (readModeName == "buffer") {
      readMode = FileReadMode::eBuffer;
      printValidMode();
    }
    else if (readModeName == "default") {
      printValidMode();
    }
    else {
      std::cout << envVarName << ": invalid mode '" << envVarValue << "' specified, using default." << std::endl;
      std::cout << "  Valid modes: normal, mmap, buffer, default (case insensitive)" << std::endl;
    }
  }

  return readMode;
}

// Returns the potentially user-specified file read mode.
FileReadMode GetSpecifiedFileReadMode()
{
  static const FileReadMode sReadMode = GetFileReadModeFromEnvVar();   // Use static to only call once.
  return sReadMode;
}

// Returns the actual read mode to use for the default, based on the previous code, which on linux varied
// according to the file's compression type.
FileReadMode GetDefaultFileReadModeForCompressionType(Imf::Compression compression)
{
  // WARNING:
  // This was written to return the mode equivalent to what the code before FileReadMode was added.
  // Don't change this to return anything else without refactoring the calling code, e.g. that which
  // closes the file opened by the Read op.
  #if defined(_WIN32)

  return FileReadMode::eNormal;

  #elif defined(__APPLE__)

  return FileReadMode::eNormal;

  #else

  return (compression != Imf::ZIPS_COMPRESSION) ? FileReadMode::eMmap : FileReadMode::eNormal;

  #endif
}

//-------------------------------------------------------------------------------------------------

// Convenience struct for passing around a file's compression type and whether it contains
// tiled image data, along with a flag indicating whether these members have been explicitly set.
struct CompressionAndHasTiles
{
  Imf::Compression compression { Imf::NUM_COMPRESSION_METHODS };
  bool hasTiles { false};
  bool unset { true };
};

//-------------------------------------------------------------------------------------------------

enum EdgeMode
{
  eEdgeMode_Plate,
  eEdgeMode_Edge,
  eEdgeMode_Repeat,
  eEdgeMode_Black,
  kNumEdgeModes
};

static const char* kEdgeModeLabels[kNumEdgeModes +1] =
{
  "plate detect",
  "edge detect",
  "repeat",
  "black",
  nullptr
};

using namespace DD::Image;

// This structure stores the channel, layer and view determined from an exr
// channel name.
class ChannelName
{
public:
  // Default constructor for std::map
  ChannelName() {}
  
  // Construct from prefixed exr channel name
  ChannelName(const char* name, const std::vector<std::string>& views)
  { 
    setFromPrefixedExrName(name, views);
  }
  
  /*! Convert the channel name from the exr file into a nuke name.
   */
  void setFromPrefixedExrName(const char* channelname, const std::vector<std::string>& views);
  std::string nukeChannelName() const;
  std::string view() const  { return _view; }
  bool hasLayer() const  { return !_layer.empty(); }
  bool hasView() const   { return !_view.empty(); }
  
private:
  std::string _chan;
  std::string _layer;
  std::string _view;
};

// CompressedScanline: used for storing and decompressing raw scan lines read from the exr file.
// Used when _stripeHeight is 1 (scan line rather than planar reads).
struct CompressedScanline
{
  // The maximum size for this compressed scan line, as read from the exr header.
  size_t _maxSizeInBytes;

  // Buffer to use for storing the compressed scan line, and the size of the buffer/compressed scan line.  
  // If _mmapedInputFile is false, then this will be a pointer into an allocation created by this class, into which the compressed OpenEXR data is loaded from file.
  // If _mmapedInputFile is true then this will be a pointer into the virtual address range of the mmap()-ed file on disk,
  // or with FileReadMode::eBuffer a system memory buffer containing the file contents.
  char *_dataBuffer;
  int _dataSize;

  // Compressor to use for decompressing this scan line.
  Imf::Compressor *_decompressor;

  // The compression format that was used to decompress the scan line, as read from the exr header.
  Imf::Compressor::Format _format;

  // Is this file mmap()-ed into virtual address space or copied into a system memory buffer? Was provided in the constructor.
  bool _mmapedInputFile;

  // A pointer to the uncompressed data for this scanline, and the size of the scan line after uncompression.
  // (The memory this points to is not owned by the scanline but will be allocated and destroyed by the compressor.) 
  const char *_uncompressedDataPtr;
  int _uncompressedSize;

  CompressedScanline()
    : _maxSizeInBytes(0)
    , _dataBuffer(nullptr)
    , _dataSize(0)
    , _decompressor(nullptr)
    , _format(Imf::Compressor::XDR)
    , _mmapedInputFile(false)
    , _uncompressedDataPtr(nullptr)
    , _uncompressedSize(0)
  {
    mFnAssert("CompressedScanline constructed without an exr header or maximum data size."); 
  }

  CompressedScanline(size_t maxSizeInBytes,
                     const Imf::Header &hdr,
                     bool mmapedInputFile)
    : _maxSizeInBytes(maxSizeInBytes)
    , _dataBuffer(nullptr)
    , _dataSize(0)
    , _format(Imf::Compressor::XDR)
    , _mmapedInputFile(mmapedInputFile)
    , _uncompressedDataPtr(nullptr)
    , _uncompressedSize(0)
  {
    _decompressor = Imf::newCompressor(hdr.compression(), _maxSizeInBytes, hdr);

    // If this is not a mmap()-ed file we must allocate some memory to hold the uncompressed scanline in RAM. If we are using mmap, there is no need for this allocation.
    if(_mmapedInputFile == false) {
      _dataBuffer = new char[_maxSizeInBytes];
    }
  }

  ~CompressedScanline()
  {
    // If this is a mmap()-ed file: we earlier allocated some memory to store the uncompressed scanline in RAM. We must now release that allocation.
    if(_mmapedInputFile == false && _dataBuffer != nullptr) {
      delete [] _dataBuffer;
    }

    delete _decompressor;
  }

  // Uncompress this scan line. A pointer to the uncompressed data will
  // be stored in _uncompressedDataPtr, and its size in _uncompressedSize.
  int uncompress(int uncompressedSize, int exrY)
  {
    if (!_dataBuffer || _dataSize == 0)
      return 0;

    if (_dataSize < uncompressedSize) {
      _uncompressedSize = _decompressor->uncompress(_dataBuffer,
                                                    _dataSize,
                                                    exrY,
                                                    _uncompressedDataPtr);

      _format = _decompressor->format();
    }
    else {
      _uncompressedDataPtr = _dataBuffer;
      _uncompressedSize = _dataSize;
      _format = Imf::Compressor::XDR;
    }

    return _uncompressedSize;
  }  

  // Get the compression format for this scan line.
  const Imf::Compressor::Format & format() const { return _format; }
};

// CompressedScanlineBuffer: used for storing and decompressing multiple exr scan lines in parallel
// when the exr format is ZIPS_COMPRESSION (ZIP-compressed scan lines).
class CompressedScanlineBuffer 
{

public:
  CompressedScanlineBuffer(const Imf::Header &hdr, bool mmapedInputFile)
    : _mmapedInputFile(mmapedInputFile)
    , _header(&hdr)
    , _compressedScanlines()
    , _lineSizeInBytes(0)
  {
    // Read the size of each line in the file from the header and store in _lineSizeInBytes.
    // Also store the maximum line size in bytes; this is the size of buffer we will allocate
    // for storing each scanline (so that the buffers can be reused rather than allocated for
    // each new scan line).
    _maxSizeInBytes = Imf::bytesPerLineTable(*_header, _lineSizeInBytes);
  }

  ~CompressedScanlineBuffer()
  {
    clearAll();
  }

  // Read a raw scanline from the input part and store in the CompressedScanlineBuffer.
  CompressedScanline *readRawScanlineFromFile(Imf::InputPart &inputPart, int exrY);

  // Copy the raw scan line previously stored by this thread into the frame buffer, uncompressing it first if necessary.
  bool copyScanlineToFrameBuffer(CompressedScanline *scanlinePtr, const Imf::FrameBuffer &frameBuffer, int exrY);
  

private:
  // Is this file mmap()-ed into virtual address space? Was provided in the constructor.
  bool _mmapedInputFile;

  // The EXR header for the file these scan lines will be read from.
  const Imf::Header *_header;

  // A std::map that will contain one CompressedScanline for each process ID that asks for one.
  std::map<my_thread_id_type, CompressedScanline *> _compressedScanlines;

  // The size in bytes of each scan line in the exr file, indexed by y - minY.
  std::vector<size_t> _lineSizeInBytes;

  // The maximum size in bytes of a compressed scan line from the exr file.
  size_t _maxSizeInBytes;

  // Delete all scan lines in the buffer.
  void clearAll();

   // Get a pointer to the scan line for thread t.
  CompressedScanline *getScanline(my_thread_id_type t);
};


void CompressedScanlineBuffer::clearAll()
{
  const std::map<my_thread_id_type, CompressedScanline *>::iterator endIt = _compressedScanlines.end();
  for (std::map<my_thread_id_type, CompressedScanline *>::iterator it = _compressedScanlines.begin(); it != endIt; it++) {
    delete it->second;
    it->second = nullptr;
  }
  _compressedScanlines.clear();
}

// Get a pointer to the scan line for thread t. If this thread hasn't asked for a scan line
// before, this will allocate a new one.
CompressedScanline *CompressedScanlineBuffer::getScanline(my_thread_id_type t)
{
  std::map<my_thread_id_type, CompressedScanline *>::iterator it = _compressedScanlines.find(t);
  if (it != _compressedScanlines.end())
    return it->second;
  else {
    CompressedScanline *scanline = new CompressedScanline(_maxSizeInBytes, *_header, _mmapedInputFile);
    _compressedScanlines.insert(std::make_pair(t, scanline));
    return scanline;
  }
}

// Read a raw scan line from the file, store it in the CompressedScanlineBuffer and return a 
// pointer to it.
CompressedScanline *CompressedScanlineBuffer::readRawScanlineFromFile(Imf::InputPart &inputPart, int exrY)
{
  const my_thread_id_type id = getThreadID();
  CompressedScanline *scanlinePtr = getScanline(id);

  if(_mmapedInputFile == false) {
    // If this is a mmap()-ed file then scanlinePtr->_dataBuffer is an allocation that CompressedScanline created:
    // we need to tell the OpenEXR library to copy the compressed data from the IStream or file into this allocation.
    inputPart.rawPixelDataToBuffer(exrY, scanlinePtr->_dataBuffer, scanlinePtr->_dataSize);
  }
  else {
    // If this is not a mmap()-ed file: scanlinePtr->_dataBuffer does not point to an allocation that we created
    // and we'll ask the OpenEXR library to place the virtual address of the mmap()-ed memory into it, so we can decompress the data directly from the mapped file's virtual address range.
    inputPart.rawPixelData(exrY, (const char*&)scanlinePtr->_dataBuffer, scanlinePtr->_dataSize);
  }

  return scanlinePtr;
}

// copyScanlineToFrameBuffer: copy scan line into the frame buffer, uncompressing it if necessary.
bool CompressedScanlineBuffer::copyScanlineToFrameBuffer(CompressedScanline *scanlinePtr, const Imf::FrameBuffer &frameBuffer, int exrY)
{
  const IMATH_NAMESPACE::Box2i &dataWindow = _header->dataWindow();
  const int minX = dataWindow.min.x;
  const int maxX = dataWindow.max.x;
  const int minY = dataWindow.min.y;

  scanlinePtr->uncompress((int)_lineSizeInBytes[exrY - minY], exrY);
    
  //
  // Convert one scan line's worth of pixel data back
  // from the machine-independent representation, and
  // store the result in the frame buffer.
  //    
  const char *readPtr = scanlinePtr->_uncompressedDataPtr;
   
  //
  // Iterate over all image channels.
  //
    
  const Imf::ChannelList & channels = _header->channels();    

  Imf::ChannelList::ConstIterator imgChannel = channels.begin();
  const Imf::ChannelList::ConstIterator imgChannelsEnd = channels.end();
  const Imf::FrameBuffer::ConstIterator frameBufferEnd = frameBuffer.end();
  for (Imf::FrameBuffer::ConstIterator frameBufferSlice = frameBuffer.begin(); frameBufferSlice != frameBufferEnd; ++frameBufferSlice) {

    const Imf::Slice &slice = frameBufferSlice.slice();
     
    //
    // Find the x coordinates of the leftmost and rightmost
    // sampled pixels (i.e. pixels within the data window
    // for which x % xSampling == 0).
    //
     
    int dMinX = IMATH_NAMESPACE::divp(minX, slice.xSampling);
    int dMaxX = IMATH_NAMESPACE::divp(maxX, slice.xSampling);

    while (imgChannel != imgChannelsEnd && strcmp (imgChannel.name(), frameBufferSlice.name()) < 0) {
      // The frame buffer contains no slice for this channel - skip it.
      skipChannel (readPtr, imgChannel.channel().type, dMaxX - dMinX + 1);

      ++imgChannel;
    }

    bool fill = false;
    if (imgChannel == imgChannelsEnd || strcmp (imgChannel.name(), frameBufferSlice.name()) > 0) {
      //
      // We have reached the end of the available image channels in the file, or reached a channel
      // that sorts after the frame buffer channel in the alphabet. Since image channels are 
      // stored alphabetically in the file, and frame buffer channels are also sorted alphabetically 
      // by channel name, this means the frame buffer channel is not present in the file.
      //
      // In the frame buffer, slice frameBufferSlice will be filled with a default value.
      //
       
      fill = true;
    }
 
    //
    // Always add the slice if "fill" is true; otherwise, test if scan line y of this channel 
    // contains any data and skip if not (the scan line contains data only if y % ySampling == 0).
    //
    if (fill || IMATH_NAMESPACE::modp(exrY, imgChannel.channel().ySampling) == 0) {
       
      char *linePtr  = slice.base +
        IMATH_NAMESPACE::divp(exrY, slice.ySampling) *
        slice.yStride;
       
      char *writePtr = linePtr + dMinX * slice.xStride;
      char *endPtr   = linePtr + dMaxX * slice.xStride;
       
      Imf::copyIntoFrameBuffer (readPtr, 
                                        writePtr,
                                        endPtr,
                                        slice.xStride,
                                        fill,
                                        slice.fillValue,
                                        scanlinePtr->format(),
                                        slice.type,
                                        fill ? slice.type : imgChannel.channel().type);

      // Move to the next image channel in the file if we're not already at the end, and 
      // if the current frame buffer channel exists in the image (i.e. "fill" is false).
      if (!fill)
        ++imgChannel;
    }
    
  } //next slice in frame buffer

  return true;
}

class exrReaderFormat : public ReaderFormat
{

  friend class exrReader;

  bool _offset_negative_display_window;
  bool _doNotAttachPrefix;
  int  _edgeMode;
  bool _alwaysIgnorePartNames;

public:

  bool offset_negative_display_window() const
  {
    return _offset_negative_display_window;
  }

  bool doNotAttachPrefix() const
  {
    return _doNotAttachPrefix;
  }
  
  int edgeMode() const
  {
    return _edgeMode;
  }

  bool alwaysIgnorePartNames() const
  {
    return _alwaysIgnorePartNames;
  }

  exrReaderFormat()
  {
    _offset_negative_display_window = true;
    _doNotAttachPrefix = false;
    _edgeMode = eEdgeMode_Plate;
    _alwaysIgnorePartNames = false;
  }

  void knobs(Knob_Callback c) override
  {
    Bool_knob(c, &_offset_negative_display_window, "offset_negative_display_window", "offset negative display window");
    Tooltip(c, "EXR allows the 'display window' to have lower left corner at any position. "
               "Nuke's format does not support this, all formats must have lower left corner at 0,0. "
               "If this file does not have its left edge at x=0 Nuke can either offset the data and "
               "display windows so that it does, or treat the negative area as overscan and shrink "
               "the format by that amount all around.<br>"
               "If this option is enabled, Nuke will offset the image so that the display window "
               "left side is at x=0.<br>"
               "If it is disabled, the format will be shrunk on both sides by the amount that is "
               "negative in x as if that area was overscan.");

    Obsolete_knob(c, "disable_mmap", nullptr);

    Bool_knob(c, &_doNotAttachPrefix, "noprefix", "do not attach prefix");
    Tooltip(c, "By default the 'exr' prefix is attached to metadata keys to make it distinct from other metadata in the tree.  Enable this option to read the metadata 'as is' without attaching the exr prefix.");

    Enumeration_knob(c, &_edgeMode, kEdgeModeLabels, "edge_pixels", "edge pixels");
    Tooltip(c, "How to treat the edges of the data window.  Edge pixels are repeated or black is used."
               "<ul>"
               "<li><b>plate detect:</b> if the data and display windows match exactly then repeat all edges otherwise use black</li>"
               "<li><b>edge detect:</b> for each matching window edge repeat edge pixels, use black for mismatching edges</li>"
               "<li><b>repeat:</b> always repeat edge pixels outside the data window</li>"
               "<li><b>black:</b> always add black pixels outside the data window</li>"
               "</ul>");
    
    Bool_knob(c, &_alwaysIgnorePartNames, "ignore_part_names", "ignore part names");
    Tooltip(c, "Older versions of Nuke just stored the layer name in the part "
               "name of multi-part files. Nuke automatically detects legacy "
               "files. Check this option to force the new behaviour and ignore "
               "the part names.");
    SetFlags(c, Knob::INVISIBLE);
  }

  void append(Hash& hash) override
  {
    hash.append(_offset_negative_display_window);
    hash.append(_doNotAttachPrefix);
    hash.append(_edgeMode);
    hash.append(_alwaysIgnorePartNames);
  }
};

class exrReader : public Reader
{
public:

  // The fd, preReadBuffer and preReadBufferSize arguments are those passed to the plugin's build
  // function by the owning Read op.
  exrReader(Read*, int fd, const unsigned char* preReadBuffer, int preReadBufferSize);
  ~exrReader() override;

  /**
   * implementation of function from Reader
   */
  PlanarPreference planarPreference() const override
  {
    if (_neverPlanarInEnv) 
      return ePlanarNever;
    
    /* "offset negative display window" is active; which
       the planar case currently can't deal with */
    if (dataOffset != 0) {
      return ePlanarNever;
    }

    /* mandate planar if _stripeHeight > 1 (ie we would have to decompress
       multiple lines at a time anyway */
    if (_stripeHeight > 1)
      return ePlanarAlways;
    
    /*
     * otherwise deny planar access
     */
    return ePlanarNever;
  }

  /**
   * always use stripes for planar access to exrs
   */
  bool useStripes() const override
  {
    return true;
  }

  /**
   * return the stripeHeight, kept in _stripeHeight
   */
  size_t stripeHeight() const override
  {
    return _stripeHeight;
  }

  const MetaData::Bundle& fetchMetaData(const char* key) override;

  /**
   * convert a y value from Nuke coordinates (0 = bottom) to EXR coordinate (0 = top)
   */
  int convertY(int y) const
  {
    const Imath::Box2i& dispwin = inputfile->header(0).displayWindow();
    return dispwin.max.y - y;
  }

  void engine_inner(const Imath::Box2i& datawin,
                     const Imath::Box2i& dispwin,
                     const ChannelSet&   channels,
                     int                 exrY,
                     Row&                row,
                     int                 x,
                     int                 X,
                     int                 r,
                     int                 R);


  /**
   * go over the parts and channels, and figure out which parts are interesting
   * and which channels want copying into other channels
   */
  void processChannels(const DD::Image::ChannelSet& channels, std::set<int>& partSet, std::map<Channel, Channel>& toCopy);
  
  PlanarI::PlaneID getPlaneFromChannel(Channel chan) override;

  void fetchPlane(ImagePlane& imagePlane) override;

  void engine(int y, int x, int r, ChannelMask, Row &) override;
  void open() override;
  void _validate(bool for_real);
  static const Description d;

  bool supports_stereo() const override
  {
    return true;
  }

  bool fileStereo() const override
  {
    return fileStereo_;
  }

  void lookupChannels(std::set<Channel>& channel, const char* name)
  {
    if (strcmp(name, "y") == 0 || strcmp(name, "Y") == 0) {
      channel.insert(Chan_Red);
      if (!iop->raw()) {
        channel.insert(Chan_Green);
        channel.insert(Chan_Blue);
      }
    }
    else {

      // if this channel does exist return early
      if(DD::Image::findChannel(name) != Chan_Black) {
        channel.insert(Reader::channel(name));
        return;
      }

      // Try to add channel. If this channel exceeds Chan_Last then Chan_Black is returned instead
      channel.insert(Reader::channel(name));

      // this code should be a temporary meassure until it is moved to a more general location
      // so that it can catch other readers attempting to load channels
      // we only want to show a single warning per session.
      static bool alerted = 0;
      const unsigned int channelCountPreference = DD::Image::GetUIntPreference(DD::Image::kChannelWarningThreshold);
      const unsigned int channelCount = DD::Image::getChannelCount();
      if(alerted &&  channelCount < channelCountPreference) {
        alerted = false;
      }

      if( channelCountPreference > 0 && channelCount == channelCountPreference) {
        // if channelCountPreference is Chan_Last, then channel count will never be > Chan_last,
        // so channelCount == channelCountPreference will be true when we hit 1024 channels;
        // if channelCountPreference is Chan_Last, then we need to check using a static assert.
        if(!alerted) {
          alerted = true;
          std::stringstream ss;
          ss << "Nuke has reached the channel warning preference of  " << channelCountPreference << " channels." << std::endl;
          Op::message_f('!', ss.str().c_str());
        }
      }

      // Raising an error is now controlled by this environment variable
      const char* const  errorOnChanMax = std::getenv("NUKE_EXR_CHAN_ERROR");
      if(errorOnChanMax && !std::strcmp(errorOnChanMax , "1")) {
        if(channelCount == Chan_Last) {
          std::stringstream ss;
          ss << "Nuke has exceeded max channel limit " << DD::Image::Chan_Last << " channels";
          iop->error(ss.str().c_str());
        }
      }
    }
  }

  // Get the nuke channels if present in this view.
  bool getChannels(const ExrChannelNameToNuke& channelName, const std::string& viewName, std::set<Channel>& channels)
  {
    // Channel lookups are not threadsafe because they can result in new channels being added to Nuke's static allChannels list.
    Guard guard(sAllChannelsLock);

    // Start with an empty channel set.
    channels.clear();
    
    std::string viewpart = (channelName.view().length() > 0) ? channelName.view() : heroview;
    std::string otherpart = channelName.nukeChannelName();

    bool gotChannels = false;
    if (viewName == viewpart) {
      // Perfect match, this is the priority view for this channel.
      fileStereo_ = true;
      lookupChannels(channels, otherpart.c_str());
      gotChannels = true;
    }
    // The view for this channel does not match the current view.
    // Allow copying if this is the hero view or there is no associated view.
    else if (viewpart == "" || viewpart == heroview) {
      // Find the nuke channels with which this is associated.
      lookupChannels(channels, otherpart.c_str());
      gotChannels = true;
    }
    
    return gotChannels;
  }

private:

  // Store the name and part number in the channel map
  struct ChannelInfo 
  {
    // Default constructor for std::map
    ChannelInfo() : name(nullptr), part(0) {}
    
    // Construct using exr channel name
    ChannelInfo(const char* n, int p, ExrChannelNameToNuke ch) :
      name(n), part(p), channelName(ch) {}
    
    bool operator<(const ChannelInfo& rhs) const
    {
      // Sort first by part
      if (part < rhs.part)
        return true;
      if (rhs.part < part)
        return false;
      
      // Then by name
      return strcmp(name, rhs.name) < 0;
    }
  
    // Comparison operator to decide on the best match
    bool isBetterThan(const ChannelInfo& rhs, const std::string& view) const;

    const char* name;
    int         part;
    ExrChannelNameToNuke channelName;
  };

  // Creates and sets up our IStream member, inputFileStream, for use as the source stream when creating our
  // MultiPartInputFile, inputfile. May also create our std::ifstream, inputStream.
  //
  // This takes the potentially user-specified FileReadMode into account, and depending on the mode may need to
  // create a temporary MultiPartInputFile to fetch the file's compression type and whether it has tiled image
  // data. If it does need to fetch that info then it populates the passed CompressionAndHasTiles so subsequent
  // code doesn't need to. Subsequent code should check the compressionAndHasTiles.unset to see whether it
  // still needs to fetch the info.
  //
  // Takes the file descriptor and any pre-read buffer that's been passed into the exrReader's constructor
  // from the owning Read op. If the FileReadMode is eBuffer this open file will be used to read into
  // the memory buffer.
  //
  // In all cases the function closes the fd file handle - do not attempt to use it
  // subsequently in any way.
  void setupInputFileStream(int fd, const unsigned char* preReadBuffer, int preReadBufferSize,
                            CompressionAndHasTiles& compressionAndHasTiles);

  // Whether the compression blocks for a particular compression type cover multiple scanlines or not.
  // If this is true, it's more efficient to do planar reads when possible and make use of the exr 
  // library's internal multi-threading.
  bool compressionBlocksSpanMultipleScanlines(const Imf::Compression &compressionType);

  // The stripe height to use for planar reads (ZIP and PIZ compressed).
  int stripeHeightForCompression(const Imf::Compression& compressionType);

  /// Determine whether or not the alpha is needed to produce our output (for pre-multiplication).
  bool needsAlpha(const ChannelSet& channelsToFill);

  OPENEXR_IMF_NAMESPACE::IStream* inputFileStream;
  Imf::MultiPartInputFile* inputfile;
  std::ifstream* inputStream;

  Lock C_lock;
  static Lock sExrLibraryLock;
  static Lock sAllChannelsLock;
  std::unique_ptr<Imf::InputPart> _inputPart0;

  std::map<Channel, ChannelInfo> channel_map;
  bool fileStereo_;
  std::vector<std::string> views;
  std::string heroview;
  // dataOffset is used to deal with the case where the display window goes
  // not have left side at 0. Both display and data can be shifted
  // over or display can be shrunk as if a negative area is overscan.
  int dataOffset;

  MetaData::Bundle _meta;

  size_t _stripeHeight;
  
  std::map<int, ChannelSet> _partSets;

  // The value of the NUKE_EXR_NEVER_PLANAR environment variable will be stored here on construction,
  // as it's expensive to check this on Windows every time planarPreference is called (bug 41471).
  const bool _neverPlanarInEnv;


  // Buffers for storing raw scanlines, to enable multiple scan lines to be decompressed in 
  // parallel by multiple engine threads. We have one buffer for each input part.
  std::vector<CompressedScanlineBuffer *> _compressedScanlineBuffers;
  // Whether to read raw scanlines from the file, which is only possible when the file type 
  // is "scanlineimage" and we are reading a single scan line at a time. We only use the 
  // CompressedScanlineBuffer above when _readRawScanlines is true.
  bool _readRawScanlines;


  //! Gets information about the ideal planar image (bounding box, data type, actual channels, etc) for reading/decoding the
  //! specified channels, overriding the default implementation in Reader. The information in the returned PlanarReadInfo
  //! is guidance for the user code, which may legitimately decide to use different settings when actually reading/decoding,
  //! though any deviation from the returned settings must be done with caution - see the documentation of planarReadAndDecode()
  //! and planarDecodePass() for more details.
  //! Note that the returned PlanarReadInfo will always report the decode is NOT threadable (by the user). This is because
  //! internally the read/decode is handles by the IlmImf library, which provides its own multithreading support, and trying
  //! to multithread at a higher level will in general be less optimal. Also because of the use of IlmImf, the read and
  //! decode passes aren't distinct, so the reported read pass buffer size is zero.
  PlanarReadInfo planarReadInfo(const ChannelSet& channels) override
  {
    // Specify the bounds rather than format, i.e., the data window rather than display window.
    // It's up to the caller to decide whether that's appropriate, it may want to set the bounds in
    // the passed GenericImagePlane to match the format (or some other range if it really wants).
    // NOTE: If the data and display windows don't match then the constructor will have set the
    // (bbox) bounds to be the data window plus a single pixel border.
    DD::Image::Box bounds(x(), y(), r(), t());

    bool packed = true;

    ChannelSet commonChannels = channels;
    commonChannels &= this->channels();

    // Initially assume half float data, then check the type of the channels of interest. If any have a
    // greater bit depth use that type, giving priority to float over uint.
    DataTypeEnum dataType = eDataTypeFloat16;
    foreach(ddiChannel, commonChannels) {
      const Imf::Channel& imfChannel = inputfile->header(channel_map[ddiChannel].part).channels()[channel_map[ddiChannel].name];
      if (imfChannel.type == Imf::FLOAT) {
        dataType = eDataTypeFloat32;
        break;  // Give 32 bit float priority.
      }
      else if (imfChannel.type == Imf::UINT) {
        dataType = eDataTypeUInt32;
      }
    }

    bool useClamps = false;
    int minValue = 0;
    int maxValue = 1;
    int whitePoint = 1;

    ImagePlaneDescriptor baseDesc(bounds, packed, commonChannels, commonChannels.size());
    DataInfo dataInfo(dataType, useClamps, minValue, maxValue, whitePoint);

    ColorCurveEnum colorCurve = eColorCurveLinear;  // #rick: Is this right? I can't find anything about colour encoding.

    GenericImagePlaneDescriptor desc(baseDesc, dataInfo, colorCurve);

    // As we're using the IlmImf library there's no separate read and decode passes, so the user code
    // shouldn't need to allocate a read buffer.
    size_t readPassBufferSize = 0;

    // IlmImf can handle multithreading internally. It's thread safe but reading/decoding from separate
    // user threads may not provide much benefit due to IlmImf's internal locks (that guarantee the thread
    // safety). So, report that the decode is non-threadable, in terms of planarDecodePass().
    // To get the IlmImf readPixels() to do internal multithreading we need to have called Imf::setGlobalThreadCount()
    // *before* constructing the Imf::InputFile (assuming it's constructed using the default second argument of
    // Imf::globalThreadCount()), which we leave to the user code as the exrReader can't know what's appropriate
    // for the application.
    bool isDecodeThreadable = false;

    const bool isValid = true;

    PlanarReadInfo planarInfo(desc, readPassBufferSize, isDecodeThreadable, isValid);

    return planarInfo;
  }

  //! Planar read and decode the specified channels of the image in one go, overriding the default implementation in Reader.
  //! Since for EXR reading the decode isn't threadable at the user level (see planarReadInfo()), it's simplest for user code
  //! to call this function. This function calls planarDecodePass(), so has the same restrictions as that function.
  void planarReadAndDecode(GenericImagePlane& image, const ChannelSet& channels, int priority) override
  {
    // We're using the IlmImf so there's no concept of separate reading and decoding and all the work is in our
    // planarDecodePass() function.
    planarDecodePass(nullptr, image, channels, 0, 1, priority);
  }

  //! The EXR implementation for the read pass does nothing - the arguments are ignored and 0 is always returned.
  //! The user can call planarReadPass() followed by planarDecodePass() but it's simpler to just call planarReadAndDecode().
  int planarReadPass(void* buffer, const ChannelSet& channels) override
  {
    return 0;
  }

  // Helper to map the specified Channel to the appropriate name used within the EXR file.
  // If there's no entry in the map this returns a dummy channel name that we never expect to find
  // in a file so that the subsequent OpenEXR read will use our chosen fill value for this channel.
  // Also, if there is no entry the function asserts that it's the alpha channel that's specified
  // and rgbToRgbA is true - that's a special case we want to allow and anything else is the
  // almost certainly an error.
  //
  // NOTE: The returned string address is (usually) from an entry in the channel_map member. It should therefore
  // only be used if channel_map isn't modified after the call to getExrChannelName. In practice that won't
  // be a problem as channel_map is set-up in the constructor.
  const char* getExrChannelName(Channel channel, bool rgbToRgbA) const
  {
    // Initialise the channel name to one that won't be in the file.
    const char* exrChannelName = "Nuke_exrReader_DummyChannel"; // TODO - set programmitcally to known non-existent channel.

    std::map<Channel, ChannelInfo>::const_iterator channelMapIter = channel_map.find(channel);

    if (channelMapIter != channel_map.end() && channelMapIter->second.name != nullptr) {
      exrChannelName = channelMapIter->second.name;
    }
    else {
      std::ostringstream debugMsg;
      debugMsg << "No entry for channel " << channel << " in channel_map and rgbToRgbA is false." << std::endl;
      mFnAssertMsg((channel == Chan_Alpha) && rgbToRgbA, debugMsg.str().c_str());
    }

    return exrChannelName;
  }

  //! Decodes the specified channels into the specified image plane. The specified channels must all be present in the EXR (this
  //! can be determined from calls to planarReadInfo()).
  //! Also, the bounds of the image plane must contain (or match) the data window of the EXR, which is reported as the bounds
  //! in the PlanarReadInfo returned by planarReadInfo(). If only a part of the data window is required then explicitly call
  //! the base class Reader::planarReadAndDecode(), which can handle that use case at the expense of performance.
  //! Since there's no distinct read pass, srcBuffer is ignored. Also, since user level multithreading is not supported
  //! threadIndex and nDecodeThreads are ignored, though to detect possible user error, in debug builds they are asserted to
  //! be 0 and 1, respectively.
  void planarDecodePass(void* srcBuffer, GenericImagePlane& image, const ChannelSet& channels,
                                int threadIndex, int nDecodeThreads, int priority) override
  {
    assert(threadIndex == 0); // We don't allow user-level multithreading.
    assert(threadIndex < nDecodeThreads);
    assert(nDecodeThreads == 1);

    // Whether we're requesting RGBA but the file only has RGB (and possibly other channels we don't care about).
    const bool rgbToRgbA = ((channels == Mask_RGBA) && !this->channels().contains(Mask_Alpha) && this->channels().contains(Mask_RGB));

    // Require all the channels requested to exist in the source file, with the useful exception of RGB to RGBA.
    // The user should detect this after the call to planarReadInfo() and be using Reader::planarReadAndDecode()
    // instead of this code path for other cases.
    if (!this->channels().contains(channels) && !rgbToRgbA) {
      assert(0);
      return;
    }

    const Imath::Box2i& displayWindow = inputfile->header(0).displayWindow();
    const Imath::Box2i& dataWindow = inputfile->header(0).dataWindow();

    // #rick:
    // The code should be extended to handle the combination of a negatively offset display window and
    // the offset option not enabled.
    exrReaderFormat* readerFormat = dynamic_cast<exrReaderFormat*>(iop->handler());
    const bool mFnUnusedVariable offsetNegativeDisplayWindow = readerFormat && readerFormat->offset_negative_display_window();
    assert(offsetNegativeDisplayWindow || (displayWindow.min.x >= 0));

    // #rick:
    // If this assert fails then either offsetNegativeDisplayWindow is not true (and the display window
    // origin is offset horizontally), which is currently excluded by the offsetNegativeDisplayWindow assert
    // above, or the detatails of the meaning of dataOffset have changed in some way.
    assert(dataOffset == -displayWindow.min.x);
    // Get the bounds of the image plane we're being asked to fill in, in the DDImage rather than OpenEXR
    // coordinate space (see later). We subtract 1 from r and t to get actual pixel coordinates, since r and
    // t represent the outer edges but OpenEXR deals in pixel coordinates (e.g., a bounds at x = 0 with a
    // width of 100 has r = 100, rather than the upper pixel coordinate 99).
    int imageMinX = image.desc().bounds().x();
    int imageMinY = image.desc().bounds().y();
    int imageMaxX = image.desc().bounds().r() - 1;
    int imageMaxY = image.desc().bounds().t() - 1;

    // The DDImage code (or is it Nuke?) requires the format, equivalent to OpenEXR's display window, to
    // have its minimum at (0, 0). In addition, the DDImage origin is at the lower left of the image but the
    // OpenEXR origin is at the upper left, which we must take into accoutn when converting the y bounds
    // into OpenEXR coordinates.
    const int xOffset = -displayWindow.min.x;
    int imfMinX = imageMinX - xOffset;
    int imfMinY = displayWindow.max.y - imageMaxY;
    int imfMaxX = imageMaxX - xOffset;
    int imfMaxY = displayWindow.max.y - imageMinY;

    // If the file's data window is partly outside the the destination generic image then we can't use the
    // optimised read as the call to Imf::InputFile::readPixels() would write beyond the edges of the image plane
    // buffer - there's no way to read less than a whole line of the data.
    // We don't just check thet width and height as the data window can be offset, an offset we're including
    // when writing to the destination image (complicated by the DDImage coordinates being potentially offset).
    // We can't just fallback here on Reader::readAndDecode() here because that's hardwired to decode to a buffer of floats
    // and our image data type may be different. So, we have to classify this as a user error - in this cases it's up to the
    // calling code to check the sizes of the bounds (data) and format (display) and use the default Reader implementation
    // of planarReadInfo() and planarReadAndDecode().
    // (This test assumes min.x <= max.x, and similarly for y.)
    if ((dataWindow.min.x < imfMinX) ||
        (dataWindow.max.x > imfMaxX) ||
        (dataWindow.min.y < imfMinY) ||
        (dataWindow.max.y > imfMaxY)) {
      assert(0);  // The user shouldn't be calling this function.
      return;
    }

    // If the data window doesn't cover the destination image then clear the whole destination image buffer
    // first. This does mean a lot of pixels may get overwritten when we write the actual pixel data but it avoids the
    // potentially costly nested loops and checks that would be required to clear only those pixels and channels srtictly
    // necessary.
    // TODO: Is this really best? The memset will result in a loop anyway. For big images with small 'excess'
    // it might be much better to only clear what we need, since that would involve touching much less memory.
    if ((imfMinX < dataWindow.min.x) ||
        (imfMaxX > dataWindow.max.x) ||
        (imfMinY < dataWindow.min.y) ||
        (imfMaxY > dataWindow.max.y)) {
      image.clearImage();
    }

    // Set the pixel coordinates we'll need to determine the appropriate address in the destination tile
    // image into  which to place this line.
    // The critical thing to appreciate is that Imf::InputFile::readPixels(y) will read the whole y^th
    // line of the data window into the location
    //    base + x * xStride + y * yStride
    // where base and the strides are as set in the Imf::FrameBuffer, per channel. The x and y values
    // readPixels() uses are the actual OpenEXR coordinates. So, we need to specify the address 'in' the
    // tile image such that this offsetting results in the correct location. I say 'in' because the
    // specified address will not actually lie inside the tile image buffer if it doesn't happen to contain the pixel
    // corresponding to the EXR origin (and since, in EXR coordinates, both the data and display windows can have
    // arbitrary positions the (EXR) origin may not be inside either anyway).
    //
    // The coordinates used for the tile buffer will always have the origin at the lower left corner of the
    // display window with y increasing upwards, since that's how Nuke handles formats. However, in EXR coordinates
    // not only can the 'min' of the display window be at any position (not just (0,0)) but it's at the top
    // left and with y increasing downwards.
    // So, the x coordinate of the EXR origin, expressed in DDImage coords, is -displayWindow.min.x.
    // Since the format matches the data window, just with a bottom (left) origin, the EXR's origin will be at
    // displayWindow.max.y, i.e. the distance from the EXR origin to the bottom of the display window (remembering
    // that y increases downwards in EXR coords). Since we store lower rows in lower addresses (as makes sense for y-up)
    // we also need to specify a negative y stride - thus we do the vertical flip in-place as we read from the file
    // into the destination tile's buffer.

    int baseIndexX = -displayWindow.min.x;
    int baseIndexY = displayWindow.max.y;
    size_t xStride = image.colStrideBytes();
    size_t yStride = -image.rowStrideBytes();

    // Trim the (translated) image coordinates to the data window.
    imfMinX = MAX(imfMinX, dataWindow.min.x);
    imfMinY = MAX(imfMinY, dataWindow.min.y);
    imfMaxX = MIN(imfMaxX, dataWindow.max.x);
    imfMaxY = MIN(imfMaxY, dataWindow.max.y);

    // Set the value to use to fill the alpha channel, if necessary.
    bool autoFill = iop->autoAlpha();
    unsigned int uintAlphaFill  = autoFill ? UINT_MAX : 0;
    half halfAlphaFill          = autoFill ? half(1.0f) : half(0.0f);
    float floatAlphaFill        = autoFill ? 1.0f : 0.0f;

    // Determine the parts to check
    std::set<int> partSet;
    foreach (z, channels) {
      partSet.insert(channel_map[z].part);
    }

    int channelCount = 0;

    // Get the channels in purpose order
    std::vector<Channel> orderedChannels;

    Channel ch = channels.first();

    while(ch != Chan_Black) {
      orderedChannels.push_back(ch);
      ch = channels.next(ch);
    }

    std::sort(orderedChannels.begin(), orderedChannels.end(), DD::Image::compareChannels);

    // Iterate through the parts (and frame buffers)
    std::set<int>::const_iterator it = partSet.begin();
    const std::set<int>::const_iterator end = partSet.end();
    for( ; it != end; ++it)
    {
      int part = *it;

      // Create a framebuffer for this part
      Imf::FrameBuffer frameBuffer;

      for(std::vector<Channel>::const_iterator itChannel = orderedChannels.begin(); itChannel != orderedChannels.end(); ++itChannel) {
        Channel ch = *itChannel;

        const bool isAlpha = (ch == Chan_Alpha);

        // Filter to the current part
        if (channel_map[ch].part != part) {
          // Don't skip the alpha channel for the first part if it's missing
          if (!isAlpha || part != *partSet.begin() || channel_map.find(Chan_Alpha) != channel_map.end()) {
            continue;
          }
        }

        const char* exrChannelName = getExrChannelName(ch, rgbToRgbA);

        const int xSampling = 1;
        const int ySampling = 1;


        if (image.desc().dataInfo().dataType() == eDataTypeUInt32) {
          char* baseAddr = reinterpret_cast<char*>(&image.writableAt<unsigned int>(baseIndexX, baseIndexY, channelCount));
          frameBuffer.insert(exrChannelName, Imf::Slice(Imf::UINT, baseAddr, xStride, yStride,
                                                                xSampling, ySampling, isAlpha ? uintAlphaFill : 0));
        }
        else if (image.desc().dataInfo().dataType() == eDataTypeFloat16) {
          char* baseAddr = reinterpret_cast<char*>(&image.writableAt<half>(baseIndexX, baseIndexY, channelCount));
          frameBuffer.insert(exrChannelName, Imf::Slice(Imf::HALF, baseAddr, xStride, yStride,
                                                                xSampling, ySampling, isAlpha ? halfAlphaFill : half(0)));
        }
        else if (image.desc().dataInfo().dataType() == eDataTypeFloat32) {
          char* baseAddr = reinterpret_cast<char*>(&image.writableAt<float>(baseIndexX, baseIndexY, channelCount));
          frameBuffer.insert(exrChannelName, Imf::Slice(Imf::FLOAT, baseAddr, xStride, yStride,
                                                                xSampling, ySampling, isAlpha ? floatAlphaFill : 0));
        }
        else {
          assert(0);
          return;
        }

        channelCount++;
      }

      // The IlmImf lib handles errors by throwing exceptions so look out for that.
      try {
        // TODO: There are some scenarious where exrReader::open isn't getting called for timeline comps,
        //  i.e. renders read via a 'parent' nkReader. I've fixed one bug related to inconsistent abort/error
        //  states but this is still happening intermittently. There's clearly something going wrong elsewhere
        //  but for now we can avoid a crash by creating a local InputPart for part 0 if that hasn't already
        //  happened.
        const bool inputPart0Created = (_inputPart0.get() != nullptr);

        if (part == 0 && inputPart0Created) {  // open() should have created the InputPart for part 0.
          _inputPart0->setFrameBuffer(frameBuffer);
          _inputPart0->readPixels(imfMinY, imfMaxY, priority);;
        }
        else {
          Imf::InputPart inputPart(*inputfile, part);
          inputPart.setFrameBuffer(frameBuffer);
          inputPart.readPixels(imfMinY, imfMaxY, priority);
        }
      }
      catch (const Iex::BaseExc& exc) {
        iop->error(exc.what());
        std::cout << "exrReader exception: " << exc.what() << std::endl;
        return;
      }
    }
  }
};

Lock exrReader::sExrLibraryLock;
Lock exrReader::sAllChannelsLock;

static Reader* build(Read* iop, int fd, const unsigned char* b, int n)
{
  return new exrReader(iop, fd, b, n);
}

static ReaderFormat* buildformat(Read* iop)
{
  return new exrReaderFormat();
}

static bool test(int fd, const unsigned char* block, int length)
{
  return block[0] == 0x76 && block[1] == 0x2f &&
         block[2] == 0x31 && block[3] == 0x01;
}

const Reader::Description exrReader::d("exr\0sxr\0", build, test, buildformat);

const MetaData::Bundle& exrReader::fetchMetaData(const char* key)
{
  return _meta;
}

CompressionAndHasTiles GetCompressionAndHasTiles(const Imf::MultiPartInputFile& inputFile)
{
  return { inputFile.header(0).compression(), inputFile.header(0).hasTileDescription(), false };
}

// Whether the compression blocks for a particular compression type cover multiple scanlines or not.
// If they do, we will do planar reads when possible and make use of the exr library's internal multi-threading.
bool exrReader::compressionBlocksSpanMultipleScanlines(const Imf::Compression &compressionType)
{
  mFnAssertStatic(Imf::DWAB_COMPRESSION+1 == Imf::NUM_COMPRESSION_METHODS);
  return (compressionType == Imf::ZIP_COMPRESSION ||    // zlib compression, in blocks of 16 scan lines
          compressionType == Imf::PIZ_COMPRESSION ||    // piz-based wavelet compression
          compressionType == Imf::PXR24_COMPRESSION ||  // lossy 24-bit float compression (LW: this is not mentioned in the documentation, but it seems to be compressed in blocks of 16 scan lines)
          compressionType == Imf::B44_COMPRESSION ||    // lossy 4-by-4 pixel block compression, fixed compression rate
          compressionType == Imf::B44A_COMPRESSION ||   // lossy 4-by-4 pixel block compression, flat fields are compressed more
          compressionType == Imf::DWAA_COMPRESSION ||   // lossy DCT based compression, in blocks of 32 scanlines. More efficient for partial buffer access.
          compressionType == Imf::DWAB_COMPRESSION );   // lossy DCT based compression, in blocks of 256 scanlines. More efficient space wise and faster to decode full frames than DWAA_COMPRESSION.
}

int exrReader::stripeHeightForCompression( const Imf::Compression& compressionType )
{
  // Guard against zero-sized images  
#ifdef FN_OS_MAC
  return std::min(512, std::max(1, info_.h())); 
#else
  return std::max(1, info_.h());
#endif
}

bool exrReader::needsAlpha(const ChannelSet& channelsToFill) 
{
  // We need alpha for premultiplication if not using linear colour space, 
  // the channels we've been asked to fill are in the RGB layer and alpha
  // is amongst the channels this Reader can produce. 
  //
  // (Note: I'm not sure why the pre-multiplication is not done in the linear
  //        case - however this is the logic that was used previously in 
  //        engine, so I don't want to change it  -- Lucy.)
  return (premult() && !lut()->linear() &&
          (channelsToFill & Mask_RGB) && (this->channels () & Mask_Alpha));
}

exrReader::exrReader(Read* r, int fd, const unsigned char* preReadBuffer, int preReadBufferSize)
  : Reader(r)
  , inputFileStream(nullptr)
  , inputfile(nullptr)
  , inputStream(nullptr)
  , fileStereo_(false)
  , dataOffset(0)
  , _stripeHeight(0)
  , _neverPlanarInEnv(getenv("NUKE_EXR_NEVER_PLANAR"))
  , _compressedScanlineBuffers(0)
  , _readRawScanlines(false)
{
#ifdef ENABLE_EXR_INFO_TTY
  std::cout << "-------------------------------- EXR info --------------------------------" << std::endl;
  std::cout << "File: " << filename() << std::endl;
#endif  // ENABLE_EXR_INFO_TTY

  // Make it default to linear colorspace:
  lut_ = LUT::GetLut(LUT::FLOAT, this);

  std::map<Imf::PixelType, int> pixelTypes;
  bool doNotAttachPrefix  = false;

  _stripeHeight = 1;

  // Get the name of the view for which we're reading image data.
  const std::string viewName = OutputContext::viewName(iop->view_for_reader(), iop);

  try {

    // We defer fetching the compression type and tile status until either we have to or it's cheap to do so.
    // We want to avoid creating a temporary MultiPartInputFile just to get this info if possible.
    CompressionAndHasTiles compressionAndHasTiles;

    setupInputFileStream(fd, preReadBuffer, preReadBufferSize, compressionAndHasTiles);

    // Whether this reader is being used for the Hiero/Studio timeline (hence also the flipbook)
    // or the node graph viewer.
    const bool forTimeline = (dynamic_cast<TimelineRead*>(iop) != nullptr);

    // For the node graph, if we haven't already got the compression/tile info we need to do so now for determining
    // the thread count and whether to use planes.
    if (!forTimeline && compressionAndHasTiles.unset) {
      Imf::MultiPartInputFile tmpInputFile(*inputFileStream, 0);  // "0" here means don't multi-thread reads from this file object.
      compressionAndHasTiles = GetCompressionAndHasTiles(tmpInputFile);
      inputFileStream->seekg(0);  // Reset the stream ready for when we use it with the 'real' InputFile.
    }

    // Set the default number of threads to use for the read to zero (no multi-threading).
    unsigned int nThreadsToUseForRead = 0;

    // If the compression blocks cover multiple scanlines, or the image is stored in tiles which
    // span multiple scanlines, we will use the PlanarIop interface to read a block of scanlines 
    // from the file at a time. When doing this it will be more efficient to turn on multi-
    // threading in the exr library.
    // When reading scanlines there is no performance gain from using multiple threads.
    // If the tile/compression info isn't available we can't yet decide whether to use planes for reading.
    // But this can only apply to timeline usage and then only have any possible effect (due to thread count settings)
    // in the corner cases that the (timeline) calling code ends up falling back to using Reader::planarReadAndDecode
    // rather than our planarReadAndDecode.
    bool usePlanesForRead = !compressionAndHasTiles.unset && (compressionAndHasTiles.hasTiles ||
                                     compressionBlocksSpanMultipleScanlines(compressionAndHasTiles.compression));

    if (forTimeline) {
      // We assume the timeline client code has set the desired exr lib worker thread count.
      nThreadsToUseForRead = Imf::globalThreadCount();
    }
    else if (usePlanesForRead) {
      nThreadsToUseForRead = DD::Image::Thread::numThreads;

      // Turn on multi-threading in the library, with the global thread count set to the number of 
      // threads that Nuke is using. This enables us to use multi-threading for tiled (planar) reads.
      // The number of threads to use for a particular read can be set later when we open the file
      // for reading.
      static std::once_flag sOnceFlag;
      std::call_once(sOnceFlag,
        [nThreadsToUseForRead]() {
          Imf::setGlobalThreadCount(nThreadsToUseForRead);
        }
      );
    }

    inputfile = new Imf::MultiPartInputFile(*inputFileStream, nThreadsToUseForRead);

    // In some cases we may not have needed to fetch the compression/tile info "early" but can now
    // do so cheaply since we've finally created the 'proper' MultiPartInputFile.
    if (compressionAndHasTiles.unset) {
      compressionAndHasTiles = GetCompressionAndHasTiles(*inputfile);
      // Update usePlanesForRead now we have the tile/compression info - it's too late to affect the number
      // of OpenEXR threads to use, but does still affect other logic
      usePlanesForRead = compressionAndHasTiles.hasTiles ||
                             compressionBlocksSpanMultipleScanlines(compressionAndHasTiles.compression);
    }

      exrReaderFormat* trf = dynamic_cast<exrReaderFormat*>(r->handler());
      const bool offsetNegativeDisplayWindow = trf && trf->offset_negative_display_window();
      doNotAttachPrefix = trf && trf->doNotAttachPrefix();
      int edgeMode = eEdgeMode_Plate;  // Default mode for preview
      if (trf) {
        edgeMode = trf->edgeMode();
      }
      bool alwaysIgnorePartNames = false; // Default mode for preview
      if (trf) {
        alwaysIgnorePartNames = trf->alwaysIgnorePartNames();
      }
      
      // Metadata comes from the first part, though we'll also add the list of views later as these may be
      // spread over multiple parts.
      exrHeaderToMetadata( inputfile->header(0), _meta, doNotAttachPrefix );
      
      ChannelSet mask;
      const bool isMultipart = inputfile->parts() > 1;

      const Imath::Box2i& datawin = inputfile->header(0).dataWindow();
      const Imath::Box2i& dispwin = inputfile->header(0).displayWindow();
      double aspect = inputfile->header(0).pixelAspectRatio();
      
      Imath::Box2i formatwin(dispwin);

      // Nuke requires format to start at 0,0 but EXR does not. User has a
      // choice to shift both the data and display windows over by the negative
      // amount so that they still line up but are offset into positive are
      // OR treat that negative display window area as overscan and subtract it
      // from both sides.

      formatwin.min.x = 0;
      formatwin.min.y = 0;

      if (dispwin.min.x != 0) {
        if ( !offsetNegativeDisplayWindow && (dispwin.min.x < 0) ) {
          // Leave data where it is and shrink the format by the negative
          // amount on both sides so that it starts at (0,0) as nuke requires.
          formatwin.max.x = dispwin.max.x - (-dispwin.min.x);
        } else {
          // Shift both to get dispwindow over to 0,0.
          dataOffset = -dispwin.min.x;
          formatwin.max.x = dispwin.max.x + dataOffset;
        }
      }

      // Can't do the same for y -- EXR origin is upper left, Nuke's is lower left.
      // Leave data where it is. Put displaywin.max.y at y=0 in Nuke and let it
      // extend as high as it goes.
      formatwin.max.y = dispwin.max.y - dispwin.min.y;

      // Note that this sets both the format and the bbox to
      // 0,0 - width,height. We need to reset the correct bbox afterwards.
      set_info(formatwin.size().x + 1, formatwin.size().y + 1, 4, aspect);

      // Remember that exr boxes start at top left, Nuke's at bottom left
      // so we need to flip the bbox relative to the frame.
      int bx = datawin.min.x + dataOffset;
      int by = dispwin.max.y - datawin.max.y;
      int br = datawin.max.x + dataOffset;
      int bt = dispwin.max.y - datawin.min.y;

      switch (edgeMode)
      {
      case eEdgeMode_Plate:
        // Add a black pixel around the edge of the box, to match what other
        // programs do with exr files, rather than replicate the edge pixels as
        // Nuke does by default. However this is not done if the bounding box
        // matches the display window, so that blurring an image that fills the
        // frame does not leak black in.
        if (datawin.min.x != dispwin.min.x || datawin.max.x != dispwin.max.x ||
            datawin.min.y != dispwin.min.y || datawin.max.y != dispwin.max.y) {
          bx--;
          by--;
          br++;
          bt++;
          info_.black_outside(true);
        }
        break;
      case eEdgeMode_Edge:
        // Add a black pixel around mismatching edges of the box, rather than
        // replicate the edge pixels as Nuke does by default.
        if (datawin.min.x != dispwin.min.x && datawin.max.x != dispwin.max.x &&
            datawin.min.y != dispwin.min.y && datawin.max.y != dispwin.max.y) {
          bx--;
          by--;
          br++;
          bt++;
          info_.black_outside(true);
        }
        else {
          if (datawin.min.x != dispwin.min.x) {
            bx--;
          }
          if (datawin.max.x != dispwin.max.x) {
            br++;
          }
          if (datawin.min.y != dispwin.min.y) {
            bt++;
          }
          if (datawin.max.y != dispwin.max.y) {
            by--;
          }
        }
        break;
      case eEdgeMode_Repeat:
        // Don't add any black pixels
        break;
      case eEdgeMode_Black:
        // Always add black pixels around the edges of the box.
        bx--;
        by--;
        br++;
        bt++;
        info_.black_outside(true);
        break;
      }
      info_.set(bx, by, br+1, bt+1);

      if (inputfile->header(0).lineOrder() == Imf::INCREASING_Y)
        info_.ydirection(-1);
      else
        info_.ydirection(1);

      if ( usePlanesForRead ) {
        _stripeHeight = stripeHeightForCompression( compressionAndHasTiles.compression );
      }

      // Get the number of parts in the input file
      const int nInputParts = inputfile->parts();
  
      // Check the type of the input file to determine whether it is a scanline or not.
      // Type is an optional property for non-multipart exrs, so check whether the file has
      // a type or not first.
      // If the file doesn't have a type, we can't be sure whether it's scanline or not, so assume 
      // it isn't, as reading raw scanlines will fail for non-scanline images.
      bool exrFileIsScanLine = false;
      if (inputfile->header(0).hasType())
          exrFileIsScanLine = (strcmp(inputfile->header(0).type().c_str(), "scanlineimage") == 0);

      // Read raw scanlines from the file and decompress in parallel using multiple engine threads if:
      //  - the exr file type is "scanlineimage" (exrFileIsScanLine)
      //  - we are reading a single scanline at a time (_stripeHeight is 1)
      _readRawScanlines = (exrFileIsScanLine && _stripeHeight == 1);
  
      // If the exr file is scanline, and we're not reading multiple lines, make a buffer for storing compressed scan lines. 
      // Each engine thread will be given space in this buffer for storing a raw scan line read from the input file 
      // before decompressing it.
      if (_readRawScanlines) {
        _compressedScanlineBuffers.resize(nInputParts);
        for (int part = 0; part < nInputParts; part++) {
          _compressedScanlineBuffers[part] = new CompressedScanlineBuffer(inputfile->header(part), inputFileStream->isMemoryMapped());
        }
      }

      // Ignore part names if selected by user
      bool ignorePartNames = alwaysIgnorePartNames;
      
      // If multi-part and not ignoring part names check for legacy files
      if (isMultipart && !ignorePartNames) {
      
        // Find the layer writing setting in the metadata
        MetaData::Bundle::PropertyPtr property = _meta.getData(MetaData::Nuke::FULL_LAYER_NAMES);
        if (property && MetaData::isPropertyInt(property)) {
          int fullLayerNames = MetaData::getPropertyInt(property, 0);
          ignorePartNames = (fullLayerNames != 0);
        }
        else {
          // Auto-detect legacy channel names (with no layer information)
          bool channelsHaveSeparators = false;
          
          // Check the channel names in every part
          for (int n = 0; n < inputfile->parts(); ++n) {
            const Imf::ChannelList& imfchannels = inputfile->header(n).channels();
            
            // Check every channel
            Imf::ChannelList::ConstIterator chan;
            for (chan = imfchannels.begin(); chan != imfchannels.end(); chan++) {
              std::string channelname = chan.name();
              
              // Check for '.' separator
              if (channelname.find('.') != std::string::npos) {
                channelsHaveSeparators = true;
                break;
              } 
            }
            if (channelsHaveSeparators) {
              // Layer names are contained in the channel names
              ignorePartNames = true;
              break;
            }
          }
        }
      }
      
      // Iterate through each part
      for (int n = 0; n < nInputParts; ++n) {

        const Imath::Box2i& datawin = inputfile->header(n).dataWindow();
        const Imath::Box2i& dispwin = inputfile->header(n).displayWindow();
        double aspect = inputfile->header(n).pixelAspectRatio();
        
        // Check these windows against the first header
        if (datawin != inputfile->header(0).dataWindow()) {
          throw std::runtime_error("Multipart data window size must be consistent");
        }
        if (dispwin != inputfile->header(0).displayWindow()) {
          throw std::runtime_error("Multipart display window size must be consistent");
        }
        if (aspect != inputfile->header(0).pixelAspectRatio()) {
          throw std::runtime_error("Multipart pixel aspect ratio must be consistent");
        }
        if (inputfile->header(n).lineOrder() != inputfile->header(0).lineOrder()) {
          throw std::runtime_error("Multipart line order must be consistent");
        }
        
        const Imf::StringVectorAttribute* vectorMultiView = nullptr;

        //     if (inputfile->header(n).hasTileDescription()) {
        //       const Imf::TileDescription& t = inputfile->header(n).tileDescription();
        //       printf("%s Tile Description:\n", filename());
        //       printf(" %d %d mode %d rounding %d\n", t.xSize, t.ySize, t.mode, t.roundingMode);
        //     }

        // should change this to use header->findTypedAttribute<TYPE>(name)
        // rather than the exception mechanism as it is nicer code and
        // less of it. But I'm too scared to do this at the moment.
        try {
          vectorMultiView = inputfile->header(n).findTypedAttribute<Imf::StringVectorAttribute>("multiView");

          if (!vectorMultiView) {
            Imf::Header::ConstIterator it = inputfile->header(n).find("multiView");
            if (it != inputfile->header(n).end() && !strcmp(it.attribute().typeName(), "stringvector"))
              vectorMultiView = static_cast<const Imf::StringVectorAttribute*>(&it.attribute());
          }
        }
        catch (...) {
        }

        if (vectorMultiView) {
          std::vector<std::string> s = vectorMultiView->value();

          bool setHero = false;

          for (size_t i = 0; i < s.size(); i++) {
            if (s[i].length()) {
              views.push_back(s[i]);
              if (!setHero) {
                heroview = s[i];
                setHero = true;
              }
            }
          }
        }
        
        std::string channelPrefix;
        
        // For multi-part files
        if (isMultipart)
        {
          // Legacy files used the part name to store the layer name
          if (!ignorePartNames) {
            // For multi-part files we need to prepend the part name to the channel names.
            // The ExrChannelNameToNuke constructor will parse the combined name for us.
            mFnAssertMsg(inputfile->header(n).hasName(), "name is mandatory in multi-part files");
            if (!inputfile->header(n).name().empty()) {
              channelPrefix += inputfile->header(n).name();
              channelPrefix += '.';
            }
          }
          
          if (inputfile->header(n).hasView()) {
            std::string strView = inputfile->header(n).view();
            // Add to the views list if necessary
            if ( std::find(views.begin(), views.end(), strView) == views.end() ) {
              views.push_back(strView);
            }
            // Set the hero view if necessary
            if (heroview.empty())
              heroview = strView;
          
            // We expect the separated view name to be the last part of the part name.
            // If not then append the view name.
            std::string viewSuffix;
            viewSuffix += '.';
            viewSuffix += strView;
            
            if ( channelPrefix.rfind(viewSuffix) != (channelPrefix.size() - viewSuffix.size() - 1) ) {
              channelPrefix += strView;
              channelPrefix += '.';
            }
          }
        }

        // For each channel in the file, create or locate the matching Nuke channel
        // number, and store it in the channel_map
        const Imf::ChannelList& imfchannels = inputfile->header(n).channels();
        Imf::ChannelList::ConstIterator chan;
        for (chan = imfchannels.begin(); chan != imfchannels.end(); chan++) {

          pixelTypes[chan.channel().type]++;
          
          std::string channelname = channelPrefix + chan.name();
          ExrChannelNameToNuke cName(channelname.c_str(), views);
          std::set<Channel> channels;

          if (this->getChannels(cName, viewName, channels)) {
            if (!channels.empty()) {
              for (std::set<Channel>::iterator it = channels.begin();
                   it != channels.end();
                   it++) {

                Channel channel = *it;
                
                ChannelInfo newChannelInfo(chan.name(), n, cName);

                // Note: the below special cases are here because we are trying to do very clever mapping of EXR channels
                // into NUKE channels. Sometimes this cleverness doesn't work and we get unwanted results.
                bool writeChannelMapping = true;

                // It may be that more than one exr channel has been found to map to a single nuke channel.
                // In this case we only update the channel map if the channel found is a better match.
                if (channel_map.find(channel) != channel_map.end()) {
                  mFnAssertMsg(channel_map[channel].name, "All channels in exr files must have names");
                  
                  writeChannelMapping = newChannelInfo.isBetterThan(channel_map[channel], viewName);
                }

                if (writeChannelMapping) {
                  channel_map[channel] = newChannelInfo;
                }

                mask += channel;
              }
            }
            else {
              iop->warning("Cannot assign channel number to %s", cName.nukeChannelName().c_str());
            }
          }

        }

    #ifdef ENABLE_EXR_INFO_TTY
        {
          const Imf::Header& header = inputfile->header(n);

          std::cout << "--- Header: " << n << " ---" << std::endl;
          
          std::cout << "Has name: " << (header.hasName() ? "yes" : "no") << std::endl;
          if (header.hasName()) {
            std::cout << "Name: " << header.name() << std::endl;
          }
          std::cout << "Has view: " << (header.hasView() ? "yes" : "no") << std::endl;
          if (header.hasView()) {
            std::cout << "View: " << header.view() << std::endl;
          }
          std::cout << "Tiled: " << (header.hasTileDescription() ? "yes" : "no") << std::endl;
          std::cout << "Has preview: " << (header.hasPreviewImage() ? "yes" : "no") << std::endl;
          
          std::string lineOrder;
          switch (header.lineOrder()) {
            case Imf::INCREASING_Y: lineOrder = "INCREASING_Y"; break;
            case Imf::DECREASING_Y: lineOrder = "DECREASING_Y"; break;
            case Imf::RANDOM_Y: lineOrder = "RANDOM_Y"; break;
            default: lineOrder = "***INVALID***"; break;
          }
          std::cout << "Line order: " << lineOrder << std::endl;
          
          std::string compression;
          switch (header.compression()) {
            case Imf::NO_COMPRESSION: compression = "NO_COMPRESSION"; break;
            case Imf::RLE_COMPRESSION: compression = "RLE_COMPRESSION"; break;
            case Imf::ZIPS_COMPRESSION: compression = "ZIPS_COMPRESSION"; break;          
            case Imf::ZIP_COMPRESSION: compression = "ZIP_COMPRESSION"; break;          
            case Imf::PIZ_COMPRESSION: compression = "PIZ_COMPRESSION"; break;
            case Imf::PXR24_COMPRESSION: compression = "PXR24_COMPRESSION"; break;          
            case Imf::B44_COMPRESSION: compression = "B44_COMPRESSION"; break;          
            case Imf::B44A_COMPRESSION: compression = "B44A_COMPRESSION"; break;
            default: compression = "***INVALID***"; break;
          }
          std::cout << "Compression: " << compression << std::endl;
              
          std::cout << "Channels:" << std::endl;
          const Imf::ChannelList& channelList = header.channels();
          for (Imf::ChannelList::ConstIterator chanIter = channelList.begin(); chanIter != channelList.end(); chanIter++) {
            std::string channelType;
            switch (chanIter.channel().type) {
              case Imf::UINT: channelType = "UINT"; break;
              case Imf::HALF: channelType = "HALF"; break;
              case Imf::FLOAT: channelType = "FLOAT"; break;
              default: channelType = "***INVALID***"; break;
            }
            std::cout << "  " << chanIter.name() << " : " << channelType << std::endl;
          }        
        
          const Imath::Box2i& dataWindow = header.dataWindow();
          const Imath::Box2i& displayWindow = header.displayWindow();
          std::cout << "Data window   : " << dataWindow.min.x << ", " << dataWindow.min.y << " -> " << dataWindow.max.x << ", " << dataWindow.max.y << std::endl;
          std::cout << "Display window: " << displayWindow.min.x << ", " << displayWindow.min.y << " -> " << displayWindow.max.x << ", " << displayWindow.max.y << std::endl;
          std::cout << "Derived:" << std::endl;
          std::cout << "Bounding box  : " << bx << ", " << by << " -> " << br << ", " << bt << " (actual pixels)" << std::endl;
          std::cout << "Format        : " << formatwin.min.x << ", " << formatwin.min.y << " -> " << formatwin.max.x << ", " << formatwin.max.y << " (atcual pixels)" << std::endl;
        }
    #endif  // ENABLE_EXR_INFO_TTY
    } // for each part
    
    // Finally set the channels
    info_.channels(mask);
  }
  catch (const Iex::BaseExc& exc) {

    iop->error(exc.what());

    delete inputfile;
    inputfile = nullptr;

    // reset bbox so that we won't get further engine calls
    set_info(0, 0, 0, 0.0);

#ifdef ENABLE_EXR_INFO_TTY
    std::cout << "Error parsing header: " << exc.what() << std::endl; 
    std::cout << "--------------------------------------------------------------------------" << std::endl;
#endif  // ENABLE_EXR_INFO_TTY
        
    return;
  }
  catch (const std::exception& exc) {

    iop->error(exc.what());

    delete inputfile;
    inputfile = nullptr;

    // reset bbox so that we won't get further engine calls
    set_info(0, 0, 0, 0.0);

#ifdef ENABLE_EXR_INFO_TTY
    std::cout << "Error parsing header: " << exc.what() << std::endl; 
    std::cout << "--------------------------------------------------------------------------" << std::endl;
#endif  // ENABLE_EXR_INFO_TTY
    
    return;
  }  

  if (pixelTypes[Imf::FLOAT] > 0) {
    _meta.setData(MetaData::DEPTH, MetaData::DEPTH_FLOAT);
  }
  else if (pixelTypes[Imf::UINT] > 0) {
    _meta.setData(MetaData::DEPTH, MetaData::DEPTH_32);
  }
  if (pixelTypes[Imf::HALF] > 0) {
    _meta.setData(MetaData::DEPTH, MetaData::DEPTH_HALF);
  }

  // If we've extracted view names, add them to the metadata with a dedicated format-agnostic key.
  // For a single part multiview file exrHeaderToMetadata will already have automatically added the names,
  // with the key exr/multiView, but the views may be spread across multiple parts without the file
  // specifying the multiView attribute - the views member will now contain the views however they're stored
  // in the file.
  if (!views.empty()) {
    // First convert the vector of strings into a single string with each view separated by a newline.
    std::vector<std::string>::const_iterator viewsEnd = views.end();
    std::string viewNames;
    for (std::vector<std::string>::const_iterator viewIter = views.begin(); viewIter != viewsEnd; ++viewIter) {
      viewNames += *viewIter;
      // Don't put a newline after the final view.
      if (std::distance(viewIter, viewsEnd) > 1) {
        viewNames += "\n";
      }
    }
    // Now we can set the metadata.
    _meta.setData(MetaData::VIEW_NAMES, viewNames);    
  }

  foreach(z, info_.channels())
    _partSets[channel_map[z].part] += z;
  
#ifdef ENABLE_EXR_INFO_TTY
  std::cout << "--------------------------------------------------------------------------" << std::endl;
#endif  // ENABLE_EXR_INFO_TTY    
}


void exrReader::setupInputFileStream(int fd, const unsigned char* preReadBuffer, int preReadBufferSize,
                                     CompressionAndHasTiles& compressionAndHasTiles)
{
  auto fileReadMode = GetSpecifiedFileReadMode();

  // Update fileReadMode to the actual appropriate read mode if it's initially default.
  if (fileReadMode == FileReadMode::eDefault) {

    // To match the previous behavior, close the Read-opened file handle before we open the file again here.
    // Assume the default mode will never map to eBuffer.
    close(fd);
    fd = -1;

    // The existing default behaviour makes the read mode selection based on the compression type.
    // Also, the node graph specifies the thread count for the MultiPartInputFile based on whether the
    // image data is tiled.
    // That requires reading from the file's header before we can create our MultiPartInputFile object
    // for doing the actual image reading, so creating a temporary MultiPartInputFile to get the header.
    Imf::MultiPartInputFile tmpInputFile(filename(), 0);  // "0" here means don't multi-thread reads from this file object.
    compressionAndHasTiles = GetCompressionAndHasTiles(tmpInputFile);

    fileReadMode = GetDefaultFileReadModeForCompressionType(compressionAndHasTiles.compression);
  }

  // For modes other than eBuffer we're going to re-open the file so close the currently open handle, if we haven't.
  if ((fileReadMode != FileReadMode::eBuffer) && (fd >= 0)) {
    close(fd);
    fd = -1;
  }

  switch (fileReadMode) {
    case FileReadMode::eBuffer:
      {
        mFnAssert(fd >= 0); // We shouldn't have closed the file handle opened by our Read op.
        inputFileStream = new BufferedFileStream(filename(), fd, preReadBuffer, preReadBufferSize);
      }
      break;
    case FileReadMode::eMmap:
      {
        // The user has specifically chosen mmap, so use this regardless of the compression type.
        // However, the "lieAboutMemoryMapped" internals of the MemoryMappedIStream does depend on the compression type
        // so we'll get it in the same way as the legacy code, i.e. first using a temporary InputFile that accesses the
        // file in the 'normal' way to read the header info.
        if (compressionAndHasTiles.unset) {
          Imf::MultiPartInputFile tmpInputFile(filename(), 0);  // "0" here means don't multi-thread reads from this file object.
          compressionAndHasTiles = GetCompressionAndHasTiles(tmpInputFile);
        }
        const bool lieAboutMemoryMapped = (compressionAndHasTiles.compression == Imf::DWAA_COMPRESSION ||
                                           compressionAndHasTiles.compression == Imf::DWAB_COMPRESSION);
        inputFileStream = new MemoryMappedIStream(filename(), lieAboutMemoryMapped);
      }
      break;
    case FileReadMode::eNormal:
    case FileReadMode::eDefault:    // Shouldn't still be eDefault, just handle all enumerations with something reasonable
      {                             // and also let the compiler warn about any new missing cases.
        #if defined(_WIN32)
        inputStream = new std::ifstream(WideCharWrapper(filename()), std::ios_base::binary);
        #else
        inputStream = new std::ifstream(filename(), std::ios_base::binary);
        #endif
        inputFileStream = new Imf::StdIFStream(*inputStream, filename());
      }
      break;
  }
}

exrReader::~exrReader()
{
  delete inputfile;
  delete inputFileStream;
  delete inputStream;

  const size_t nBuffers = _compressedScanlineBuffers.size();
  for (unsigned int buffer = 0; buffer < nBuffers; buffer++)
    delete _compressedScanlineBuffers[buffer];
  _compressedScanlineBuffers.clear();
}

void exrReader::open()
{
  // Create input part 0 now for use in any subsequent engine(), fetchPlane() or planarDecodePass() calls,
  // that need part 0. The motivation is for performance reasons for the engine/fetchPlane usage, it won't
  // make any difference for planarDecodePass but simplifies the code to do it unconditionally.
  _inputPart0 = std::make_unique<Imf::InputPart>(*inputfile, 0);
}

void exrReader::_validate(bool for_real)
{
}

void exrReader::engine_inner(const Imath::Box2i& datawin,
                              const Imath::Box2i& dispwin,
                              const ChannelSet&   channels,
                              int                 exrY,
                              Row&                row,
                              int                 x,
                              int                 X,
                              int                 r,
                              int                 R)
{
  row.range(this->x(), this->r());

  // Record all the parts and channels
  std::set<int> partSet;
  std::map<Channel, Channel> toCopy;

  processChannels(channels, partSet, toCopy);

  // Iterate through the parts (and frame buffers)
  std::set<int>::const_iterator it = partSet.begin();
  const std::set<int>::const_iterator end = partSet.end();
  for( ; it != end; ++it)
  {
    int part = *it;
      
    // Create a framebuffer for this part
    Imf::FrameBuffer fbuf;

    foreach (z, channels) {
        
      // Filter to the current part
      if (channel_map[z].part != part)
        continue;

      // Skip duplicate channels
      if (toCopy.find(z) != toCopy.end()) {
        continue;
      }
    
      // Place the row in the respective framebuffer
      float* dest = row.writable(z);
      for (int xx = x; xx < X; xx++)
        dest[xx] = 0;
      for (int xx = R; xx < r; xx++)
        dest[xx] = 0;
      fbuf.insert(channel_map[z].name,
                  Imf::Slice(Imf::FLOAT, (char*)(dest + dataOffset), sizeof(float), 0));
    }

    try {
  
      // Scan line case: read raw scanlines into a buffer - only one thread can access the file at a time, 
      // so this bit has a lock round it. Then decompress the scan lines and store in the frame buffer in
      // a separate step - multiple engine threads can do this part at once. 

        if (_readRawScanlines) {

          if (iop->aborted())
              return;                     // abort if another thread does so
          
          // Read a scan line from the input part. Only one thread should read from the file at a time.
          CompressedScanline *scanlinePtr = nullptr; 
          {
            Guard guard(C_lock);
            if (part == 0) {
              // Read from previously constructed input part 0 (this optimisation is possible because all files have a part 0)
              Imf::InputPart& inputPart = *(_inputPart0.get());
              scanlinePtr = _compressedScanlineBuffers[part]->readRawScanlineFromFile(inputPart, exrY); 
            }
            else {
              // Make a new input part and read from it
              Imf::InputPart inputPart(*inputfile, part);
              scanlinePtr = _compressedScanlineBuffers[part]->readRawScanlineFromFile(inputPart, exrY);
            }
          }
          
          // Store the scan line in the frame buffer. (This will uncompress it first if necessary.)
          _compressedScanlineBuffers[part]->copyScanlineToFrameBuffer(scanlinePtr, fbuf, exrY);
      }
      else {
          // Fallback case: read and decompress the image data in one step. Only one engine thread can do this
          // at a time.

          Guard guard(C_lock);
  
          if (iop->aborted())
              return; // abort if another thread does so
    
          if (part == 0) {
            // Read from previously constructed input part 0 (this optimisation is possible because all files have a part 0)
            Imf::InputPart& inputPart = *(_inputPart0.get());
            inputPart.setFrameBuffer(fbuf);
            inputPart.readPixels(exrY);
          }
          else {
            // Make a new input part and read from it
            Imf::InputPart inputPart(*inputfile, part);
            inputPart.setFrameBuffer(fbuf);
            inputPart.readPixels(exrY);
          }
      }
    }
    catch (const std::exception& exc) {
      iop->error(exc.what());
      return;
    }   
    
  }
   
  // Copy the duplicated channels
  foreach (z, channels) {
    if (toCopy.find(z) != toCopy.end()) {
      float* dest = row.writable(z);
      const float* src = row[toCopy[z]];

      for (int col = x; col < r; col++) {
        dest[col] = src[col];
      }
    }
  }
}

void exrReader::engine(int y, int x, int r, ChannelMask c1, Row& row)
{
  mFnAssert(inputfile != nullptr);

  // Need to find the part containing this channel
  const Imath::Box2i& dispwin = inputfile->header(0).displayWindow();
  const Imath::Box2i& datawin = inputfile->header(0).dataWindow();

  // Invert to EXR y coordinate:
  int exrY = convertY(y);

  // Figure out intersection of x,r with the data in exr file:
  const int X = MAX(x, datawin.min.x + dataOffset);
  const int R = MIN(r, datawin.max.x + dataOffset + 1);

  // Black outside the box:
  if (exrY < datawin.min.y || exrY > datawin.max.y || R <= X) {
    row.erase(c1);
    return;
  }

  ChannelSet channels(c1);
  if (needsAlpha(channels))
    channels += (Mask_Alpha);

  engine_inner(datawin, dispwin, channels, exrY, row, x, X, r, R);

  // Do colorspace conversion, now that we have the alpha for premultiplied:
  if (!iop->aborted() && !lut()->linear()) {
    const float* alpha = (channels & Mask_Alpha) ? row[Chan_Alpha] + X : nullptr;
    for (Channel chan = Chan_Red; chan <= Chan_Blue; chan = Channel(chan + 1)) {
      if (intersect(channels, chan)) {
        const float* src = row[chan] + X;
        float* dest = row.writable(chan) + X;
        from_float(chan, dest, src, alpha, R - X);
      }
    }
  }
}

void exrReader::processChannels(const DD::Image::ChannelSet& channels, std::set<int>& partSet, std::map<Channel, Channel>& toCopy)
{
  // Record all the parts and channels
  std::map<ChannelInfo, Channel> usedChans;

  // Determine the channels to generate and copy
  foreach (z, channels) {

    if (channel_map.count(z) == 0)
      continue;

    // Simply copy duplicate channels
    if (usedChans.find(channel_map[z]) != usedChans.end()) {
      toCopy[z] = usedChans[channel_map[z]];
      continue;
    }
    
    // Record the channel and part
    usedChans[channel_map[z]] = z;
    partSet.insert(channel_map[z].part);
  }

}

PlanarI::PlaneID exrReader::getPlaneFromChannel(Channel chan)
{
  return _partSets[channel_map[chan].part];
}

void exrReader::fetchPlane(ImagePlane& outputPlane)
{
  const int y = outputPlane.bounds().y();
  const int lines = outputPlane.bounds().h();

  // The channels in the image plane we've been asked to fill. 
  const ChannelSet& outputChannels = outputPlane.channels();

  // Pointer to the image plane to fill with the data read from the exr file.
  // This points to the output plane unless we need temporary copies of channels
  // that aren't in the output (e.g. alpha for premultiplication).
  ImagePlane* imagePlanePtr = &outputPlane;

  ChannelSet channelsToFill = outputChannels;
  
  // Temporary image plane to use inside this function if we need alpha and the plane
  // passed in doesn't have it.
  ImagePlane tempImagePlane;

  // If we need alpha (for premultiplication) and the plane we've been asked to fill
  // doesn't have it, set up the temporary plane and make imagePlanePtr point to that
  // one instead.
  const bool hasAlpha = outputChannels & Mask_Alpha;
  const bool needsTempAlpha = needsAlpha(outputChannels) && !hasAlpha;
  if (needsTempAlpha) {
    channelsToFill += Mask_Alpha;
    tempImagePlane = ImagePlane(outputPlane.bounds(),
                                outputPlane.packed(),
                                channelsToFill);
    imagePlanePtr = &tempImagePlane;
  }

  // Make the image plane we're going to fill in writable. (If this isn't the output
  // plane, the output plane will be allocated later by the call to 
  // ImagePlane::copyIntersectionFrom().)
  imagePlanePtr->makeWritable();


  const Imath::Box2i& datawin = inputfile->header(0).dataWindow();

  if(std::getenv("NUKE_EXR_DISABLE_THREADED_FILL") == nullptr)
  {
    if(datawin.min.x > imagePlanePtr->bounds().x() || datawin.max.x +1 < imagePlanePtr->bounds().r() || datawin.min.y > imagePlanePtr->bounds().y() || datawin.max.y +1 < imagePlanePtr->bounds().t()) {
      foreach(z, channelsToFill) {
        imagePlanePtr->fillChannelThreaded(z, 0.0);
      }
    }
  }
  else {
    foreach(z, channelsToFill) {
      imagePlanePtr->fillChannel(z, 0.0);
    }
  }

  mFnAssertMsg(datawin.min.x >= imagePlanePtr->bounds().x(), "Planar Interface: Pixels buffer of ImagePlane is not big enough for pixels from the EXR source");
  mFnAssertMsg(datawin.max.x < imagePlanePtr->bounds().r(), "Planar Interface: Pixels buffer of ImagePlane is not big enough for pixels from the EXR source");

  int exrBottom = convertY(y);
  int exrTop = convertY(y + lines - 1);
  
  exrBottom = std::min(exrBottom, datawin.max.y);
  exrTop = std::max(exrTop, datawin.min.y);
  
  if (exrTop > exrBottom)
    return;

  std::set<int> partSet;
  std::map<Channel, Channel> toCopy;

  processChannels(channelsToFill, partSet, toCopy);

  // Iterate through the parts (and frame buffers)
  for (const auto part : partSet) {
    
    const int rowStride = imagePlanePtr->rowStride() * sizeof(float);
    
    // We need to get the address that corresponds to the origin, since readPixels will write into
    // our output buffer using
    //    dest + x * xStride + y * yStride
    // where x and y are EXR pixel coordinates and the strides are what we pass in when setting up
    // the Slice for each channel (and actually we need to specify the appropriate dest address for
    // each channel, in which case channelDest, see below, will only equal dest for channel 0).
    // Ah, but it's a bit more complicated than that - you've noticed the use of convertY().
    // We use that because we're going to automatically vertically flip the image data as we read it.
    // By passing y = 0 into convertY() we get the output address that corresponds to the *EXR origin*
    // and then passing in a negative row stride, so that as the EXR lib increments its row index, moving
    // down the image (remember EXR y increases downwards) then the -ve row stride cause it to write
    // into earlier addresses in the output buffer.
    // All very confusing! The simplest case is that the data and display windows match, with their
    // (EXR) minimum, ie top row, at (EXR) y = 0. And suppose we only using a single PlanarIop stripe.
    // Then call to convertY(0) will return max.y, i.e. the top row in Nuke coords. So, the dest
    // address corresponds to the start of the top row and then readPixels uses
    //    (start of top row) + (x * col stride) + (y * - row stride)
    // to get the output buffer address (with x and y in EXR coords), so when y = 0 it writes to
    // the top row at the end of the buffer, and when y reaches max.y it'll write at an *earlier*
    // address corresponding to the bottom row at the start of the output buffer.
    float* dest = imagePlanePtr->writableAt(convertY(0), 0).ptr();
    
    // Create a framebuffer for this part
    Imf::FrameBuffer fbuf;
    
    for (auto z : channelsToFill) {

      if (channel_map.count(z) == 0)
        continue;

      // Filter to the current part
      if (channel_map[z].part != part)
        continue;

      // Skip duplicate channels
      if (toCopy.find(z) != toCopy.end()) {
        continue;
      }

      if (channel_map.find(z) != channel_map.end()) {

        int chanNo = imagePlanePtr->chanNo(z);

        // Place the row in the respective framebuffer
        float* channelDest = (dest + chanNo * imagePlanePtr->chanStride());
        fbuf.insert(channel_map[z].name,
                    Imf::Slice(Imf::FLOAT, (char*)channelDest, sizeof(float), -rowStride));
      }
    }

    // setFrameBuffer uses shared state
    Guard guard(C_lock);

    try {
      if (iop->aborted())
        return;                     // abort if another thread does so
      
      if (part == 0) {
        // Read from previously constructed input part 0 (this optimisation is possible because all files have a part 0)
        Imf::InputPart& inputPart = *(_inputPart0.get());
        inputPart.setFrameBuffer(fbuf);
        inputPart.readPixels(exrTop, exrBottom);
      }
      else {
        // Make a new input part and read from it
        Imf::InputPart inputPart(*inputfile, part);
        inputPart.setFrameBuffer(fbuf);
        inputPart.readPixels(exrTop, exrBottom);
      }
    }
    catch (const std::exception& exc) {
      iop->error(exc.what());
      return;
    }
  }

  // Copy the duplicated channels
  foreach (z, channelsToFill) {
    if (toCopy.find(z) != toCopy.end()) {
      imagePlanePtr->copyChannel(z, toCopy[z]);
    }
  }

  if (!iop->aborted() && !lut()->linear()) {
    const float* alpha = (channelsToFill & Mask_Alpha) ? &imagePlanePtr->at(imagePlanePtr->bounds().x(), imagePlanePtr->bounds().y(), Chan_Alpha) : nullptr;
    for (Channel chan = Chan_Red; chan <= Chan_Blue; chan = Channel(chan + 1)) {
      if (intersect(channelsToFill, chan)) {
        float* pix = &imagePlanePtr->writableAt(imagePlanePtr->bounds().x(), imagePlanePtr->bounds().y(), imagePlanePtr->chanNo(chan));
        from_float(chan, pix, pix, alpha, imagePlanePtr->bounds().area(), imagePlanePtr->colStride());
      }
    }
  }

  // If we made a temporary image plane to store the alpha, copy the 
  // intersection between this and the output plane into the output
  // plane (this will allocate the output plane if necessary, and 
  // should do a shallow copy if it's more efficient to do so).
  if (!iop->aborted() && needsTempAlpha) {
    mFnAssert(imagePlanePtr == &tempImagePlane); 
    outputPlane.copyIntersectionFrom(*imagePlanePtr);
  }
}
   
// Comparison operator to decide on the best match
bool exrReader::ChannelInfo::isBetterThan(const ChannelInfo& rhs, const std::string& viewname) const
{
  // If in doubt, this one is better
  bool isBetter = true;

  const int existingLength = strlen(rhs.name);
  const int newLength = strlen(name);

  const bool existingChannelHasEmptyLayerName = (existingLength > 0) && rhs.name[0] == '.';

  // Check for an exact view match (as opposed to default/hero view)
  if (viewname == channelName.view() && viewname != rhs.channelName.view()) {
    isBetter = true;
  }
  else if (viewname == rhs.channelName.view() && viewname != channelName.view()) {
    isBetter = false;
  }
  else if (existingChannelHasEmptyLayerName && existingLength == (newLength + 1)) {
    // Bug 31548 - Read - EXR rendering black for R, G and B channels
    // "channel" should override ".channel"
    isBetter = true;
  }
  else if (existingLength > newLength) {
    // Bug 23181 - Specific named channel are not read correctly for both stereo views
    // the loop is in alphabetical order, so check length
    // to avoid replacing "right.rotopaint.blue" with
    // the less specific but alphabetically later
    // "rotopaint.blue"
    isBetter = false;
  }
  
  return isBetter;
}
