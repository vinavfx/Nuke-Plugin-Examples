// Copyright (c) 2009 The Foundry Visionmongers Ltd.  All Rights Reserved.

static const char* const CLASS = "SimpleBlur";

static const char* const HELP =
  "Does a simple box blur";

// Standard plug-in include files.

#include "DDImage/Iop.h"
#include "DDImage/NukeWrapper.h"
using namespace DD::Image;
#include "DDImage/Row.h"
#include "DDImage/Tile.h"
#include "DDImage/Knobs.h"

using namespace std;

class SimpleBlur : public Iop
{

  int _size;
  
public:

  int maximum_inputs() const { return 1; }
  int minimum_inputs() const { return 1; }
  
  //! Constructor. Initialize user controls to their default values.

  SimpleBlur (Node* node) : Iop (node)
  {
    _size = 20;
  }

  ~SimpleBlur () {}
  
  void _validate(bool);
  void _request(int x, int y, int r, int t, ChannelMask channels, int count);
  
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
  return new SimpleBlur(node);
}

/*! The Iop::Description is how NUKE knows what the name of the operator is,
   how to create one, and the menu item to show the user. The menu item may be
   0 if you do not want the operator to be visible.
 */
const Iop::Description SimpleBlur::description ( CLASS, "Merge/SimpleBlur",
                                                     SimpleBlurCreate );


void SimpleBlur::_validate(bool for_real)
{
  copy_info(); // copy bbox channels etc from input0, which will validate it.
  info_.pad( _size);
  
}

void SimpleBlur::_request(int x, int y, int r, int t, ChannelMask channels, int count)
{
  // request extra pixels around the input
  input(0)->request( x - _size , y - _size , r + _size, t + _size, channels, count );
}


/*! For each line in the area passed to request(), this will be called. It must
   calculate the image data for a region at vertical position y, and between
   horizontal positions x and r, and write it to the passed row
   structure. Usually this works by asking the input for data, and modifying
   it.

 */
void SimpleBlur::engine ( int y, int x, int r,
                              ChannelMask channels, Row& row )
{
 
  // make a tile for current line with padding arond for the blur
  Tile tile( input0(), x - _size , y - _size , r + _size, y + _size , channels);  
  if ( aborted() ) {
    std::cerr << "Aborted!";
    return;
  }
  

  foreach ( z, channels ) {
    float* outptr = row.writable(z) + x;
    for( int cur = x ; cur < r; cur++ ) {
      float value = 0;
      float div = 0;
  
      if ( intersect( tile.channels(), z ) ) {  
        // a simple box blur
        for ( int px = -_size; px < _size; px++ ) {
          for ( int py = -_size; py < _size; py++ ) { 
            value += tile[z][ tile.clampy(y + py)][ tile.clampx(cur + px) ];
            div++;
          }
        }
        if ( div )
          value /= div;
      }
      *outptr++ = value;
    }
  }
}
