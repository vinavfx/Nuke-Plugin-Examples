// Copyright (c) 2009 The Foundry Visionmongers Ltd.  All Rights Reserved.

// This is sample code for a plugin which uses Python to manipulate 3D geometry.
// It provides selectable handles for the vertices of incoming geometry as well as
// a text area where a user can enter a Python script and a button which runs the script.
//
// There's a couple of approaches to providing the input geometry to Python. The simple
// approach is to pass an array of points in a variable called "points". The more complex approach
// is to provide a wrapper class which gives access to more of the geometry. We do the latter here,
// but depending on what you're doing, you may be able to just pass simple Python objects to the script.

#include "PythonGeo.h"
#include <assert.h>
#include "structmember.h"

using namespace DD::Image;

//
// Global variables.
//

const char* const CLASS = "PythonGeo";
const char* const HELP = "@i;PythonGeo@n; An example plugin which manipulates 3D geometry using Python.";


//
// Macros
//

#define PyGeometryListObjectCheckValid(o) if (!o->_geo) { PyErr_SetString(PyExc_IndexError, "object out of scope"); return 0; }
#define PyGeoInfoObjectCheckValid(o)      if (!o->_geo || !o->_geo->_geo) { PyErr_SetString(PyExc_IndexError, "object out of scope"); return NULL; }
#define PythonGeoKnobObjectCheckValid(o)  if (!o->_knob) { PyErr_SetString(PyExc_IndexError, "object out of scope"); return NULL; }


extern PyTypeObject PyGeoInfo_Type;
extern PyTypeObject PyGeometryList_Type;
extern PyTypeObject PyPrimitive_Type;
extern PyTypeObject PythonGeoKnob_Type;


//
// GeometryList python definitions.
//

static PySequenceMethods GeometryList_as_sequence = {
	(lenfunc)GeometryList_slength, /*sq_length*/
	nullptr, /*sq_concat*/
	nullptr, /*sq_repeat*/
	(ssizeargfunc)GeometryList_item, /*sq_item*/
	nullptr, /*sq_slice*/
	nullptr,		/*sq_ass_item*/
	nullptr,		/*sq_ass_slice*/
	nullptr /*sq_contains*/
};

static PyMethodDef GeometryList_methods[] = {
	{ nullptr,	nullptr }
};

PyTypeObject PyGeometryList_Type = {
	PyObject_HEAD_INIT(&PyType_Type)
	"GeometryList",
	sizeof(PyGeometryListObject),
	0,
	GeometryList_dealloc,			/* tp_dealloc */
	nullptr,//(printfunc)GeometryList_print,		/* tp_print */
	nullptr,					/* tp_getattr */
	nullptr,					/* tp_setattr */
	nullptr,					/* tp_compare */
	nullptr,//(reprfunc)GeometryList_repr,			/* tp_repr */
	nullptr,    			/* tp_as_number */
	&GeometryList_as_sequence,				/* tp_as_sequence */
	nullptr,					/* tp_as_mapping */
	nullptr,//(hashfunc)GeometryList_hash, 		/* tp_hash */
	nullptr,					/* tp_call */
	nullptr,//(reprfunc)GeometryList_str,			/* tp_str */
	PyObject_GenericGetAttr,		/* tp_getattro */
	nullptr,					/* tp_setattro */
	nullptr,					/* tp_as_buffer */
	Py_TPFLAGS_DEFAULT, /* tp_flags */
  nullptr,				/* tp_doc */
	nullptr,					/* tp_traverse */
	nullptr,					/* tp_clear */
  nullptr,          /* tp_richcompare */
	0,					/* tp_weaklistoffset */
	nullptr,					/* tp_iter */
	nullptr,					/* tp_iternext */
	GeometryList_methods,			/* tp_methods */
	nullptr,          /* tp_members */
	nullptr,			/* tp_getset */
	nullptr,					/* tp_base */
	nullptr,					/* tp_dict */
	nullptr,					/* tp_descr_get */
	nullptr,					/* tp_descr_set */
	0,					/* tp_dictoffset */
	nullptr,					/* tp_init */
	PyType_GenericAlloc,			/* tp_alloc */
	GeometryList_new,				/* tp_new */
	PyObject_Del,           		/* tp_free */
};


//
// GeoInfo definitions
//

static PyMethodDef GeoInfo_methods[] = {
  { "points", (PyCFunction)GeoInfo_points, METH_NOARGS, "points" },
  { "primitives", (PyCFunction)GeoInfo_primitives, METH_NOARGS, "primitives" },
  { "normals", (PyCFunction)GeoInfo_normals, METH_NOARGS, "normals" },
	{ nullptr,	nullptr }
};

PyTypeObject PyGeoInfo_Type = {
	PyObject_HEAD_INIT(&PyType_Type)
	"Geometry",
	sizeof(PyGeoInfoObject),
	0,
	GeoInfo_dealloc,			/* tp_dealloc */
	nullptr,//(printfunc)GeoInfo_print,		/* tp_print */
	nullptr,					/* tp_getattr */
	nullptr,					/* tp_setattr */
	nullptr,					/* tp_compare */
	nullptr,			/* tp_repr */
	nullptr,    			/* tp_as_number */
	nullptr,				/* tp_as_sequence */
	nullptr,					/* tp_as_mapping */
	nullptr, 		/* tp_hash */
	nullptr,					/* tp_call */
	nullptr,			/* tp_str */
	PyObject_GenericGetAttr,		/* tp_getattro */
	nullptr,					/* tp_setattro */
	nullptr,					/* tp_as_buffer */
	Py_TPFLAGS_DEFAULT, /* tp_flags */
  nullptr,				/* tp_doc */
	nullptr,					/* tp_traverse */
	nullptr,					/* tp_clear */
  nullptr,          /* tp_richcompare */
	0,					/* tp_weaklistoffset */
	nullptr,					/* tp_iter */
	nullptr,					/* tp_iternext */
	GeoInfo_methods,			/* tp_methods */
	nullptr,          /* tp_members */
	nullptr,			/* tp_getset */
	nullptr,					/* tp_base */
	nullptr,					/* tp_dict */
	nullptr,					/* tp_descr_get */
	nullptr,					/* tp_descr_set */
	0,					/* tp_dictoffset */
	nullptr,					/* tp_init */
	PyType_GenericAlloc,/* tp_alloc */
	GeoInfo_new,				/* tp_new */
	PyObject_Del,       /* tp_free */
};


//
// PythonGeo_Knob python definitions
//
static PyMethodDef PythonGeoKnob_methods[] = {
  { "getGeometry",  PythonGeoKnob_getGeometry,  METH_VARARGS, "getGeometry() -> geometry list." },
  { "getSelection", PythonGeoKnob_getSelection, METH_VARARGS, "getSelection() -> list of selected geometry indices." },
  { nullptr, nullptr, 0, nullptr }
};

PyTypeObject PythonGeoKnob_Type = {
	PyObject_HEAD_INIT(&PyType_Type)
	"PythonGeo_Knob",
	sizeof(PythonGeoKnobObject),
	0,
	PythonGeoKnob_dealloc,			/* tp_dealloc */
	nullptr,		/* tp_print */
	nullptr,					/* tp_getattr */
	nullptr,					/* tp_setattr */
	nullptr,					/* tp_compare */
	nullptr,			/* tp_repr */
	nullptr,    			/* tp_as_number */
	nullptr,				/* tp_as_sequence */
	nullptr,					/* tp_as_mapping */
	nullptr, 		/* tp_hash */
	nullptr,					/* tp_call */
	nullptr,			/* tp_str */
	PyObject_GenericGetAttr,		/* tp_getattro */
	PyObject_GenericSetAttr,					/* tp_setattro */
	nullptr,					/* tp_as_buffer */
	Py_TPFLAGS_DEFAULT, /* tp_flags */
  nullptr,				/* tp_doc */
	nullptr,					/* tp_traverse */
	nullptr,					/* tp_clear */
  nullptr,          /* tp_richcompare */
	0,					/* tp_weaklistoffset */
	nullptr,					/* tp_iter */
	nullptr,					/* tp_iternext */
	PythonGeoKnob_methods,			/* tp_methods */
	nullptr,          /* tp_members */
	nullptr,			/* tp_getset */
	nullptr,					/* tp_base */
	nullptr,					/* tp_dict */
	nullptr,					/* tp_descr_get */
	nullptr,					/* tp_descr_set */
	0,					/* tp_dictoffset */
	nullptr,					/* tp_init */
	PyType_GenericAlloc,			/* tp_alloc */
	PythonGeoKnob_new,				/* tp_new */
	PyObject_Del,           		/* tp_free */
};


//
// Python wrapper classes for 3D geometry
//

// These are very basic and only serve as an example of how we could do this.
// The plugin provides the Python script with a class "GeometryList" which behaves as a sequence
// containing "Geometry" instances. "Geometry" has methods to return the Primitives as a tuple containing
// tuples of vertices, the points as a tuple of XYZ triples and the normals in the same format. It would be better
// to have wrapper classes for these as sequences as well so that a script could just pull out, say, primitive 0
// without having to create the whole massive tuple, but time is pressing and we don't want to make this example
// too complicated. We should also add a class for Primitives as so on....

// We manage the lifetime of the GeometryList class by the primitive expedient of making it invalid once the
// script returns. This prevents Python retaining a pointer onto geometry which may be freed unexpectedly.

//
// GeometryList python functions
//

PyObject *GeometryList_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
	if ( !PyArg_ParseTuple( args, ":GeometryList" ) )
		return nullptr;

	PyGeometryListObject *o = (PyGeometryListObject *) PyObject_MALLOC(sizeof(PyGeometryListObject));
	if (o == nullptr)
		return PyErr_NoMemory();
	PyObject_INIT( o, &PyGeometryList_Type );
	o->_geo = nullptr;
	return (PyObject *)o;
}

PyGeometryListObject *PyGeometryListObject_FromGeometryList( GeometryList *geometryList )
{
  PyGeometryListObject *o = (PyGeometryListObject *) PyObject_MALLOC(sizeof(PyGeometryListObject));
  PyObject_INIT( o, &PyGeometryList_Type );
  o->_geo = geometryList;
  return o;
}

void GeometryList_dealloc( PyObject *o )
{
}

Py_ssize_t GeometryList_slength( PyGeometryListObject *o )
{
	PyGeometryListObjectCheckValid(	o );
  return o->_geo->size();
}

PyObject *GeometryList_item( PyGeometryListObject *o, Py_ssize_t i )
{
	PyGeometryListObjectCheckValid(	o );
	if ( i < 0 || i >= (Py_ssize_t)o->_geo->size() ) {
		PyErr_SetString(PyExc_IndexError, "index out of range");
		return nullptr;
	}
  return (PyObject *)PyGeoInfoObject_FromGeoInfo( o, static_cast<int>(i) );
}


//
// GeoInfo python functions
//

PyObject *GeoInfo_new( PyTypeObject *type, PyObject *args, PyObject *kwds )
{
	if ( !PyArg_ParseTuple( args, ":Geometry" ) )
		return nullptr;

	PyGeoInfoObject *o = (PyGeoInfoObject *) PyObject_MALLOC(sizeof(PyGeoInfoObject));
	if (o == nullptr)
		return PyErr_NoMemory();
	PyObject_INIT( o, &PyGeoInfo_Type );
	o->_geo = nullptr;
	return (PyObject *)o;
}


PyGeoInfoObject *PyGeoInfoObject_FromGeoInfo( PyGeometryListObject *geometryList, int index )
{
  PyGeoInfoObject *o = (PyGeoInfoObject *) PyObject_MALLOC(sizeof(PyGeoInfoObject));
  PyObject_INIT( o, &PyGeoInfo_Type );
  o->_geo = geometryList;
  o->_index = index;
  Py_INCREF( o->_geo );
  return o;
}


void GeoInfo_dealloc( PyObject *o )
{
  Py_XDECREF( ((PyGeoInfoObject*)o)->_geo );
}


PyObject *GeoInfo_points( PyGeoInfoObject *self, PyObject *args, PyObject *kwds ) {
	PyGeoInfoObjectCheckValid( self );
  const GeoInfo& info = (*self->_geo->_geo)[self->_index];
  const PointList* points = info.point_list();
  size_t n = points->size();

  PyObject *list = PyTuple_New( n*3 );
  size_t j = 0;
  for ( size_t i = 0; i < n; i++ ) {
    const Vector3& v = (*points)[i];
    PyTuple_SetItem( list, j++, PyFloat_FromDouble( v.x ) );
    PyTuple_SetItem( list, j++, PyFloat_FromDouble( v.y ) );
    PyTuple_SetItem( list, j++, PyFloat_FromDouble( v.z ) );
  }
  return list;
}

PyObject *GeoInfo_primitives( PyGeoInfoObject *self, PyObject *args, PyObject *kwds ) {
	PyGeoInfoObjectCheckValid( self );
  const GeoInfo& info = (*self->_geo->_geo)[self->_index];
  const Primitive** PRIMS = info.primitive_array();
  const unsigned prims = info.primitives();
  PyObject *list = PyTuple_New( prims );
  for ( unsigned i = 0; i < prims; i++ ) {
    const Primitive* prim = *PRIMS++;
    PyObject *vertices = PyTuple_New( prim->vertices() );
    for ( unsigned j = 0; j < prim->vertices(); j++ )
      PyTuple_SetItem( vertices, j, PyLong_FromLong( prim->vertex(j) ) );
//    Py_INCREF( vertices );
    PyTuple_SetItem( list, i, vertices );
  }
  return list;
}

PyObject *GeoInfo_normals( PyGeoInfoObject *self, PyObject *args, PyObject *kwds ) {
	PyGeoInfoObjectCheckValid( self );
  const GeoInfo& info = (*self->_geo->_geo)[self->_index];
  const AttribContext* N_attrib  = info.get_attribcontext("N");
  if (N_attrib &&
      (!N_attrib->attribute || !N_attrib->attribute->size()))
    N_attrib = nullptr;
  if (N_attrib) {
    // TODO - here we need to worry about thwether we're dealing with point or vertex normals.
    // For the purposes of this example, I'm assuming point normals.
    //    if (N_group == Group_Vertices)
    //    else if (N_group == Group_Points)
    unsigned normals = N_attrib->attribute->size();
    PyObject *list = PyTuple_New( normals*3 );
    int j = 0;
    for ( unsigned i = 0; i < normals; i++ ) {
      Vector3 N = N_attrib->attribute->normal(i);
      PyTuple_SetItem( list, j++, PyFloat_FromDouble( N.x ) );
      PyTuple_SetItem( list, j++, PyFloat_FromDouble( N.y ) );
      PyTuple_SetItem( list, j++, PyFloat_FromDouble( N.z ) );
    }
    return list;
  }
  Py_RETURN_NONE;
}


//
// PythonGeo_Knob methods.
//


const char* PythonGeo_Knob::Class() const
{
  return "PythonGeo_Knob";
}


PythonGeo_Knob::PythonGeo_Knob(DD::Image::Knob_Closure *kc, PythonGeo *pgOp, const char* n) :
  Knob(kc, n),
  PluginPython_KnobI(),
  _pgOp(pgOp),
  _scene(nullptr)
{
  setPythonType(&PythonGeoKnob_Type);
  set_flag( DO_NOT_WRITE );
}


PythonGeo_Knob::~PythonGeo_Knob()
{
  delete _scene;
}


bool PythonGeo_Knob::build_handle(ViewerContext* ctx)
{
  return ctx->transform_mode() != VIEWER_2D;
}


void PythonGeo_Knob::draw_handle(ViewerContext* ctx) {
  // All we do here is create a selectable viewer handle for each point in the incoming geometry.
  // To go further, we could provide handles for edges and faces, or whatever.
  if ( ctx->draw_knobs() && _pgOp->_allowSelection ) {
    Scene *scene = _pgOp->scene();
    GeometryList& out = *scene->object_list();

    _selection.clear();
    int startPoint = 0;
    for ( unsigned obj = 0; obj < out.size(); obj++ ) {
      GeoInfo& info = out[obj];
      const PointList* points = info.point_list();
      const int n = static_cast<int>(points->size());
      std::vector<unsigned int> objSelection;
      for ( int i = 0; i < n; i++ ) {
        const Vector3 v = (*points)[i];
        make_handle( SELECTABLE, ctx, handleCallback, i + startPoint, v.x, v.y, v.z, ViewerContext::kCrossCursor );
        // Here we save the list of selected handles. This is done way too often, but we need a ViewerContext
        // to find out if a handle is selected, so we can't just query the selection when the user clicks on the
        // button.
        if ( is_selected( ctx, handleCallback, i + startPoint ) )
          objSelection.push_back( i );
      }
      startPoint = n;
      _selection.push_back( objSelection );
    }
  }
}


/* This was never defined...?
bool PythonGeo_Knob::isHandleSelected(int i)
{
}
*/

PyObject* PythonGeo_Knob::getGeometry()
{
  if (_pgOp == nullptr)
    Py_RETURN_NONE;

  GeoOp *myOp = dynamic_cast<GeoOp*>( _pgOp->node_input( 0, Op::EXECUTABLE_SKIP ) );

  if ( !myOp ) 
    Py_RETURN_NONE;

  delete _scene;
  _scene = new Scene();
  myOp->validate(true);
  myOp->build_scene( *_scene );
  
  GeometryList* out = _scene->object_list();
  if (out == nullptr)
    Py_RETURN_NONE;

  PyGeometryListObject* geometryList = PyGeometryListObject_FromGeometryList(out);
  if (geometryList == nullptr)
    Py_RETURN_NONE;

  return (PyObject*)geometryList;
}


PyObject* PythonGeo_Knob::getSelection()
{
  PyObject* objList = PyTuple_New(_selection.size());
  for (unsigned int curObj = 0; curObj < _selection.size(); ++curObj) {
    std::vector<unsigned int>& selPoints = _selection[curObj];
    PyObject* selectionList = PyTuple_New(selPoints.size());
    for (unsigned int i = 0; i < selPoints.size(); ++i)
      PyTuple_SetItem(selectionList, i, PyLong_FromLong(selPoints[i]));
    PyTuple_SetItem(objList, curObj, selectionList);
  }
  return objList;
}


PluginPython_KnobI* PythonGeo_Knob::pluginPythonKnob()
{
  return this;
}


bool PythonGeo_Knob::handleCallback(ViewerContext* ctx, Knob* p, int index)
{
  // We're not handling any events for this example, but we could, say, provide a popup menu when
  // a user clicks on a point, or call our Python script.
  return false;
}


// A helper function to create the custom knob
PythonGeo_Knob* PythonGeo_knob(Knob_Callback f, PythonGeo* pyGeo, const char* name)
{
  if ( f.makeKnobs() ) {
    PythonGeo_Knob* knob = new PythonGeo_Knob(&f, pyGeo, name);
    f(DD::Image::PLUGIN_PYTHON_KNOB, Custom, knob, name, nullptr, pyGeo);
    return knob;
  } else {
    f(DD::Image::PLUGIN_PYTHON_KNOB, Custom, nullptr, name, nullptr, pyGeo);
    return nullptr;
  }
}


PyObject* PythonGeoKnob_new(PyTypeObject* type, PyObject* args, PyObject* kwargs)
{
  Py_RETURN_NONE;
}


void PythonGeoKnob_dealloc(PyObject *o)
{
}


PyObject* PythonGeoKnob_getGeometry(PyObject* self, PyObject* args)
{
  PythonGeoKnobObject* knob = (PythonGeoKnobObject*)self;
  PythonGeoKnobObjectCheckValid(knob);
  if (knob->_knob == nullptr)
    Py_RETURN_NONE;
  return knob->_knob->getGeometry();
}


PyObject* PythonGeoKnob_getSelection(PyObject* self, PyObject* args)
{
  PythonGeoKnobObject* knob = (PythonGeoKnobObject*)self;
  PythonGeoKnobObjectCheckValid(knob);
  if (knob->_knob == nullptr)
    Py_RETURN_NONE;
  return knob->_knob->getSelection();
}


//
// PythonGeo methods
//

// We subclass ModifyGeo although we're not modifying anything. The same techniques can be used
// to call Python to implement geometry modification and that's what this example originally did,
// hence the superclass.
PythonGeo::PythonGeo(Node *node) : ModifyGeo(node), _allowSelection(true)
{
}


void PythonGeo::knobs(Knob_Callback f)
{
  ModifyGeo::knobs(f);
  Bool_knob( f,  &_allowSelection, "allowSelection" );
  SetFlags( f, Knob::STARTLINE );
  PythonGeo_knob( f, this, "geo" );
}


//! Hash up knobs that may affect points
void PythonGeo::get_geometry_hash()
{
  ModifyGeo::get_geometry_hash(); // Get all hashes up-to-date
  geo_hash[Group_Points].append(Op::hash());
  geo_hash[Group_Points].append(_allowSelection);
}


void PythonGeo::modify_geometry(int obj, Scene& scene, GeometryList& out)
{
  // We don't do anything here, but we could call our Python script to modify the geometry if we wanted.
}


Op* build(Node *node)
{
  return new PythonGeo(node);
}

const Op::Description PythonGeo::description(CLASS, build);


