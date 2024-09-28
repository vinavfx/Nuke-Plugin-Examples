#include "AddCustomQt.moc.h"

#include "DDImage/NukeWrapper.h"
#include "DDImage/PixelIop.h"
#include "DDImage/Row.h"
#include "DDImage/Knobs.h"

#include <QtWidgets/QTabWidget>

#include <sstream>

using namespace DD::Image;

static const char* const HELP = "Adds a constant to a set of channels";

class MyKnob : public DD::Image::Knob 
{
  friend class MyWidget;
  
  int _data;

public:

  MyKnob(DD::Image::Knob_Closure* kc, int* data, const char* n) : Knob(kc, n)
  {
    _data = 0;
    if ( data )
      _data = *data;
  }

  virtual const char* Class() const { return "MyKnob"; }

  virtual bool not_default () const { return _data != 0; }

  virtual void 	to_script (std::ostream &os, const OutputContext *, bool quote) const 
  {
    std::cerr << "to script " << std::endl;
    if (quote) {
      std::stringstream ss;
      ss << _data;
      os << cstring( ss.str().c_str() );
    }
    else
      os << _data;
  }

  virtual bool from_script(const char *	v)  
  {
    std::cerr << "from script " << std::endl;
    std::stringstream ss(v);
    ss >> _data;
    changed();
    return true;
  }

  void store(StoreType type, void* data, Hash &hash, const OutputContext &oc) 
  {
    int* destData = (int*)data;
    *destData = _data;
    hash.append(_data);
  }
  
  void setValue(int value) 
  {
    new_undo ( "setValue" );
    _data = value;
    changed();
  }
  
  virtual WidgetPointer make_widget(const DD::Image::WidgetContext& context) 
  {
    MyWidget* widget = new MyWidget( this );
    return widget;
  }
};


MyWidget::MyWidget(MyKnob* knob) : _knob(knob)
{
  setNotchesVisible(true);
  setWrapping(false);
  _knob->addCallback(WidgetCallback, this);
  connect(this, SIGNAL(valueChanged(int)), this, SLOT(valueChanged(int)));
}

MyWidget::~MyWidget()
{
  if ( _knob ) 
    _knob->removeCallback( WidgetCallback, this );
}

void MyWidget::valueChanged(int value)
{
  std::cerr << "valueChanged" << std::endl;
  _knob->setValue( value );
}
 
void MyWidget::update() 
{
  std::cerr << "update  " << _knob->_data << std::endl;
  setValue( _knob->_data );
}

void MyWidget::destroy() 
{
  _knob = 0;
}

int MyWidget::WidgetCallback(void* closure, Knob::CallbackReason reason)
{
  // could double check if on main thread here just in case and bail out
  // if not
  MyWidget* widget = (MyWidget*)closure;
  assert(widget);
  switch (reason) {
    case Knob::kIsVisible:
      {
        // We check for visibility up to the containing tab widget. 
        // This means that a widget is still considered visible when its NodePanel is hidden due to being in hidden tab in a dock.

        for (QWidget* w = widget->parentWidget(); w; w = w->parentWidget())
          if (qobject_cast<QTabWidget*>(w))
            return widget->isVisibleTo(w);
        return widget->isVisible();
      }

    case Knob::kUpdateWidgets:
      widget->update();
      return 0;

    case Knob::kDestroying:
      widget->destroy();
      return 0;

    default:
      return 0;
  }
}

class AddCustomQt : public PixelIop
{
  int _value;
  public:
  void in_channels(int input, ChannelSet& mask) const;
  AddCustomQt(Node* node) : PixelIop(node)
  {
    _value = 0;
  }

  bool pass_transform() const { return true; }
  void pixel_engine(const Row &in, int y, int x, int r, ChannelMask, Row & out);
  virtual void knobs(Knob_Callback);
  static const Iop::Description d;
  const char* Class() const { return d.name; }
  const char* node_help() const { return HELP; }
  void _validate(bool);
};

void AddCustomQt::_validate(bool for_real)
{
  copy_info();
  if (_value) {
    set_out_channels(Mask_All);
    info_.black_outside(false);
    return;
  }
  set_out_channels(Mask_None);
}

void AddCustomQt::in_channels(int input, ChannelSet& mask) const
{
  // mask is unchanged
}

void AddCustomQt::pixel_engine(const Row& in, int y, int x, int r,
                       ChannelMask channels, Row& out)
{
  foreach (z, channels) {
    const float c = _value/100.0f;
    const float* inptr = in[z] + x;
    const float* END = inptr + (r - x);
    float* outptr = out.writable(z) + x;
    while (inptr < END)
      *outptr++ = *inptr++ + c;
  }
}

void AddCustomQt::knobs(Knob_Callback f)
{
  // a custom knob with custom data
  CustomKnob1(MyKnob, f, &_value, "gain");
}

static Iop* build(Node* node) { return new NukeWrapper(new AddCustomQt(node)); }
const Iop::Description AddCustomQt::d("AddCustomQt", "Color/Math/AddCustomQt", build);
