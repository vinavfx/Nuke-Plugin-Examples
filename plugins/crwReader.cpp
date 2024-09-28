// crwReader.C
// Copyright (c) 2010 The Foundry Visionmongers Ltd.  All Rights Reserved.

/* Reads crw files via popen of dcraw conversion tool.  (Really is
   reading 16bit P6 format PPM files)

   04/14/03     Initial Release                Charles Henrich (henrich@d2.com)
   09/14/06     Indentation, removed unused variables and unnecessary knobs
 */

#ifdef _WIN32
  #define _WINSOCKAPI_
#endif

#include "DDImage/Reader.h"
#include "DDImage/Row.h"
#include "DDImage/Thread.h"
#include "DDImage/DDString.h"
#include "DDImage/MetaData.h"
#include "DDImage/Knobs.h"

#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>

#ifdef _WIN32
  #include <io.h>
#else
  #include <unistd.h>
#endif

#ifdef _WIN32
  #define ushort unsigned short
  #define popen _popen
  #define pclose _pclose
#endif

using namespace DD::Image;

static const char* kDefaultDcRawArgs = "-4 -c";

class crwReaderFormat : public ReaderFormat
{
  std::string _commandArgs;

public:
  crwReaderFormat()
  {
    _commandArgs = kDefaultDcRawArgs;
  }

  const std::string& commandArgs() const
  {
    return _commandArgs;
  }

  void knobs(Knob_Callback c) override
  {
    String_knob(c, &_commandArgs, "args", "arguments to 'dcraw'");
  }

  void append(Hash& hash) override
  {
    hash.append(_commandArgs);
  }
};

class crwReader : public Reader
{

  int C_ppmwidth, C_ppmheight, C_ppmmaxval;
  ushort* C_image_cache;
  void barf(const char* command)
  {
    iop->error("\nError running %s\n"
               "If you have the \"dcraw\" software installed, make sure that it's in your path.\n"
               "If you don't have it, the latest version is available as source from:\n"
               "    http://www.cybercom.net/~dcoffin/dcraw/\n"
               "where you can also find links to precompiled versions for Windows and OSX."
               , command);
  }

public:

  MetaData::Bundle _meta;
  const MetaData::Bundle& fetchMetaData(const char* key) override
  {
    return _meta;
  }

  crwReader(Read*, int fd);
  ~crwReader() override;
  void engine(int y, int x, int r, ChannelMask, Row &) override;
  static const Description d;

};

static bool test(int fd, const unsigned char* block, int length)
{
  /* Figure out test later XXX */
  return true;
}

static Reader* build(Read* iop, int fd, const unsigned char* b, int n)
{
  // crwReader never accesses the opened file itself, just passes the filename to dcraw,
  // so we might as well close the file now rather than wait until the dtor, then pass in
  // -1 to the ctor to indicate an invalid value, in case that ever gets modified to use it.
  ::close(fd);
  return new crwReader(iop, -1);
}

static ReaderFormat* buildformat(Read* iop)
{
  return new crwReaderFormat();
}

const Reader::Description crwReader::d("crw\0cr2\0", build, test, buildformat);

crwReader::crwReader(Read* r, int fd) : Reader(r)
{
  // WARNING: This would normally only ever be called by build(), which will have closed the file
  // and set the file descriptor, fd, to -1.

  C_image_cache = nullptr;

  info_.ydirection(-1);

  std::string args = kDefaultDcRawArgs;

  crwReaderFormat* trf = dynamic_cast<crwReaderFormat*>(r->handler());
  if (trf) {
    args = trf->commandArgs();
  }

  char command[BUFSIZ];
  snprintf(command, BUFSIZ, "dcraw %s \"%s\"", args.c_str(), filename());
#ifdef _WIN32
  FILE* pipe = popen(command, "rb");
#else
  FILE* pipe = popen(command, "r");
#endif

  if (!pipe) {
    barf(command);
    return;
  }
  printf("crwReader: %s\n", command);

  char magic[4];
  magic[0] = '\0';
  // put some dummy values in so if reading fails it does not crash:
  C_ppmwidth = 640;
  C_ppmheight = 480;

  fscanf(pipe, "%3s %d %d %d", magic, &C_ppmwidth, &C_ppmheight, &C_ppmmaxval);

  if (strcmp(magic, "P6") != 0) {
    pclose(pipe);
    barf(command);
    return;
  }
  printf("crwReader: reading pixels\n");

  int numpixels = C_ppmwidth * C_ppmheight * 3;
  C_image_cache = new ushort[numpixels];

  fgetc(pipe); /* Skip whitespace char after header */

  int numread = fread(C_image_cache, 2, numpixels, pipe);
  if (numread < numpixels) {
    if (numread < 1)
      barf(command);
    else
      iop->error("dcraw only returned %d of the %d samples needed",
                 numread, numpixels);
  }

  frommsb(C_image_cache, numpixels);

  pclose(pipe);
  printf("crwReader: done\n");

  set_info(C_ppmwidth, C_ppmheight, 3);

  _meta.setData(MetaData::DEPTH, MetaData::DEPTH_16);
}

crwReader::~crwReader()
{
  delete[] C_image_cache;
}

void crwReader::engine(int y, int x, int r, ChannelMask channels, Row& row)
{
  int xcount;
  int cacheoffset = 0;
  float* dstpixrow;

  if (!C_image_cache) {
    row.erase(channels);
    return;
  }

  y = height() - 1 - y;

  foreach(z, channels) {
    dstpixrow = row.writable(z);
    cacheoffset = (y * C_ppmwidth + x) * 3 + (z - 1);
    for (xcount = x; xcount < r; xcount++) {
      dstpixrow[xcount] = float(C_image_cache[cacheoffset] * 1.0 / C_ppmmaxval);
      cacheoffset += 3;
    }
  }
}
