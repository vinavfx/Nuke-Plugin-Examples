// pngReader.C
// Copyright (c) 2009 The Foundry Visionmongers Ltd.  All Rights Reserved.

// Reads png files using libpng (www.libpng.org)

#include "png.h"
#include "DDImage/DDWindows.h"
#undef FAR  // suppress warning in zconf.h caused by windows.h
#include "DDImage/Reader.h"
#include "DDImage/Row.h"
#include "DDImage/ARRAY.h"
#include "DDImage/Thread.h"

#if defined (__unix__) || (defined (__APPLE__) && defined (__MACH__))
#include <unistd.h>
#endif

// need to make libpng built using vs2003 link with vs2005
#ifdef _WIN32
  #include <io.h>
extern "C" {
FILE _iob[3] = {
  __acrt_iob_func(0), __acrt_iob_func(1), __acrt_iob_func(2)
};
}
#endif

using namespace DD::Image;

class pngReader : public Reader
{
  FILE* in;
  png_structp png_ptr;
  png_infop info_ptr;
  png_byte** row_pointers;
  png_byte* png_pixels;
  png_uint_32 wdt, hgt;
  int depth, dd, bpp, color, interlace;

  MetaData::Bundle _meta;
public:
  const MetaData::Bundle& fetchMetaData(const char* key) override
  {
    return _meta;
  }
  const char* errormessage;
  pngReader(Read*, int fd);
  ~pngReader() override;
  void engine(int y, int x, int r, ChannelMask, Row &) override;
  void open() override;
  static const Description d;

};

/** Check for the correct magic numbers at the start of the file
 */
static bool test(int, const unsigned char* block, int length)
{
  return png_sig_cmp((png_byte*)block, (png_size_t)0, 8) == 0;
}

static Reader* build(Read* iop, int fd, const unsigned char* b, int n)
{
  return new pngReader(iop, fd);
}

const Reader::Description pngReader::d("png\0", build, test);

static void errorhandler(png_structp png_ptr, const char* b)
{
  ((pngReader*)(png_get_error_ptr(png_ptr)))->errormessage = b;
}

pngReader::pngReader(Read* r, int fd) : Reader(r)
{
  in = nullptr;
  png_ptr = nullptr;
  info_ptr = nullptr;
  row_pointers = nullptr;
  png_pixels = nullptr;
  errormessage = "libpng error";

  // Use the error handler for the warnings as well - neither can be null, or we might get a crash
  png_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING, this, errorhandler, errorhandler);
  if (!png_ptr) {
    iop->error("Failed to read .png file; png library mismatch");
    return;
  }
  info_ptr = png_create_info_struct(png_ptr);
  if (!info_ptr) {
    iop->error("Failed to read .png file; can't create information field");
    return;
  }
  if (setjmp(png_jmpbuf(png_ptr))) {
    iop->error(errormessage);
    return;
  }

  lseek(fd, 0, 0);
  in = fdopen(fd, "r");
  png_init_io(png_ptr, in);

  png_read_info(png_ptr, info_ptr);
  png_get_IHDR(png_ptr, info_ptr, &wdt, &hgt, &bpp, &color,
               &interlace, nullptr, nullptr);

  // transform paletted images into full-color rgb
  if (color == PNG_COLOR_TYPE_PALETTE)
    png_set_expand(png_ptr);
  // expand images to bit-depth 8 (only applicable for grayscale images)
  if (color == PNG_COLOR_TYPE_GRAY && bpp < 8)
    png_set_expand(png_ptr);
  // transform transparency maps into full alpha-channel
  if (png_get_valid(png_ptr, info_ptr, PNG_INFO_tRNS))
    png_set_expand(png_ptr);
  // transform grayscale images into full-color
  if (color == PNG_COLOR_TYPE_GRAY ||
      color == PNG_COLOR_TYPE_GRAY_ALPHA)
    png_set_expand_gray_1_2_4_to_8(png_ptr);
  // swap bytes if needed
#if __BIG_ENDIAN__
#else
  if (bpp == 16)
    png_set_swap(png_ptr);
#endif

  // read the new info after applying all the above transforms
  png_read_update_info (png_ptr, info_ptr);
  png_get_IHDR(png_ptr, info_ptr, &wdt, &hgt, &bpp, &color,
               &interlace, nullptr, nullptr);

  switch (color) {
    case PNG_COLOR_TYPE_GRAY: dd = 1;
      depth = 1;
      break;                                          // read into R
    case PNG_COLOR_TYPE_GRAY_ALPHA: dd = 2;
      depth = 4;
      break;                                                // leave G and B black
    case PNG_COLOR_TYPE_RGB: dd = 3;
      depth = 3;
      break;
    case PNG_COLOR_TYPE_RGB_ALPHA: dd = 4;
      depth = 4;
      break;
    default:
      iop->error("Failed to read .png file; unsupported color scheme.");
      return;
  }
#if 0
  if (interlace != PNG_INTERLACE_NONE) {
    iop->error("Failed to read .png file; unsupported image interlacing.");
    return;
  }
#endif

  // Read pixel aspect ratio.
  // If not specified, pass 0.0 which means the default will be set within set_info
  // depending on -a (anamorphic) Nuke option.
  double par = 0.0;
  png_uint_32 px = 1;
  png_uint_32 py = 1;
  if (png_get_pHYs(png_ptr, info_ptr, &px, &py, nullptr) && py != 0) {
    par = static_cast<double>(px) / static_cast<double>(py);
  }

  // If pixel aspect ratio is not specified, set_info will choose the first format with
  // matching width and height. In cases where several formats have the same width and height,
  // but different pixel aspect ratio, pixel aspect ratio is needed to choose the right format.
  set_info(wdt, hgt, depth, par);

  _meta.setData(MetaData::DEPTH, MetaData::DEPTH_FIXED(bpp));
}

// Delay the expensive reading of file until this is called, in a parallel
// thread. We then read the data and dispose of the png reader structure:
void pngReader::open()
{
  if (png_ptr) {
    if (setjmp(png_jmpbuf(png_ptr))) {
      iop->error("libpng error");
      return;
    }
    png_uint_32 row_bytes = png_get_rowbytes(png_ptr, info_ptr);
    png_pixels = new png_byte[row_bytes * hgt];
    row_pointers = new png_byte *[hgt];
    for (png_uint_32 i = 0; i < hgt; i++)
      row_pointers[i] = png_pixels + i * row_bytes;

    // now we can go ahead and just read the whole image
    png_read_image (png_ptr, row_pointers);

    // read rest of file, and get additional chunks in info_ptr - REQUIRED
    png_read_end (png_ptr, info_ptr);

    // clean up after the read, and free any memory allocated - REQUIRED
    png_destroy_read_struct (&png_ptr, &info_ptr, (png_infopp) nullptr);
    png_ptr = nullptr;
    fclose(in);
    in = nullptr;

#if 0
    // Dump it for use as icon:
    if (hgt < 65) {
      printf("%d x %d png file:", wdt, hgt);
      for (unsigned y = 0; y < hgt; y++) {
        for (unsigned x = 0; x < wdt; x++) {
          unsigned char* p = row_pointers[y] + x * dd;
          unsigned argb = (p[0] << 16) + (p[1] << 8) + (p[2]) + (p[3] << 24);
          if (!(x % 8))
            printf("\n");
          printf("0x%08x,", argb);
        }
      }
      printf("\n");
    }
#endif
  }
}

pngReader::~pngReader()
{
  if (png_ptr) {
    png_destroy_read_struct(&png_ptr, &info_ptr, nullptr);
    fclose(in);
  }
  delete[] row_pointers;
  delete[] png_pixels;
}

// The engine reads individual rows out of the input.
void pngReader::engine(int y, int x, int xr, ChannelMask channels, Row& row)
{
  if (!png_pixels)
    return;                // some error happend before open() was called
  png_uint_32 picy = hgt - y - 1;
  const int n = xr - x;

  // now convert all the color components
  if (bpp == 16) {
    U16* alpha = nullptr;
    U16* linebuffer16 = (U16*)(row_pointers[picy]) + x * dd;
    if (dd == 2)
      alpha = linebuffer16 + 1;
    if (dd == 4)
      alpha = linebuffer16 + 3;
    if (dd >= 1 && channels & Mask_Red) { // first channel always goes into 'red'
      float* dst = row.writable(Chan_Red);
      if (dst)
        from_short(Chan_Red, dst + x, linebuffer16, alpha, n, 16, dd);
    }
    if (dd >= 3 && channels & Mask_Green) { // write green if this is RGB
      float* dst = row.writable(Chan_Green);
      if (dst)
        from_short(Chan_Green, dst + x, linebuffer16 + 1, alpha, n, 16, dd);
    }
    if (dd >= 3 && channels & Mask_Blue) { // write blue if this RGB
      float* dst = row.writable(Chan_Blue);
      if (dst)
        from_short(Chan_Blue, dst + x, linebuffer16 + 2, alpha, n, 16, dd);
    }
    if (dd == 4 && channels & Mask_Alpha) { // rgba
      float* dst = row.writable(Chan_Alpha);
      if (dst)
        from_short(Chan_Alpha, dst + x, linebuffer16 + 3, nullptr, n, 16, dd);
    }
    if (dd == 2 && channels & Mask_Alpha) { // gray+a
      float* dst = row.writable(Chan_Alpha);
      if (dst)
        from_short(Chan_Alpha, dst + x, linebuffer16 + 1, nullptr, n, 16, dd);
    }
  }
  else {
    png_byte* linebuffer = row_pointers[picy] + x * dd;
    uchar* alpha = nullptr;
    if (dd == 2)
      alpha = linebuffer + 1;
    if (dd == 4)
      alpha = linebuffer + 3;
    if (dd >= 1 && channels & Mask_Red) { // first channel always goes into 'red'
      float* dst = row.writable(Chan_Red);
      if (dst)
        from_byte(Chan_Red, dst + x, linebuffer, alpha, n, dd);
    }
    if (dd >= 3 && channels & Mask_Green) { // write green if this is RGB
      float* dst = row.writable(Chan_Green);
      if (dst)
        from_byte(Chan_Green, dst + x, linebuffer + 1, alpha, n, dd);
    }
    if (dd >= 3 && channels & Mask_Blue) { // write blue if this RGB
      float* dst = row.writable(Chan_Blue);
      if (dst)
        from_byte(Chan_Blue, dst + x, linebuffer + 2, alpha, n, dd);
    }
    if (dd == 4 && channels & Mask_Alpha) { // rgba
      float* dst = row.writable(Chan_Alpha);
      if (dst)
        from_byte(Chan_Alpha, dst + x, linebuffer + 3, nullptr, n, dd);
    }
    if (dd == 2 && channels & Mask_Alpha) { // gray+a
      float* dst = row.writable(Chan_Alpha);
      if (dst)
        from_byte(Chan_Alpha, dst + x, linebuffer + 1, nullptr, n, dd);
    }
  }
  if (dd == 2 && channels & Mask_Green) { // gray+a sets green and blue to black
    row.erase(Chan_Green);
  }
  if (dd == 2 && channels & Mask_Blue) { // gray+a sets green and blue to black
    row.erase(Chan_Blue);
  }
}
