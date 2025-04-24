
#include <Python.h>
#include <tcl.h>

// Should stay the same as the defines in tohil.c
#define TOHIL_TCL_INTERP_STASH_NAME "_tohil_interp"
#define TCL_TCL_INTERP_CAPSULE_NAME "tohil.interp"

// === Capsule Retrieval ===
static Tcl_Interp *get_stashed_tcl_interp() {
    PyObject *main = PyImport_AddModule("__main__");
    if (!main) return NULL;

    PyObject *capsule = PyObject_GetAttrString(main, TOHIL_TCL_INTERP_STASH_NAME);
    if (!capsule) return NULL;

    Tcl_Interp *interp = (Tcl_Interp *)PyCapsule_GetPointer(capsule, TCL_TCL_INTERP_CAPSULE_NAME);
    Py_DECREF(capsule);
    return interp;
}

// === Loader Object ===
typedef struct {
    PyObject_HEAD
    PyObject *fullname;  // module name
} VFSLoaderObject;

static PyObject *vfs_loader_new(PyTypeObject *type, PyObject *args, PyObject *kwds) {
    VFSLoaderObject *self = (VFSLoaderObject *)type->tp_alloc(type, 0);
    if (self) self->fullname = NULL;
    return (PyObject *)self;
}

static int vfs_loader_exec_module(PyObject *self, PyObject *module) {
    VFSLoaderObject *loader = (VFSLoaderObject *)self;
    if (!loader->fullname) {
        PyErr_SetString(PyExc_RuntimeError, "Loader has no module name");
        return -1;
    }

    Tcl_Interp *interp = get_stashed_tcl_interp();
    if (!interp) {
        PyErr_SetString(PyExc_RuntimeError, "Tcl interpreter not found");
        return -1;
    }

    const char *name = PyUnicode_AsUTF8(loader->fullname);
    char tcl_path[256];
    snprintf(tcl_path, sizeof(tcl_path), "/vfs/%s.py", name);

    Tcl_Obj *pathObj = Tcl_NewStringObj(tcl_path, -1);
    Tcl_IncrRefCount(pathObj);

    Tcl_Channel chan = Tcl_FSOpenFileChannel(interp, pathObj, "r", 0);
    if (!chan) {
        Tcl_DecrRefCount(pathObj);
        PyErr_Format(PyExc_ImportError, "Could not open %s in Tcl VFS", tcl_path);
        return -1;
    }

    Tcl_SetChannelOption(interp, chan, "-encoding", "utf-8");

    Tcl_Obj *contents;
    if (Tcl_ReadChars(chan, contents = Tcl_NewObj(), -1, 0) == -1) {
        Tcl_DecrRefCount(pathObj);
        PyErr_SetString(PyExc_ImportError, "Failed to read from VFS file");
        return -1;
    }

    Tcl_Close(interp, chan);
    Tcl_DecrRefCount(pathObj);

    const char *source = Tcl_GetString(contents);
    PyObject *code = Py_CompileString(source, tcl_path, Py_file_input);
    if (!code) return -1;

    PyObject *result = PyEval_EvalCode(code, PyModule_GetDict(module), PyModule_GetDict(module));
    Py_DECREF(code);
    Py_XDECREF(result);
    return result ? 0 : -1;
}

static PyTypeObject VFSLoaderType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "vfsloader.Loader",
    .tp_basicsize = sizeof(VFSLoaderObject),
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_new = vfs_loader_new,
    .tp_methods = NULL,
    .tp_members = NULL,
};

// === Finder Object ===
typedef struct {
    PyObject_HEAD
} VFSFinderObject;

static PyObject *vfs_finder_find_spec(PyObject *self, PyObject *args, PyObject *kwds) {
    const char *name;
    if (!PyArg_ParseTuple(args, "s", &name))
        return NULL;

    char vfs_path[256];
    snprintf(vfs_path, sizeof(vfs_path), "/vfs/%s.py", name);

    Tcl_Interp *interp = get_stashed_tcl_interp();
    if (!interp) Py_RETURN_NONE;

    Tcl_Obj *exists = Tcl_NewStringObj(vfs_path, -1);
    Tcl_IncrRefCount(exists);
    int file_exists = Tcl_FSAccess(exists, F_OK) == 0;
    Tcl_DecrRefCount(exists);

    if (!file_exists)
        Py_RETURN_NONE;

    PyObject *loader = PyObject_CallObject((PyObject *)&VFSLoaderType, NULL);
    if (!loader) return NULL;

    ((VFSLoaderObject *)loader)->fullname = PyUnicode_FromString(name);

    PyObject *importlib = PyImport_ImportModule("importlib.machinery");
    PyObject *ModuleSpec = PyObject_GetAttrString(importlib, "ModuleSpec");

    PyObject *spec = PyObject_CallFunction(ModuleSpec, "sO", name, loader);
    Py_DECREF(ModuleSpec);
    Py_DECREF(importlib);
    Py_DECREF(loader);
    return spec;
}

static PyTypeObject VFSFinderType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "vfsloader.Finder",
    .tp_basicsize = sizeof(VFSFinderObject),
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_new = PyType_GenericNew,
    .tp_methods = (PyMethodDef[]){
        {"find_spec", (PyCFunction)vfs_finder_find_spec, METH_VARARGS, "Find spec"},
        {NULL}
    },
};

static PyObject *SourceLoaderType = NULL;

// TODO:
// - Load the importlib.abc.SourceLoader to use as a base class
// - Implement methods:
//     (a) path_mtime(self, path): return modification time as int.
//     (b) get_data(self, path): return data from path as raw bytes.
// === Register with sys.meta_path ===
int
register_vfs_importer()
{
    PyObject * importlib_abc = PyImport_ImportModule("importlib.abc");
    if (importlib_abc == NULL)
	return -1;

    SourceLoaderType = PyObject_GetAttrString(importlib_abc, "SourceLoader");
    Py_DECREF(importlib_abc);

    if (SourceLoaderType == NULL)
	return -1;

    if (! PyType_Check(SourceLoaderType)) {
	PyErr_SetString(PyExc_TypeError, "SourceLoader is not a type object");
	Py_DECREF(SourceLoaderType);
	SourceLoaderType = NULL;
	return -1;
    }

    VFSLoaderType.tp_base = (PyTypeObject *) SourceLoaderType;
    Py_INCREF(SourceLoaderType); /* PyType_Ready will steal this reference */

    if (PyType_Ready(&VFSFinderType) < 0) return -1;
    if (PyType_Ready(&VFSLoaderType) < 0) return -1;

    PyObject *finder = PyObject_CallObject((PyObject *)&VFSFinderType, NULL);
    if (!finder) return -1;

    PyObject *sys = PyImport_ImportModule("sys");
    PyObject *meta_path = PyObject_GetAttrString(sys, "meta_path");

    PyList_Insert(meta_path, 0, finder);  /* insert at front */

    Py_DECREF(meta_path);
    Py_DECREF(sys);
    Py_DECREF(finder);
}
