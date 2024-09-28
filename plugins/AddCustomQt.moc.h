#ifndef HAVE_MYWIDGET_MOC_H
#define HAVE_MYWIDGET_MOC_H
#include "DDImage/Knobs.h"
#include <QtCore/QObject>
#include <QtWidgets/QDial>



class MyKnob;

/// Example of a custom knob
class MyWidget : public QDial 
{
  Q_OBJECT

  public:
    MyWidget(MyKnob* knob);
    ~MyWidget();

    void update();
    void destroy();
    static int WidgetCallback(void* closure, DD::Image::Knob::CallbackReason reason);

  public Q_SLOTS:
    void valueChanged(int value);

  private:
    MyKnob* _knob;
};
#endif
