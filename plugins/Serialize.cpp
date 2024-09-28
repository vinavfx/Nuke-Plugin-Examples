#include <DDImage/Iop.h>
#include <DDImage/Thread.h>
#include <DDImage/Row.h>
#include <DDImage/Tile.h>
#include <DDImage/Knobs.h>
#include <DDImage/ViewerContext.h>
#include <DDImage/Pixel.h>
#include <DDImage/gl.h>

#include <iostream>

#include <stdio.h>

using namespace DD::Image;


class CallbackHandler 
{
  public:
    CallbackHandler() {}
    virtual ~CallbackHandler() {}
    virtual bool load(std::string loadString){ return true; }
    virtual void save(std::string &saveString){}
};


class SerializeKnob : public Knob 
{
  private:
    CallbackHandler * _host;
    const char *Class() const { return "SerializeKnob"; }

  public:
    SerializeKnob(Knob_Closure *kc, CallbackHandler * host, const char* n): Knob(kc, n),_host(host)
    {}

    bool not_default() const 
    {
      return true;
    }

    virtual bool from_script(const char* v) 
    {
      std::string loadString(v);

      printf("Function from_script() called, with param: %s.\n", loadString.c_str());

      if (_host && loadString!="") {
        bool success = false;	

        try {
          success = _host->load(loadString);
          if(!success)
            error("Failed to load from script");
        } catch (const char * msg) {
          printf("Failed to load from script: %s\n", msg);
        } catch (...) {
          error("Failed to load from script");
        }

        return success;
      }

      return true;
    }

    virtual void to_script(std::ostream& theStream, const OutputContext*, bool quote) const 
    {

      printf("Function to_script() called, with \"quote\" param: %d.\n", quote);

      std::string saveString;

      if (_host) {
        try {
          _host->save( saveString );
        }catch (const char * msg) {
          printf("Failed to save to script: %s\n", msg);
        }catch (...) {
          error("Failed to save to script");
        }
      }

      if(quote) {
        saveString.insert(saveString.begin(),'\"');
        saveString+="\"";
      }

      printf("\tSaved data: %s.\n", saveString.c_str());

      theStream << saveString;
    }
};


////////////////////////////////////////////////////////////////////////////////////////////////////////////

class Serialize : public Iop, public CallbackHandler
{
  protected:
    Lock _lock;
    bool _isFirstTime;
    ChannelSet _requestChannels, _outChannels;
    SerializeKnob * _serializeKnob;
    std::string important_data_to_keep;

    static const char* const _className;
    static const char* const _helpString;

  public:
    static const Description d;
    const char* Class() const { return _className; }
    const char* node_help() const { return _helpString; }

    Serialize(Node * node):
      Iop(node), _serializeKnob(NULL), important_data_to_keep("Hello World!")
    {
      // Set the channels this plugin needs to do any processing
      _requestChannels = Mask_RGBA;
      // Set the channels this plugin creates 
      _outChannels = Mask_RGBA;
    }

    //////////////////////////////////////////////////////////////////////////////////////////////////////////
    // Inputs

    virtual const char* input_label(int n, char*) const 
    {
      switch (n) {
        case 0: return "Source";
        default: return "";
      }
    }

    virtual bool test_input(int input, DD::Image::Op* op) const 
    {
      switch (input) {
        case 0: return dynamic_cast<DD::Image::Iop*>(op) != 0;
      }
      return false;
    }

    virtual Op* default_input(int input) const 
    {
      switch (input) {
        case 0: return Iop::default_input(this);
      }
      return 0;
    }

    virtual int maximum_inputs() const { return 1; }
    virtual int minimum_inputs() const { return 1; }


    //////////////////////////////////////////////////////////////////////////////////////////////////////////
    // Validate, request, engine

    virtual void _invalidate() 
    {
      Iop::_invalidate();
    }

    virtual void _validate(bool for_real) 
    {
      copy_info();
      for(int i=0; i<inputs(); i++) {
        Op * thisInput = input(i);
        if(thisInput) {
          thisInput->validate(for_real);
        }
      }
      set_out_channels( Mask_All );

    }

    virtual void in_channels(int, ChannelSet& mask) const 
    {
      mask += _requestChannels;
    }

    virtual void  _request (int x, int y, int r, int t, ChannelMask channels, int count) 
    {
      for (int n = 0; n < inputs(); n++) {
        Iop * thisInput = dynamic_cast<Iop*>(Op::input(n));
        ChannelSet in = channels;
        in_channels(n, in);
        if (thisInput && in) {
          thisInput->request(in, 0);
        }
      }
    }

    virtual void engine( int y,int x,int r, ChannelMask channels, Row& row ) {
      // Copy unprocessed channels through to the output
      // NOTE: this must happen first as .get(...) changes row
      ChannelSet enginePass = channels;
      enginePass -= _outChannels;
      if (enginePass) {	
        /* printing out which channels are just being copied */
        std::cout << "enginePass: " << enginePass << std::endl;

        input0().get( y, x, r, enginePass, row );
      }

      // an example to set the output data, per image buffer
      ChannelSet engineOut = channels;
      engineOut &= _outChannels;

      if (engineOut) {
        /* printing out diagnostic information */
        //std::cout << "engineOut: " << engineOut << std::endl;

        foreach (z, engineOut) {
          float * row1 = row.writable(z);
          for(int xx=x; xx<r; xx++)
            row1[xx] = 0;
        }
      }

    }

    //////////////////////////////////////////////////////////////////////////////////////////////////////////
    // Parameters

    virtual void knobs( Knob_Callback f ) 
    {
      _serializeKnob = CustomKnob1(SerializeKnob, f, this, "serializeKnob");
    }

    int knob_changed(Knob* k) 
    {
      return Iop::knob_changed(k);
    }  

    /* functions for loading and saving data in the script */
    bool load(std::string loadString)
    {
      /* going to pretend that we've de-serialized loadString
         and are copying the meaningful results back into
         "important_data_to_keep" */

      important_data_to_keep = loadString;

      printf("Trying to load \"%s\" from script.\n", loadString.c_str());

      return true;
    }

    void save(std::string &saveString)
    {
      /* if we were using boost to save the data */
      //std::ostringstream archive_stream;
      //boost::archive::text_oarchive archive(archive_stream);
      //archive << test;
      //std::string outbound_data_ = archive_stream.str();

      /* saving some stuff to the string */
      //saveString += outbound_data_;//"Hello World!";

      /* instead we're going to pretend "important_data_to_keep"
         already contains serialized data */
      saveString += important_data_to_keep; 
      /* i.e. saveString += "Hello World!"; */

      printf("Trying to save \"%s\" to script.\n", saveString.c_str());

    }
};

const char* const Serialize::_className = "Serialize";
const char* const Serialize::_helpString = "Serialize: Simply saves and loads a text string to and from the current Nuke script.";

static Iop* c(Node *node) { return new Serialize( node ); }
const Iop::Description Serialize::d( _className, 0, c );
