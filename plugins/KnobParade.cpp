// NoIop creating an instance of the vast majority of all NDK accessible knobs.
// Copyright (c) 2011 The Foundry Visionmongers Ltd.  All Rights Reserved.

#include "DDImage/NoIop.h"
#include "DDImage/Knobs.h"
#include "DDImage/Knob.h"

#include "DDImage/Convolve.h"       //Defines ConvolveArray struct used in Array_knob.

#include "DDImage/ChannelSet.h"
#include "DDImage/Channel.h"        //Defines the ChannelSets, ChannelMasks and Channels used in the various channel_knobs.

#include "DDImage/Format.h"         //Defines the FormatPair class used by Format_knob.

#include "DDImage/Matrix4.h"        //Defines the Matrix4 class used by Transform2d_knob.

#include "DDImage/LookupCurves.h"   //Defines the LookupCurves class used by the LookupCurves_knob.

#include "DDImage/TableKnobI.h"     //Defines the table knob interface used by, surprisingly enough, the Table_knob.

#include "DDImage/HistogramKnob.h"  //Defines the Histogram_Data class used by the Histogram_knob.

#include "DDImage/Colorspace_KnobI.h"

using namespace DD::Image;

static const char* const HELP =
  "Creates one of every sort of NDK supported knobs";

static const char* const        enumerationKnobNames[]  = { "list entry 1", "list entry 2", 0 };
static const char* const        bitmaskKnobNames[]      = { "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", 0 };
static const CurveDescription   lookupCurvesDefaults[]  = { { "master", "y C 0 1" }, {0} };
static const char* const        pulldownKnobEntries[]   = { "list entry 1", "panel entry1", 0 };
static const char* const        pyPulldownKnobEntries[] = { "list entry 1", "nuke.message(\"entry1\")", 0 };
static const char* const        cascadingEnumerationKnobNames[] = { "title1/entry1", "title1/entry2", "title2/entry1", 0 };

typedef std::vector< std::vector<std::string> > ListStore;   //Convenience for List_knob.
typedef std::map<int, std::string> DynamicBitmaskName;       //Convenience for Dynamic_Bitmask_knob.

class KnobParade : public NoIop
{
  //Define data storage member variables for all Knob's requiring them.
  const char*         _stringKnob;
  const char*         _fileKnob;
  int                 _intKnob;
  int                 _enumKnob;
  unsigned int        _bitmaskKnob;
  bool                _boolKnob;
  double              _doubleKnob;
  float               _floatKnob;
  ConvolveArray       _arrayKnob;
  ChannelSet          _channelSetKnob;
  ChannelSet          _channelMaskKnob;
  ChannelSet          _inputChannelSetKnob;
  ChannelSet          _inputChannelMaskKnob;
  Channel             _channelKnob[4];
  Channel             _inputChannelKnob[4];
  float               _xyKnob[2];
  float               _xyzKnob[3];
  float               _whKnob[2];
  float               _bboxKnob[4];
  FormatPair          _formatKnob;
  float               _colorKnob[3];
  float               _aColorKnob[4];
  Matrix4             _transform2dKnob;
  const char*         _multilineStringKnob;
  Matrix4             _axisKnob;
  float               _uvKnob[2];
  float               _box3Knob[6];
  LookupCurves        _lookupCurvesKnob;
  float               _eyedropperKnob[8];
  float               _rangeKnob[8];
  double              _keyerKnob[4];
  unsigned int        _colorChipKnob;
  int                 _colorspaceKnob;
  double              _scaleKnob[2];
  int                 _oneViewKnob;
  std::set<int>       _multiViewKnob;
  std::map<int, int>  _viewViewKnob;
  std::pair<int, int> _viewPairKnob;
  ListStore           _listKnob;
  double              _pixelAspectKnob;
  const char*         _passwordKnob;
  int                 _toolboxKnob;
  int                 _cascadingEnumerationKnob;
  DynamicBitmaskName  _dynamicBitmaskName;
  unsigned int        _dynamicBitmaskKnob;
  float               _positionVectorKnob[6];
  const char*         _cachedFileKnob;
  int                 _multiIntKnob[6];
  float               _multiFloatKnob[6];
  const char*         _writeFileKnob;
  Histogram_Data*     _histogramKnob;

  public:
  //Initialise members to their default values in the Op constructor.
  KnobParade(Node* node) : NoIop(node),
  _stringKnob(""),
  _fileKnob(""),
  _intKnob(0),
  _enumKnob(0),
  _bitmaskKnob(0),
  _boolKnob(false),
  _doubleKnob(0.0f),
  _floatKnob(0.0f),
  _arrayKnob(),
  _channelSetKnob(Chan_Black),
  _channelMaskKnob(Chan_Black),
  _inputChannelSetKnob(Chan_Black),
  _inputChannelMaskKnob(Chan_Black),
  _multilineStringKnob(""),
  _lookupCurvesKnob(lookupCurvesDefaults),
  _colorChipKnob(0),
  _colorspaceKnob(0),
  _oneViewKnob(0),
  _pixelAspectKnob(1.0f),
  _passwordKnob(""),
  _toolboxKnob(0),
  _cascadingEnumerationKnob(0),
  _dynamicBitmaskKnob(0),
  _cachedFileKnob(""),
  _writeFileKnob("")
  {
    for (int i=0; i<8; i++) { 
      _eyedropperKnob[i]    =   0.0f;
      _rangeKnob[i]         =   (1.0f/8.0f)*(float)i;
    }

    for (int i=0; i<6; i++) {
      _multiIntKnob[i]     =   0;
      _multiFloatKnob[i]   =   0.0f;
    }

    for (int i=0; i<4; i++) {
      _channelKnob[i]       =   Chan_Black;
      _inputChannelKnob[i]  =   Chan_Black;
      _bboxKnob[i]          =   0.0f;
      _aColorKnob[i]        =   0.0f;
    }

    for (int i=0; i<3; i++) {
      _xyzKnob[i]           =   0.0f;
      _colorKnob[i]         =   0.0f;
    }

    for (int i=0; i<2; i++) {
      _xyKnob[i]            =   0.0f;
      _whKnob[i]            =   0.0f;
      _uvKnob[i]            =   0.0f;
      _scaleKnob[i]         =   1.0f;
    }

    _formatKnob.format(0);                //Get default script format.
    _transform2dKnob.makeIdentity();      //Zero to no transform.
    _axisKnob.makeIdentity();

    _box3Knob[0] = 0.0f; _box3Knob[1] = 0.0f; _box3Knob[2] = 0.0f;
    _box3Knob[3] = 1.0f; _box3Knob[4] = 1.0f; _box3Knob[5] = 1.0f;

    _keyerKnob[0] = 0.0f; _keyerKnob[1] = 0.2f; 
    _keyerKnob[2] = 0.8f; _keyerKnob[3] = 1.0f;

    for (int i=1; i<outputContext().viewcount(); i++) {
      _multiViewKnob.insert(i);                           //Add all the views currently in the script, starting from index 1 (which avoids the 'default' view).
      _viewViewKnob.insert(std::pair<int, int>(i, i));    //Add all views mapped to themselves.
    }

    if (outputContext().viewcount()>0) {
      _viewPairKnob.first  = 1;
      _viewPairKnob.second = 1;
    }

    std::vector<std::string> tempVec1, tempVec2;
    tempVec1.push_back("vec1 entry1"); tempVec1.push_back("vec1 entry2");
    tempVec2.push_back("vec2 entry1"); tempVec2.push_back("vec2 entry1");
    _listKnob.push_back(tempVec1);     _listKnob.push_back(tempVec2);

    _dynamicBitmaskName[0] = "a";
    _dynamicBitmaskName[1] = "b";
    _dynamicBitmaskName[3] = "d";

    _positionVectorKnob[0] = 0.0f; _positionVectorKnob[1] = 0.0f; _positionVectorKnob[2] = 0.0f;
    _positionVectorKnob[3] = 1.0f; _positionVectorKnob[4] = 1.0f; _positionVectorKnob[5] = 1.0f;

    //Fill Histogram_knob data structure with (in this case linear in) info.
    _histogramKnob = new Histogram_Data;
    int j = 0;
    while (j<100) {
      for(int i=0; i<j; i++) _histogramKnob->addLumIn(((float)j)/100.0f);
      j++;
    }
  }

  ~KnobParade() {
    //Special case for Histogram_knobs - the data store is reference counted, so decrement on Op destruction.
    _histogramKnob->removeUser();
  }

  bool test_input(int n, Op* op) const { 
    return true;
  }

  void knobs(Knob_Callback f)
  {
    Tab_knob(f, "Data");

    //No slider by default
    Int_knob(f, &_intKnob, "Int_knob");

    Float_knob(f, &_floatKnob, "Float_knob");

    Double_knob(f, &_doubleKnob, "Double_knob");

    UV_knob(f, _uvKnob, "UV_knob");      

    //Uses the convolveArray struct for data storage. 
    Array_knob(f, &_arrayKnob, 3, 3, "Array_knob");

    MultiInt_knob(f, _multiIntKnob, 6, "MultiInt_knob");

    MultiFloat_knob(f, _multiFloatKnob, 6, "MultiFloat_knob");

    Divider(f, "Color");

    //Can use Vector3 class of data storage - pass the address of the Vector3.x element
    Color_knob(f, _colorKnob, "Color_knob");

    //Can use Vector4 class for data storage
    AColor_knob(f, _aColorKnob, "AColor_knob");

    //8-bit rrggbb00 store. Use from_sRGB() on these to get float.
    ColorChip_knob(f, &_colorChipKnob, "ColorChip_knob");
    SetFlags(f, Knob::STARTLINE); //STARTLINE not set by default.

    //Specialised Enumeration_knob to handle OCIO colorspaces; "scene_linear" = OCIO::SCENE_LINEAR
    Knob* colorspace = Colorspace_knob(f, &_colorspaceKnob, "scene_linear", "Colorspace_knob");
    if (f.makeKnobs() && colorspace) {
      colorspace->colorspaceKnob()->configChanged(true);
    }

    Divider(f, "String");

    String_knob(f, &_stringKnob, "String_knob");

    File_knob(f, &_fileKnob, "File_knob");

    Cached_File_knob(f, &_cachedFileKnob, "Cached_File_knob");

    //Stores 2 filenames - one for proxy and one for full res.
    Write_File_knob(f, &_writeFileKnob, "Write_File_knob");

    Multiline_String_knob(f, &_multilineStringKnob, "Multiline_String_knob");

    //Only char**, no string version. Generally more aimed at Python Panel use.
    Password_knob(f, &_passwordKnob, "Password_knob");

    Divider(f, "Selection");

    //Intro to bitmasks.
    Bitmask_knob(f, &_bitmaskKnob, bitmaskKnobNames, "Bitmask_knob");

    //Does not have startline set by default.
    Bool_knob(f, &_boolKnob, "Bool_knob"); SetFlags(f, Knob::STARTLINE);

    Tab_knob(f, "Transforms");

    //Double & float variants
    XY_knob(f, _xyKnob, "XY_knob");

    //Only float
    XYZ_knob(f, _xyzKnob, "XYZ_knob");

    //Double & float variants
    WH_knob(f, _whKnob, "WH_knob");

    //Double & float variants
    BBox_knob(f, _bboxKnob, "BBox_knob");

    //Initialisation using FormatPair and .format
    Format_knob(f, &_formatKnob, "Format_knob");

    Box3_knob(f, _box3Knob, "Box3_knob");

    //Allows range setting on construction.
    //Double variant only.
    Scale_knob(f, _scaleKnob, "Scale_knob");

    //Automatically takes proxy scale handling into account when
    //dealing with proxy scales of a different aspect ratio.
    PixelAspect_knob(f, &_pixelAspectKnob, "PixelAspect_knob");

    //Float only variant
    //Draws arrow in 3d view.
    PositionVector_knob(f, _positionVectorKnob, "PositionVector_knob");

    Divider(f, "Transform2d_knob");

    Transform2d_knob(f, &_transform2dKnob, "Transform2d_knob");

    Divider(f, "Axis_knob");

    //Matrix can be got from the knob called 'matrix'
    Axis_knob(f, &_axisKnob, "Axis_knob");



    Tab_knob(f, "Buttons Menus and Lists");

    Divider(f, "Buttons");

    //Use knob_changed and check against knob name to implement action
    Button(f, "Button"); SetFlags(f, Knob::STARTLINE);

    Script_knob(f, "puts test", "Script_knob"); SetFlags(f, Knob::STARTLINE);

    PyScript_knob(f, "", "PyScript_knob"); SetFlags(f, Knob::STARTLINE);

    Divider(f, "Menus");

    Enumeration_knob(f, &_enumKnob, enumerationKnobNames, "Enumeration_knob");

    //Two parts to each enum entry - first is title, second is the actual command
    Pulldown_knob(f, pulldownKnobEntries, "Pulldown_knob");

    PyPulldown_knob(f, pyPulldownKnobEntries, "PyPulldown_knob");

    //The menu name argument should be a python created menu. In this case 
    //we'll reuse the snapping menu seen on Axis derived nodes.
    Menu_knob(f, "Axis/Snap", "Menu_knob");

    CascadingEnumeration_knob(f, &_cascadingEnumerationKnob, cascadingEnumerationKnobNames, "CascadingEnumeration_knob");

    Dynamic_Bitmask_knob(f, &_dynamicBitmaskKnob, &_dynamicBitmaskName, "Dynamic_Bitmask_knob");

    Divider(f, "Lists");

    List_knob(f, &_listKnob, "List_knob");

    //Handles data storage and mem management itself. Needs to have relevant
    //cols added on creation only (hence f.makeKnobs check).
    Knob* tableKnob = Table_knob(f, "Table_knob");
    if (f.makeKnobs()) {
      Table_KnobI* tableKnobI = tableKnob->tableKnob();
      tableKnobI->addColumn("col1", "col1", Table_KnobI::FloatColumn, true);
      tableKnobI->addColumn("col1", "col1", Table_KnobI::FloatColumn, true);
      
      if(tableKnobI->getRowCount() == 0) {
        tableKnobI->addRow();
      }
    }



    Tab_knob(f, "Layout Chans and Views");

    BeginGroup(f, "BeginGroup");
    BeginClosedGroup(f, "BeginClosedGroup");
    EndGroup(f);
    EndGroup(f);

    //Named and unnamed versions for access in knob_changed/etc for hiding.
    //STARTLINE not set by default.
    Text_knob(f, "Text_knob"); SetFlags(f, Knob::STARTLINE);

    //No labelling, and no impact at start of line - using Text_knob in addition. 
    //STARTLINE not set by default.
    Text_knob(f, " "); SetFlags(f, Knob::STARTLINE); Spacer(f, 250); Text_knob(f, "<-Spacer");
    Newline(f); Text_knob(f, "<-Newline"); Newline(f);

    Divider(f, "Divider");

    Help_knob(f, "Help_knob"); Text_knob(f, "<-Help_knob");

    //Use the Knob::TOOLBAR_* enumerations to position around the sides of the viewer.
    //Be wary of what knobs you put inside a toolbar - it's only really designed for
    //containing small entry knobs, enumerations, toolboxes and buttons (using the SMALLUI knob).
    BeginToolbar(f, "BeginToolbar/EndToolbar", "BeginToolbar/EndToolbar", Knob::TOOLBAR_LEFT);
    EndToolbar(f);

    BeginTabGroup(f, "BeginTabGroup/\nEndTabGroup");
    Tab_knob(f, "Tab_knob");
    EndTabGroup(f);

    //ExoGroups are groups of controls which appear on every tab. See the rotopaint
    //node for an example of this. Without anything contained within the group
    //nothing will appear on the panel. It's not a group in the conventional sense
    //as it does not the embedded params in twirlies or tabs.
    Text_knob(f, "BeginExoGroup/EndExoGroup");
    BeginExoGroup(f, "BeginExoGroup/EndExoGroup");
    //Text_knob(f, "Filler");
    EndExoGroup(f);

    Divider(f, "Channels");

    //All variants on CHANNEL_MASK_KNOB. All script and input only channel variants.
    //TODO: mask vs set.
    ChannelSet_knob(f, &_channelSetKnob, "ChannelSet_knob");
    ChannelMask_knob(f, &_channelMaskKnob, "ChannelMask_knob");
    Input_ChannelSet_knob(f, &_inputChannelSetKnob, 0, "Input_ChannelSet_knob");
    Input_ChannelMask_knob(f, &_inputChannelMaskKnob, 0, "Input_ChannelMask_knob");

    //Both variants on CHANNEL_KNOB. All script and input only channel variants.
    Channel_knob(f, _channelKnob, 4, "Channel_knob");
    Input_Channel_knob(f, _inputChannelKnob, 4, 0, "Input_Channel_knob");

    Divider(f, "Views");

    //Initialising view knobs
    OneView_knob(f, &_oneViewKnob, "OneView_knob");

    MultiView_knob(f, &_multiViewKnob, "MultiView_knob");

    std::pair<std::string, std::string>   viewViewLabels("in", "out");
    ViewView_knob(f, &_viewViewKnob, "ViewView_knob", &viewViewLabels);

    ViewPair_knob(f, &_viewPairKnob, "ViewPair_knob");



    Tab_knob(f, "Curves");

    //Can pass a type enum to define the curve drawing style, including options for
    //colouring background and plots plus synchronising/wrapping curve data.
    Text_knob(f, "LookupCurves_knob"); SetFlags(f, Knob::STARTLINE);
    LookupCurves_knob(f, &_lookupCurvesKnob, "LookupCurves_knob"); ClearFlags(f, Knob::STARTLINE);

    Tab_knob(f, "Histogram");

    //Uses the Histogram_Data class for data storage.
    Histogram_knob(f, _histogramKnob, "Histogram_knob");

    Tab_knob(f, "Specialist");

    //Needs a 8 array. Stores 4 sampled colours plus x, y, r, t for sampled area.
    //Use of knob_changed to be able to grab at other channels.
    Text_knob(f, "Eyedropper_knob"); SetFlags(f, Knob::STARTLINE);
    Eyedropper_knob(f, _eyedropperKnob, "Eyedropper_knob"); ClearFlags(f, Knob::STARTLINE);

    //Float and double variants.
    Range_knob(f, _rangeKnob, 8, "Range_knob");

    //Double only, no float variant.
    Keyer_knob(f, _keyerKnob, "Keyer_knob");

    //Takes on the characteristics of whatever its linked to
    Link_knob(f, "Float_knob", "Link_knob");

    //View only knob detailing the metadata on the incoming stream.
    //Used by ViewMetadata node. ModifyMetaData uses List_knob.
    MetaData_knob(f, "MetaData_knob");

    //Manages its own data storage.
    MetaKeyFrame_knob(f, "MetaKeyFrame_Knob");

    //Convenience knob. Use RippleKnobI header to access methods.
    Ripple_knob(f, "Ripple_knob");


    //Knobs not available or not recommended for use.
    //MultiArray_knob - not recommended. Look into Table knobs. 
    //Python_knob - used by the Python API. Not for use via NDK.
    //CP_knob - deprecated.
    //PLUGIN_PYTHON_KNOB - see PythonGeo example for usage example.
    //Toolbox_knob - relies on compiled in icons, as opposed to those included in the icon path.
    //ControlPointCollection_knob - part of WIP control point/roto abstraction.
    //TransformJack_knob. WIP. Viewer handle only transform jack, with adjustable display elements.

    //Knobs not demonstrated but available for use.
    //GeoSelect_knob. Only available on GeoOps, and one already created by parent class under the name 'geo_select'.
  }

  void _validate(bool for_real)
  {
    NoIop::_validate(for_real);
  }

  const char* node_help() const { return HELP; }

  static const Iop::Description d;
  const char* Class() const { return d.name; }
};

static Iop* build(Node* node) { return new KnobParade(node); }
const Iop::Description KnobParade::d("KnobParade", 0, build);
