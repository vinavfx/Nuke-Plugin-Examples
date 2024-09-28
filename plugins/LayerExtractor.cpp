#include "DDImage/Row.h"
#include "DDImage/Iop.h"
#include "DDImage/Pixel.h"
#include "DDImage/Knobs.h"
#include <DDImage/Enumeration_KnobI.h>

#include <sstream>
#include <algorithm>
#include <map>

using namespace DD::Image;

const char *staticChoices[3] = 
{
    "static1",
    "static2",
    0
};


static const char* const CLASS = "LayerExtractor";
static const char* const HELP = "Example that shows how to use dynamic enumeration knobs";


class LayerExtractor : public Iop
{
private:
    Format *customFormat;

    // knob values
    const char *selectedIDs;
	
    int selectedLayer;
    Knob *selectedLayerKnob;

protected:
    void _validate(bool for_real);
    void _request(int x, int y, int r, int t, ChannelMask c1, int count);

public:
    virtual void knobs(Knob_Callback);

    static const Description description;
    const char* Class() const { return description.name; }
    const char* node_help() const { return HELP; }

    LayerExtractor(Node* node);
    ~LayerExtractor();

    void engine(int y, int x, int r, ChannelMask c1, Row& out_row);
	
    void add_layer_knobs(Knob_Callback f);

};

void LayerExtractor::_validate(bool for_real)
{
	copy_info();
  delete customFormat;
  customFormat = new Format(256, 256);
  info_.format(*customFormat);
  info_.full_size_format(*customFormat);
  info_.x(0);
  info_.y(0);
  info_.r(256);
  info_.t(256);
  info_.turn_on(Mask_RGBA);

	ChannelSet channels = input0().channels();
	std::vector<std::string> channelNames ;
	foreach ( z, channels ) {
		channelNames.push_back( channel_name(z) );
	}
							   
	// based upon the input channels, make a new list
	Enumeration_KnobI* pSelectLayerEnum = selectedLayerKnob->enumerationKnob();
	pSelectLayerEnum->menu( channelNames );
	
	// check if the current selectLayer is out of range and reset.
    if ( selectedLayer >= (int) channelNames.size() ) {
		selectedLayerKnob->set_value(0);
    }
   

}


void LayerExtractor::_request(int x, int y, int r, int t, ChannelMask c1, int count)
{
  input0().request(x, y, r, t, c1 ,count);
}

LayerExtractor::LayerExtractor(Node* node) : Iop(node)
{
    customFormat = NULL;
    selectedIDs = NULL;
    selectedLayer = 0;
}

LayerExtractor::~LayerExtractor()
{
    delete customFormat;
    customFormat = NULL;
}

void LayerExtractor::engine(int y, int x, int r, ChannelMask c1, Row& out_row)
{

  out_row.get( input0(), y, x, r, c1 );

  return;
}

void LayerExtractor::knobs(Knob_Callback f)
{
  Knob * knob = Enumeration_knob(f, &selectedLayer, staticChoices, "selected_layer", "selected layer");
	if( knob ) {
		selectedLayerKnob = knob;
	}
	SetFlags(f,  Knob::SAVE_MENU );
}

static Op* build(Node* node) { return new LayerExtractor(node); }
const Op::Description LayerExtractor::description("LayerExtractor", build);
