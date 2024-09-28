// Copyright (c) 2009 The Foundry Visionmongers Ltd.  All Rights Reserved.

static const char* const CLASS = "AddInputs";

static const char* const HELP =
  "Add all the inputs together";

// Standard plug-in include files.

#include "DDImage/Iop.h"
#include "DDImage/NukeWrapper.h"
using namespace DD::Image;
#include "DDImage/Row.h"
#include "DDImage/Tile.h"
#include "DDImage/Knobs.h"

using namespace std;

class AddInputs : public Iop
{
public:
  
  int minimum_inputs() const { return 2; }
  int maximum_inputs() const { return 2; }
  
  //! Constructor. Initialize user controls to their default values.

  AddInputs (Node* node) : Iop (node)
  {
  }

  ~AddInputs () {}
  
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
static Iop* AddInputsCreate(Node* node)
{
  return new AddInputs(node);
}

/*! The Iop::Description is how NUKE knows what the name of the operator is,
   how to create one, and the menu item to show the user. The menu item may be
   0 if you do not want the operator to be visible.
 */
const Iop::Description AddInputs::description ( CLASS, "Merge/AddInputs",
                                                     AddInputsCreate );


void AddInputs::_validate(bool for_real)
{
  copy_info(); // copy bbox channels etc from input0
  merge_info(1); // merge info from input 1
}

void AddInputs::_request(int x, int y, int r, int t, ChannelMask channels, int count)
{
  // request from input 0 and input 1
  input(0)->request( x, y, r, t, channels, count );
  input(1)->request( x, y, r, t, channels, count );
}


/*! For each line in the area passed to request(), this will be called. It must
   calculate the image data for a region at vertical position y, and between
   horizontal positions x and r, and write it to the passed row
   structure. Usually this works by asking the input for data, and modifying
   it.

 */
void AddInputs::engine ( int y, int x, int r,
                              ChannelMask channels, Row& row )
{
  // input 0 row
  row.get(input0(), y, x, r, channels);
 
  // input 1 row
  Row input1Row(x, r);
  input1Row.get(input1(), y, x, r, channels);
 
  foreach ( z, channels ) {
    const float* input1 = input1Row[z] + x;
    const float* input0  = row[z] + x;
    float* outptr = row.writable(z) + x;
    const float* end = outptr + (r - x);
 
    while (outptr < end) {
      *outptr++ = *input0++ + *input1++;
    }
  }
}
