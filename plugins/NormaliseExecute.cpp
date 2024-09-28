// Copyright (c) 2009 The Foundry Visionmongers Ltd.  All Rights Reserved.

static const char* const CLASS = "NormaliseExecute";

static const char* const HELP =
  "Example Normalises input to 1.0 over a frame range";

// Standard plug-in include files.

#include "DDImage/Iop.h"
#include "DDImage/NukeWrapper.h"
#include "DDImage/Row.h"
#include "DDImage/Tile.h"
#include "DDImage/Knobs.h"
#include "DDImage/Thread.h"
#include "DDImage/Executable.h"

using namespace std;
using namespace DD::Image;

#ifdef FN_OS_WINDOWS
  // Disable "'this' : used in base member initializer list" warning
  // Executable ctor only copies the pointer so ok in this case
  #pragma warning(disable:4355)
#endif

class NormaliseExecute : public Iop, Executable
{
  double _maxValue;
  double   _calcMaxValue;
  
  bool _firstTime;
  Lock _lock;
  
public:

  int maximum_inputs() const { return 1; }
  int minimum_inputs() const { return 1; }
  
  //! Constructor. Initialize user controls to their default values.

  NormaliseExecute (Node* node) : Iop (node), Executable(this)
  {
    _maxValue = 0;
    _calcMaxValue = 0;
  }

  ~NormaliseExecute () {}
  
  void _validate(bool);
  void _request(int x, int y, int r, int t, ChannelMask channels, int count);
  void _open();
  void knobs(Knob_Callback f );

  /// executable methods
  
  void beginExecuting();
  void endExecuting();
  void execute();
  virtual Executable* executable() { return this; }

  
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
static Iop* NormaliseExecuteCreate(Node* node)
{
  return new NormaliseExecute(node);
}

/*! The Iop::Description is how NUKE knows what the name of the operator is,
   how to create one, and the menu item to show the user. The menu item may be
   0 if you do not want the operator to be visible.
 */
const Iop::Description NormaliseExecute::description ( CLASS, "Merge/NormaliseExecute",
                                                     NormaliseExecuteCreate );


void NormaliseExecute::_validate(bool for_real)
{
  copy_info(); // copy bbox channels etc from input0, which will validate it.
  
}

void NormaliseExecute::_request(int x, int y, int r, int t, ChannelMask channels, int count)
{
  // request all input input as we are going to search the whole input area
  ChannelSet readChannels = input0().info().channels();
  input(0)->request( readChannels, count );
}

void NormaliseExecute::_open()
{
}

void NormaliseExecute::beginExecuting()
{
  std::cerr << "Begin Executing." << std::endl;
  _maxValue = _calcMaxValue  = 0;
}

void NormaliseExecute::endExecuting()
{
  std::cerr <<"End Executing." << std::endl;
  knob("maxValue")->set_value( _calcMaxValue );
}  


void NormaliseExecute::execute()
{
  // do anaylsis for current frame
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
        _calcMaxValue = std::max( (float)*CUR, (float)_calcMaxValue );
        CUR++;
      }
    }
  }

}


void NormaliseExecute::knobs( Knob_Callback f ) {
   Float_knob(f,  &_maxValue, IRange(0,5), "maxValue", "Max");
   Divider(f);
   const char* renderScript = "currentNode = nuke.toNode(\"this\")\n"
   "nodeList = [currentNode]\n"
   "nukescripts.render_panel(nodeList, False)\n";
   PyScript_knob(f, renderScript, "Get Max Value");
}

/*! For each line in the area passed to request(), this will be called. It must
   calculate the image data for a region at vertical position y, and between
   horizontal positions x and r, and write it to the passed row
   structure. Usually this works by asking the input for data, and modifying
   it.

 */
void NormaliseExecute::engine ( int y, int x, int r,
                              ChannelMask channels, Row& row )
{
  Row in( x,r);
  in.get( input0(), y, x, r, channels );
  if ( aborted() )
    return;
  
  foreach( z, channels ) {
    float *CUR = row.writable(z) + x;
    const float* inptr = in[z] + x;
    const float *END = row[z] + r;
    while ( CUR < END ) {
        *CUR++ = *inptr++ * ( 1.0f / static_cast<float>(_maxValue ));
    }
  }
}
