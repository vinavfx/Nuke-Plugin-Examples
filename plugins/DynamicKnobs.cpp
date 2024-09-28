// Copyright (c) 2011 The Foundry Visionmongers Ltd.  All Rights Reserved.

static const char* const CLASS = "DynamicKnobs";

static const char* const HELP = "Demonstrates simplest case use of the "
  "replace_knobs functionality available. "
  "See the NDK Reference Guide for more information.";

#include "DDImage/NoIop.h"
#include "DDImage/Knobs.h"
#include "DDImage/Row.h"

using namespace DD::Image;

class DynamicKnobs : public NoIop
{
  bool    _showDynamic;   //Storage for our static knob. This controls whether dynamic knobs are shown.
  int     _numNewKnobs;   //Used to track the number of knobs created by the previous pass, so that the same number can be deleted next time.
  float   _dynamicKnob;   //Storage for our dynamic knob. Normally this would be dynamic (ie heap allocated) itself, but shown as local for simplicity.
  public:
  DynamicKnobs(Node* node) : NoIop(node),
  _showDynamic(false),
  _numNewKnobs(0),
  _dynamicKnob(0.0f)
  {
  }

  virtual void knobs(Knob_Callback);
  virtual int  knob_changed(Knob*);
  static void addDynamicKnobs(void*, Knob_Callback);                                 //Our add knobs callback, used by add_knobs & replace_knobs.
  bool    getShowDynamic()      const { return (knob("show_dynamic")->get_value() != 0.0); }  //Because KNOB_CHANGED_ALWAYS set, can't use _showDynamic directly.
  float*  getDynamicKnobStore()       { return &_dynamicKnob; }                      //Get for dynamic knob store. As above, simplified.
  const char* Class()           const { return CLASS; }
  const char* node_help()       const { return HELP; }
  static Iop::Description d;
};

void DynamicKnobs::knobs(Knob_Callback f)
{
  Bool_knob(f, &_showDynamic, "show_dynamic", "Show Dynamic");
  SetFlags(f, Knob::KNOB_CHANGED_ALWAYS);

  //If you were creating knobs by default that would be replaced, this would need to 
  //be called to create those. 
  //_numNewKnobs = add_knobs(addDynamicKnobs, this->firstOp(), f);    

  //Call the callback manually this once to ensure script loads have the appropriate knobs to load into.
  if(!f.makeKnobs())
    DynamicKnobs::addDynamicKnobs(this->firstOp(), f);
}

int DynamicKnobs::knob_changed(Knob* k)
{
  if(k==&Knob::showPanel || k->is("show_dynamic")) {
    _numNewKnobs = replace_knobs(knob("show_dynamic"), _numNewKnobs, addDynamicKnobs, this->firstOp());
    return 1;
  }
  return NoIop::knob_changed(k);
}       

void DynamicKnobs::addDynamicKnobs(void* p, Knob_Callback f) 
{
  if(((DynamicKnobs*)p)->getShowDynamic()) {
    Float_knob(f, ((DynamicKnobs*)p)->getDynamicKnobStore(), "dynamic_knob", "Dynamic Knob");
  }   
}

static Iop* build(Node* node) { return new DynamicKnobs(node); }
Iop::Description DynamicKnobs::d(CLASS, 0, build);
