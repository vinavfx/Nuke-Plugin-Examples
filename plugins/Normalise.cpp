// Copyright (c) 2009 The Foundry Visionmongers Ltd.  All Rights Reserved.

static const char* const CLASS = "Normalise";

static const char* const HELP =
  "Example normalises input to 1.0";

// Standard plug-in include files.

#include "DDImage/Iop.h"
#include "DDImage/NukeWrapper.h"
using namespace DD::Image;
#include "DDImage/Row.h"
#include "DDImage/Tile.h"
#include "DDImage/Knobs.h"
#include "DDImage/Thread.h"


using namespace std;

class Normalise : public Iop
{
  float _maxValue;
  bool _firstTime;
  Lock _lock;
  
public:

  int maximum_inputs() const { return 1; }
  int minimum_inputs() const { return 1; }
  
  //! Constructor. Initialize user controls to their default values.

  Normalise (Node* node) : Iop (node)
  {
    _maxValue = 0;
    _firstTime = true;
  }

  ~Normalise () {}
  
  void _validate(bool);
  void _request(int x, int y, int r, int t, ChannelMask channels, int count);
  void _open();
  
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
static Iop* NormaliseCreate(Node* node)
{
  return new Normalise(node);
}

/*! The Iop::Description is how NUKE knows what the name of the operator is,
   how to create one, and the menu item to show the user. The menu item may be
   0 if you do not want the operator to be visible.
 */
const Iop::Description Normalise::description ( CLASS, "Merge/Normalise",
                                                     NormaliseCreate );


void Normalise::_validate(bool for_real)
{
  copy_info(); // copy bbox channels etc from input0, which will validate it.
}

void Normalise::_request(int x, int y, int r, int t, ChannelMask channels, int count)
{
  // request all input input as we are going to search the whole input area
  ChannelSet readChannels = input0().info().channels();
  input(0)->request( readChannels, count );
}

void Normalise::_open()
{
  _firstTime = true;
}


/*! For each line in the area passed to request(), this will be called. It must
   calculate the image data for a region at vertical position y, and between
   horizontal positions x and r, and write it to the passed row
   structure. Usually this works by asking the input for data, and modifying
   it.

 */
void Normalise::engine ( int y, int x, int r,
                              ChannelMask channels, Row& row )
{
  {
    Guard guard(_lock);
    if ( _firstTime ) {
      // do anaylsis.
      Format format = input0().format();

      // these useful format variables are used later
      const int fx = format.x();
      const int fy = format.y();
      const int fr = format.r();
      const int ft = format.t();

      ChannelSet readChannels = input0().info().channels();

      Interest interest( input0(), fx, fy, fr, ft, readChannels, true );
      interest.unlock();
      
      // fetch each row and find the highest number pixel
      _maxValue = 0; 
      for ( int ry = fy; ry < ft; ry++) {
        progressFraction( ry, ft - fy );
        Row row( fx, fr );
        row.get( input0(), ry, fx, fr, readChannels );
        if ( aborted() )
          return;
          
        foreach( z, readChannels ) {
          const float *CUR = row[z] + fx;
          const float *END = row[z] + fr;
          while ( CUR < END ) {
            _maxValue = std::max( (float)*CUR, _maxValue );
            CUR++;
          }
        }
      }
      _firstTime = false;
    }
  } // end lock
  
  Row in( x,r);
  in.get( input0(), y, x, r, channels );
  if ( aborted() )
    return;
  
  foreach( z, channels ) {
    float *CUR = row.writable(z) + x;
    const float* inptr = in[z] + x;
    const float *END = row[z] + r;
    while ( CUR < END ) {
        *CUR++ = *inptr++ * ( 1.0f / _maxValue );
    }
  }
}
