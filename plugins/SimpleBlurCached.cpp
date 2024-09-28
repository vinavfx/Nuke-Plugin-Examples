// Copyright (c) 2018 The Foundry Visionmongers Ltd.  All Rights Reserved.

static const char* const CLASS = "SimpleBlurCached";

static const char* const HELP =
  "Does a simple box blur";

#include "DDImage/Iop.h"
#include "DDImage/NukeWrapper.h"
#include "DDImage/Row.h"
#include "DDImage/Tile.h"
#include "DDImage/Knobs.h"
#include "DDImage/ImageCache.h"
#include "DDImage/ChannelSet.h"

using namespace std;
using namespace DD::Image;

class SimpleBlurCached : public Iop
{
  int _param_blur;
  float _param_constant;
  bool _isFirstTime;

  std::vector<float> vec_dst_image;

  Lock _lock;

  bool fetchRGBAImage(std::vector<float>&, const int height, const int width);

  /* Return available RGBA channels */
  ChannelSet getNeededChannels() const;

public:

  int maximum_inputs() const { return 1; }
  int minimum_inputs() const { return 1; }

  //! Constructor. Initialize user controls to their default values.
  SimpleBlurCached (Node* node) : Iop (node)
  {
    _param_blur = 1;
    _param_constant = 0;
    _isFirstTime = true;
  }

  ~SimpleBlurCached () {}

  void _validate(bool);
  void _request(int x, int y, int r, int t, ChannelMask channels, int count);
  void knobs(Knob_Callback f);

  //! This function does all the work.
  void engine ( int y, int x, int r, ChannelMask channels, Row& out );

  //! Return the name of the class.
  const char* Class() const { return CLASS; }
  const char* node_help() const { return HELP; }

  //! Information to the plug-in manager of DDNewImage/Nuke.
  static const Iop::Description description;
};

/*! This is a function that creates an instance of the operator, and is
   needed for the Iop::Description to work.
 */
static Iop* SimpleBlurCreate(Node* node)
{
  return new SimpleBlurCached(node);
}

/*! The Iop::Description is how NUKE knows what the name of the operator is,
   how to create one, and the menu item to show the user. The menu item may be
   0 if you do not want the operator to be visible.
 */
const Iop::Description SimpleBlurCached::description ( CLASS, "Merge/SimpleBlurCached",
                                                     SimpleBlurCreate );

ChannelSet SimpleBlurCached::getNeededChannels() const
{
  ChannelSet rgbaChannels = Mask_RGBA;
  rgbaChannels &= info_.channels();

  return rgbaChannels;
}

void SimpleBlurCached::_validate(bool for_real)
{
  copy_info(); // copy bbox channels etc from input0, which will validate it.

  _isFirstTime = true;
}

void SimpleBlurCached::_request(int x, int y, int r, int t, ChannelMask channels, int count)
{
  // Add available RGBA channels so that we can pull them when setting up the cache
  ChannelSet requiredChannels = channels + getNeededChannels();

  input(0)->request(0, 0, info_.w(), info_.h(), requiredChannels, count);
}

void SimpleBlurCached::knobs(Knob_Callback f)
{
  Int_knob(f,  &_param_blur, "size", "size");
  Float_knob(f, &_param_constant, "constant", "constant");
}

bool SimpleBlurCached::fetchRGBAImage(std::vector<float>& vec, const int height, const int width)
{
  ChannelSet rgbaChannels = getNeededChannels();

  Tile tile(input0(), 0, 0, width, height, rgbaChannels);
  if (aborted()) {
    return false;
  }

  vec.resize(width * height * 4);

  foreach(z, rgbaChannels) {

    int channel_skip = 0;
    if(z == Chan_Red)
      channel_skip = 0;
    else if(z == Chan_Green)
      channel_skip = 1;
    else if(z == Chan_Blue)
      channel_skip = 2;
    else if(z == Chan_Alpha)
      channel_skip = 3;

    for (int py = 0; py < height; ++py) {
      for (int px = 0; px < width; ++px) {
        const size_t index = (py*width + px)*4 + channel_skip;
        float value = tile[z][py][px];

        vec[index] = value;
      }
    }
  }

  return true;
}

void SimpleBlurCached::engine ( int y, int x, int r,
                              ChannelMask channels, Row& row )
{
  const int width = info_.w();
  const int height = info_.h();

  // engine calls are multi-threaded so any processing must be locked
  if (_isFirstTime) {
    Guard guard(_lock);
    if (_isFirstTime) {

      const size_t totalPixels = width * height * 4;

      std::vector<float> vec_src_image;
      vec_dst_image.resize(totalPixels);

      /* try fetch input image */
      if (fetchRGBAImage(vec_src_image, height, width) ) {
        /* now going to see if the input is in the cache */
        Image_Cache *i_cache = &Image_Cache::mainCache();
        printf("Checking active cache: %d.\n", i_cache->is_active());

        size_t desired_read_bytes = (vec_src_image.size())*sizeof(float);

        DD::Image::Hash hash;
        hash.reset();
        hash.append(input0().hash());
        hash.append(_param_blur);
        printf("Printing hash value: %d.\n", (int)hash.value());
        printf("Has file: %d.\n", i_cache->has_file(hash));

          /* is our blurred image already in the cache? */
        bool cache_read_success = true;
        if (i_cache->is_active() && i_cache->has_file(hash) ) {
          DD::Image::ImageCacheReadI* cache_read = i_cache->open( hash );

          size_t read_bytes = cache_read->read(vec_dst_image.data(), desired_read_bytes);

          if (!i_cache->is_read() || read_bytes != desired_read_bytes) {
            cache_read_success = false;
          }

          cache_read->close();
        }
        else {
          cache_read_success = false;
        }

        /* did we get the cached blur image or do we need to calculate it? */
        if (!cache_read_success) {

          /* blur the image, very naively, do not use this code for anything useful! */
          float blur_sum; int blur_counter;
          for(int i=0;i<height;i++)
            for(int j=0;j<width;j++)
              for(int c=0;c<4;c++){
                blur_sum = 0; blur_counter = 0;
                for (int u=std::max(0,i-_param_blur); u<=std::min(height-1,i+_param_blur); u++)
                  for (int v=std::max(0,j-_param_blur); v<=std::min(width-1,j+_param_blur); v++){
                    blur_sum += vec_src_image[(u*width + v)*4 + c];
                    blur_counter++;
                  }
                vec_dst_image[(i*width + j)*4 + c] = (blur_counter > 0) ? blur_sum / (float)blur_counter : 0;
              }

          /* write result to cache */
          DD::Image::ImageCacheWriteI* cache_write = i_cache->create( hash );
          size_t desired_write_bytes = (vec_dst_image.size())*sizeof(float);

          cache_write->write(vec_dst_image.data(), desired_write_bytes);

          if (!i_cache->is_written())
            printf("Error saving blurred image to cache (is written: %d).\n", (int)i_cache->is_written());

          cache_write->close();
        }

        /* now just going to add the constant to the image */
        for(size_t i = 0; i<vec_dst_image.size(); i++)
          vec_dst_image[i] = vec_dst_image[i] + _param_constant;

      }
      else {
        /* return on abort */
        return;
      }

      _isFirstTime = false;
    }
  }

  // Copy unprocessed channels through to the output
  // NOTE: this must happen first as .get(...) changes row
  const ChannelSet rgbaChannels = getNeededChannels();
  ChannelSet enginePass = channels;

  enginePass -= rgbaChannels;
  if (enginePass) {
    input0().get( y, x, r, enginePass, row );
  }

  // an example to set the output data, per image buffer
  ChannelSet engineOut = channels;
  engineOut &= rgbaChannels;

  if (engineOut) {
    foreach (z, channels) {
      float * row1 = row.writable(z);

      int channel_skip = 0;
      if(z == Chan_Red)
        channel_skip = 0;
      else if(z == Chan_Green)
        channel_skip = 1;
      else if(z == Chan_Blue)
        channel_skip = 2;
      else if(z == Chan_Alpha)
        channel_skip = 3;

      for(int xx=x; xx<r; xx++)
        row1[xx] = vec_dst_image[y*4*width + (xx-x)*4 + channel_skip];
    }
  }
}
