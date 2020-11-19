//    This is a component of AXIS, a front-end for emc
//    Copyright 2004, 2005, 2006 Jeff Epler <jepler@unpythonic.net> and
//    Chris Radek <chris@timeguy.com>
//
//    This program is free software; you can redistribute it and/or modify
//    it under the terms of the GNU General Public License as published by
//    the Free Software Foundation; either version 2 of the License, or
//    (at your option) any later version.
//
//    This program is distributed in the hope that it will be useful,
//    but WITHOUT ANY WARRANTY; without even the implied warranty of
//    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//    GNU General Public License for more details.
//
//    You should have received a copy of the GNU General Public License
//    along with this program; if not, write to the Free Software
//    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA


#include <Python.h>
#include "py3c/py3c.h"
#include <structmember.h>

#include <google/protobuf/message_lite.h>

#include <machinekit/machinetalk/protobuf/types.pb.h>
#include <machinekit/machinetalk/protobuf/message.pb.h>
using namespace google::protobuf;

#include "rs274ngc.hh"
#include "rs274ngc_interp.hh"
#include "interp_return.hh"
#include "canon.hh"
#include "config.h"		// LINELEN

#include "czmq.h"
#include "pbutil.hh" // hal/haltalk

#include "rtapi_math.h"


static zsock_t *z_preview, *z_status;
static const char *istat_topic = "status";
static int batch_limit = 100;
static const char *p_client = "preview"; //NULL; // single client for now

static machinetalk::Container istat, output;

static size_t n_containers, n_messages, n_bytes;


// #define REPLY_TIMEOUT 3000 //ms
// #define UPDATE_TIMEOUT 3000 //ms

int _task = 0; // control preview behaviour when remapping

// publish an interpreter status change.
static void publish_istat(machinetalk::InterpreterStateType state)
{
    static machinetalk::InterpreterStateType last_state = machinetalk::INTERP_STATE_UNSET;
    int retval;

    if (state ^ last_state) {
	istat.set_type(machinetalk::MT_INTERP_STAT);
	istat.set_interp_state(state);
    istat.set_interp_name("preview");

	// NB: this will also istat.Clear()
	retval = send_pbcontainer(istat_topic, istat, z_status);
	assert(retval == 0);

	last_state = state; // change tracking
    }
}

// send off a preview frame if sufficent preview frames accumulated, or flushing
// is is assumed a repeated submessage preview was just added
static void send_preview(const char *client, bool flush = false)
{
    int retval;
    n_messages++;

    if ((output.preview_size() > batch_limit) || flush) {
	n_containers++;
	n_bytes += output.ByteSize();
	output.set_type(machinetalk::MT_PREVIEW);
	retval = send_pbcontainer(client, output, z_preview);
	assert(retval == 0);
    }
}

// send preview start message
static void preview_start()
{
    machinetalk::Preview *p = output.add_preview();
    p->set_type(machinetalk::PV_PREVIEW_START);
    send_preview(p_client);
}

// send preview end message
static void preview_end()
{
    machinetalk::Preview *p = output.add_preview();
    p->set_type(machinetalk::PV_PREVIEW_END);
    send_preview(p_client);
}

static int z_init(void)
{
    // const char *uri = getenv("PREVIEW_URI");
    // if (uri) z_preview_uri = uri;
    // uri = getenv("STATUS_URI");
    // if (uri) z_status_uri = uri;

    if (getenv("BATCH"))
	batch_limit = atoi(getenv("BATCH"));

    // Verify that the version of the library that we linked against is
    // compatible with the version of the headers we compiled against.
    GOOGLE_PROTOBUF_VERIFY_VERSION;


    z_preview = zsock_new (ZMQ_XPUB);
#if 0
    rc = zsock_bind(z_preview, z_preview_uri);
    assert (rc != 0);
#endif

    z_status = zsock_new (ZMQ_XPUB);
    assert(z_status);
#if 0 
    rc = zsock_bind(z_status, z_status_uri);
    assert (rc != 0);

#endif

    note_printf(istat, "interpreter startup pid=%d", getpid());
    publish_istat(machinetalk::INTERP_IDLE);

    return 0;
}

// called on module unload
static void z_shutdown(void)
{
    fprintf(stderr, "preview: socket shutdown\n");
    if (n_containers > 0)
    {
        fprintf(stderr, "preview: %zu containers %zu preview msgs %zu bytes  avg=%zu bytes/container\n",
            n_containers, n_messages, n_bytes, n_bytes/n_containers);
    }
    zsock_destroy(&z_preview);
    zsock_destroy(&z_status);
}

// this is junk that you have to define in exactly this way because of how mah
// implemented the python "remap" functionality of the interpreter
// (and it needs Python.h for the definition of struct inttab)
char _parameter_file_name[LINELEN];
#if PY_MAJOR_VERSION >=3

extern "C" PyObject* PyInit_emctask(void);
extern "C" PyObject* PyInit_interpreter(void);
extern "C" PyObject* PyInit_emccanon(void);
extern "C" struct _inittab builtin_modules[];
struct _inittab builtin_modules[] = {
    { "interpreter", PyInit_interpreter },
    { "emccanon", PyInit_emccanon },
    { NULL, NULL }
};

#else

extern "C" void initinterpreter();
extern "C" void initemccanon();
extern "C" struct _inittab builtin_modules[];
struct _inittab builtin_modules[] = {
    { (char *) "interpreter", initinterpreter },
    { (char *) "emccanon", initemccanon },
    { NULL, NULL }
};
#endif

static PyObject *int_array(int *arr, int sz) {
    PyObject *res = PyTuple_New(sz);
    for(int i = 0; i < sz; i++) {
        PyTuple_SET_ITEM(res, i, PyLong_FromLong(arr[i]));
    }
    return res;
}

typedef struct {
    PyObject_HEAD
    double settings[ACTIVE_SETTINGS];
    int gcodes[ACTIVE_G_CODES];
    int mcodes[ACTIVE_M_CODES];
} LineCode;

static PyObject *LineCode_gcodes(LineCode *l) {
    return int_array(l->gcodes, ACTIVE_G_CODES);
}
static PyObject *LineCode_mcodes(LineCode *l) {
    return int_array(l->mcodes, ACTIVE_M_CODES);
}

static PyGetSetDef LineCodeGetSet[] = {
    {(char*)"gcodes", (getter)LineCode_gcodes},
    {(char*)"mcodes", (getter)LineCode_mcodes},
    {NULL, NULL},
};

static PyMemberDef LineCodeMembers[] = {
    {(char*)"sequence_number", T_INT, offsetof(LineCode, gcodes[0]), READONLY},

    {(char*)"feed_rate", T_DOUBLE, offsetof(LineCode, settings[1]), READONLY},
    {(char*)"speed", T_DOUBLE, offsetof(LineCode, settings[2]), READONLY},
    {(char*)"motion_mode", T_INT, offsetof(LineCode, gcodes[1]), READONLY},
    {(char*)"block", T_INT, offsetof(LineCode, gcodes[2]), READONLY},
    {(char*)"plane", T_INT, offsetof(LineCode, gcodes[3]), READONLY},
    {(char*)"cutter_side", T_INT, offsetof(LineCode, gcodes[4]), READONLY},
    {(char*)"units", T_INT, offsetof(LineCode, gcodes[5]), READONLY},
    {(char*)"distance_mode", T_INT, offsetof(LineCode, gcodes[6]), READONLY},
    {(char*)"feed_mode", T_INT, offsetof(LineCode, gcodes[7]), READONLY},
    {(char*)"origin", T_INT, offsetof(LineCode, gcodes[8]), READONLY},
    {(char*)"tool_length_offset", T_INT, offsetof(LineCode, gcodes[9]), READONLY},
    {(char*)"retract_mode", T_INT, offsetof(LineCode, gcodes[10]), READONLY},
    {(char*)"path_mode", T_INT, offsetof(LineCode, gcodes[11]), READONLY},

    {(char*)"stopping", T_INT, offsetof(LineCode, mcodes[1]), READONLY},
    {(char*)"spindle", T_INT, offsetof(LineCode, mcodes[2]), READONLY},
    {(char*)"toolchange", T_INT, offsetof(LineCode, mcodes[3]), READONLY},
    {(char*)"mist", T_INT, offsetof(LineCode, mcodes[4]), READONLY},
    {(char*)"flood", T_INT, offsetof(LineCode, mcodes[5]), READONLY},
    {(char*)"overrides", T_INT, offsetof(LineCode, mcodes[6]), READONLY},
    {NULL}
};

static PyTypeObject LineCodeType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    "gcode.linecode",       /*tp_name*/
    sizeof(LineCode),       /*tp_basicsize*/
    0,                      /*tp_itemsize*/
    /* methods */
    0,                      /*tp_dealloc*/
    0,                      /*tp_print*/
    0,                      /*tp_getattr*/
    0,                      /*tp_setattr*/
    0,                      /*tp_compare*/
    0,                      /*tp_repr*/
    0,                      /*tp_as_number*/
    0,                      /*tp_as_sequence*/
    0,                      /*tp_as_mapping*/
    0,                      /*tp_hash*/
    0,                      /*tp_call*/
    0,                      /*tp_str*/
    0,                      /*tp_getattro*/
    0,                      /*tp_setattro*/
    0,                      /*tp_as_buffer*/
    Py_TPFLAGS_DEFAULT,     /*tp_flags*/
    0,                      /*tp_doc*/
    0,                      /*tp_traverse*/
    0,                      /*tp_clear*/
    0,                      /*tp_richcompare*/
    0,                      /*tp_weaklistoffset*/
    0,                      /*tp_iter*/
    0,                      /*tp_iternext*/
    0,                      /*tp_methods*/
    LineCodeMembers,     /*tp_members*/
    LineCodeGetSet,      /*tp_getset*/
    0,                      /*tp_base*/
    0,                      /*tp_dict*/
    0,                      /*tp_descr_get*/
    0,                      /*tp_descr_set*/
    0,                      /*tp_dictoffset*/
    0,                      /*tp_init*/
    0,                      /*tp_alloc*/
    PyType_GenericNew,      /*tp_new*/
    0,                      /*tp_free*/
    0,                      /*tp_is_gc*/
};

static PyObject *callback;
static int interp_error;
static int last_sequence_number;
static bool metric;
static double _pos_x, _pos_y, _pos_z, _pos_a, _pos_b, _pos_c, _pos_u, _pos_v, _pos_w;
CANON_PLANE _pl;
EmcPose tool_offset;

static InterpBase *pinterp;
#define interp_new (*pinterp)

#define callmethod(o, m, f, ...) PyObject_CallMethod((o), (char*)(m), (char*)(f), ## __VA_ARGS__)

static void maybe_new_line(int sequence_number=interp_new.sequence_number());
static void maybe_new_line(int sequence_number) {
    if(!pinterp) return;
    if(interp_error) return;
    if(sequence_number == last_sequence_number)
        return;

    return ;; // not used - leaks memory
    LineCode *new_line_code =
        (LineCode*)(PyObject_New(LineCode, &LineCodeType));
    interp_new.active_settings(new_line_code->settings);
    interp_new.active_g_codes(new_line_code->gcodes);
    interp_new.active_m_codes(new_line_code->mcodes);
    new_line_code->gcodes[0] = sequence_number;
    last_sequence_number = sequence_number;
    // PyObject *result =
    //     callmethod(callback, "next_line", "O", new_line_code);
    // Py_DECREF(new_line_code);
    // if(result == NULL) interp_error ++;
    // Py_XDECREF(result);
}

void NURBS_FEED(int line_number, std::vector<CONTROL_POINT> nurbs_control_points, unsigned int k) {
    double u = 0.0;
    unsigned int n = nurbs_control_points.size() - 1;
    double umax = n - k + 2;
    unsigned int div = nurbs_control_points.size()*15;
    std::vector<unsigned int> knot_vector = knot_vector_creator(n, k);
    PLANE_POINT P1;
    while (u+umax/div < umax) {
        PLANE_POINT P1 = nurbs_point(u+umax/div,k,nurbs_control_points,knot_vector);
        STRAIGHT_FEED(line_number, P1.X,P1.Y, _pos_z, _pos_a, _pos_b, _pos_c, _pos_u, _pos_v, _pos_w);
        u = u + umax/div;
    }
    P1.X = nurbs_control_points[n].X;
    P1.Y = nurbs_control_points[n].Y;
    STRAIGHT_FEED(line_number, P1.X,P1.Y, _pos_z, _pos_a, _pos_b, _pos_c, _pos_u, _pos_v, _pos_w);
    knot_vector.clear();
}

void ARC_FEED(int line_number,
              double first_end, double second_end, double first_axis,
              double second_axis, int rotation, double axis_end_point,
              double a_position, double b_position, double c_position,
              double u_position, double v_position, double w_position) {
    double x, y, z;
    if (_pl == CANON_PLANE_XY) {
        x = first_end;
        y = second_end;
        z = axis_end_point;
    }
    else if (_pl == CANON_PLANE_XZ) {
        x = second_end;
        y = axis_end_point;
        z = first_end;
    }
    else if (_pl == CANON_PLANE_YZ) {
        x = axis_end_point;
        y = first_end;
        z = second_end;
    }
    else {
        x = _pos_x;
        y = _pos_y;
        z = _pos_z;
    }
    _pos_x = x; _pos_y = y; _pos_z = z;
    _pos_a=a_position; _pos_b=b_position; _pos_c=c_position;
    _pos_u=u_position; _pos_v=v_position; _pos_w=w_position;
    maybe_new_line(line_number);
    if(interp_error) return;
    // PyObject *result =
    //     callmethod(callback, "arc_feed", "ffffifffffff",
    //                         first_end, second_end, first_axis, second_axis,
    //                         rotation, axis_end_point,
    //                         a_position, b_position, c_position,
    //                         u_position, v_position, w_position);
    // if(result == NULL) interp_error ++;
    // Py_XDECREF(result);

    machinetalk::Preview *p = output.add_preview();
    p->set_type(machinetalk::PV_ARC_FEED);
    p->set_line_number(line_number);
    p->set_first_end(first_end);
    p->set_second_end(second_end);
    p->set_first_axis(first_axis);
    p->set_second_axis(second_axis);
    p->set_rotation(rotation);
    p->set_axis_end_point(axis_end_point);

    machinetalk::Position *pos = p->mutable_pos();
    pos->set_x(x);
    pos->set_y(y);
    pos->set_z(z);
    pos->set_a(a_position);
    pos->set_b(b_position);
    pos->set_c(c_position);
    pos->set_u(u_position);
    pos->set_v(v_position);
    pos->set_w(w_position);
    send_preview(p_client);

}

void STRAIGHT_FEED(int line_number,
                   double x, double y, double z,
                   double a, double b, double c,
                   double u, double v, double w) {
    _pos_x=x; _pos_y=y; _pos_z=z;
    _pos_a=a; _pos_b=b; _pos_c=c;
    _pos_u=u; _pos_v=v; _pos_w=w;
    maybe_new_line(line_number);
    if(interp_error) return;
    // PyObject *result =
    //     callmethod(callback, "straight_feed", "fffffffff",
    //                         x, y, z, a, b, c, u, v, w);
    // if(result == NULL) interp_error ++;
    // Py_XDECREF(result);

    machinetalk::Preview *p = output.add_preview();
    p->set_type(machinetalk::PV_STRAIGHT_FEED);
    p->set_line_number(line_number);

    machinetalk::Position *pos = p->mutable_pos();
    pos->set_x(x);
    pos->set_y(y);
    pos->set_z(z);
    pos->set_a(a);
    pos->set_b(b);
    pos->set_c(c);
    pos->set_u(u);
    pos->set_v(v);
    pos->set_w(w);
    send_preview(p_client);

}

void STRAIGHT_TRAVERSE(int line_number,
                       double x, double y, double z,
                       double a, double b, double c,
                       double u, double v, double w) {
    _pos_x=x; _pos_y=y; _pos_z=z;
    _pos_a=a; _pos_b=b; _pos_c=c;
    _pos_u=u; _pos_v=v; _pos_w=w;
    maybe_new_line(line_number);
    if(interp_error) return;
    // PyObject *result =
    //     callmethod(callback, "straight_traverse", "fffffffff",
    //                         x, y, z, a, b, c, u, v, w);
    // if(result == NULL) interp_error ++;
    // Py_XDECREF(result);

    machinetalk::Preview *p = output.add_preview();
    p->set_type(machinetalk::PV_STRAIGHT_TRAVERSE);
    p->set_line_number(line_number);

    machinetalk::Position *pos = p->mutable_pos();
    pos->set_x(x);
    pos->set_y(y);
    pos->set_z(z);
    pos->set_a(a);
    pos->set_b(b);
    pos->set_c(c);
    pos->set_u(u);
    pos->set_v(v);
    pos->set_w(w);
    send_preview(p_client);

}

void SET_G5X_OFFSET(int g5x_index,
                    double x, double y, double z,
                    double a, double b, double c,
                    double u, double v, double w) {
    maybe_new_line();
    if(interp_error) return;
    // PyObject *result =
    //     callmethod(callback, "set_g5x_offset", "ifffffffff",
    //                         g5x_index, x, y, z, a, b, c, u, v, w);
    // if(result == NULL) interp_error ++;
    // Py_XDECREF(result);

    machinetalk::Preview *p = output.add_preview();
    p->set_type(machinetalk::PV_SET_G5X_OFFSET);
    //    p->set_line_number(line_number);
    p->set_g5_index(g5x_index);

    machinetalk::Position *pos = p->mutable_pos();
    pos->set_x(x);
    pos->set_y(y);
    pos->set_z(z);
    pos->set_a(a);
    pos->set_b(b);
    pos->set_c(c);
    pos->set_u(u);
    pos->set_v(v);
    pos->set_w(w);
    send_preview(p_client);


}

void SET_G92_OFFSET(double x, double y, double z,
                    double a, double b, double c,
                    double u, double v, double w) {
    maybe_new_line();
    if(interp_error) return;
    // PyObject *result =
    //     callmethod(callback, "set_g92_offset", "fffffffff",
    //                         x, y, z, a, b, c, u, v, w);
    // if(result == NULL) interp_error ++;
    // Py_XDECREF(result);

    machinetalk::Preview *p = output.add_preview();
    p->set_type(machinetalk::PV_SET_G92_OFFSET);
    //    p->set_line_number(line_number);

    machinetalk::Position *pos = p->mutable_pos();
    pos->set_x(x);
    pos->set_y(y);
    pos->set_z(z);
    pos->set_a(a);
    pos->set_b(b);
    pos->set_c(c);
    pos->set_u(u);
    pos->set_v(v);
    pos->set_w(w);
    send_preview(p_client);

}

void SET_XY_ROTATION(double t) {
    maybe_new_line();
    if(interp_error) return;
    // PyObject *result =
    //     callmethod(callback, "set_xy_rotation", "f", t);
    // if(result == NULL) interp_error ++;

    machinetalk::Preview *p = output.add_preview();
    p->set_type(machinetalk::PV_SET_G92_OFFSET);
    //    p->set_line_number(line_number);
    p->set_xy_rotation(t);
    send_preview(p_client);

};

void USE_LENGTH_UNITS(CANON_UNITS u) { }

void SELECT_PLANE(CANON_PLANE pl) {
    _pl = pl;
    maybe_new_line();
    // if(interp_error) return;

    machinetalk::Preview *p = output.add_preview();
    p->set_type(machinetalk::PV_SELECT_PLANE);
    p->set_plane(pl);
    send_preview(p_client);

    // PyObject *result =
    //     callmethod(callback, "set_plane", "i", pl);
    // if(result == NULL) interp_error ++;
    // Py_XDECREF(result);
}

void SET_TRAVERSE_RATE(double rate) {
    maybe_new_line();
    if(interp_error) return;
    // PyObject *result =
    //     callmethod(callback, "set_traverse_rate", "f", rate);
    // if(result == NULL) interp_error ++;
    // Py_XDECREF(result);

    machinetalk::Preview *p = output.add_preview();
    p->set_type(machinetalk::PV_SET_TRAVERSE_RATE);
    //    p->set_line_number(line_number);
    p->set_rate(rate);
    send_preview(p_client);

}

void SET_FEED_MODE(int spindle, int mode) {
#if 0
    maybe_new_line();
    if(interp_error) return;
    PyObject *result =
        callmethod(callback, "set_feed_mode", "i", mode);
    if(result == NULL) interp_error ++;
    Py_XDECREF(result);
#endif
}

void CHANGE_TOOL(int pocket) {
    maybe_new_line();
    if(interp_error) return;
    PyObject *result =
        callmethod(callback, "change_tool", "i", pocket);
    if(result == NULL) interp_error ++;
    Py_XDECREF(result);

    machinetalk::Preview *p = output.add_preview();
    p->set_type(machinetalk::PV_CHANGE_TOOL);
    //    p->set_line_number(line_number);
    p->set_pocket(pocket);
    send_preview(p_client);
}

void CHANGE_TOOL_NUMBER(int pocket) {
    maybe_new_line();
    if(interp_error) return;
}

/* XXX: This needs to be re-thought.  Sometimes feed rate is not in linear
 * units--e.g., it could be inverse time feed mode.  in that case, it's wrong
 * to convert from mm to inch here.  but the gcode time estimate gets inverse
 * time feed wrong anyway..
 */
void SET_FEED_RATE(double rate) {
    maybe_new_line();
    if(interp_error) return;
    // PyObject *result =
    //     callmethod(callback, "set_feed_rate", "f", rate);
    // if(result == NULL) interp_error ++;
    // Py_XDECREF(result);

    machinetalk::Preview *p = output.add_preview();
    p->set_type(machinetalk::PV_SET_FEED_RATE);
    //    p->set_line_number(line_number);
    p->set_rate(rate);
    send_preview(p_client);

}

void DWELL(double time) {
    maybe_new_line();
    // if(interp_error) return;
    // PyObject *result =
    //     callmethod(callback, "dwell", "f", time);
    // if(result == NULL) interp_error ++;
    // Py_XDECREF(result);

    machinetalk::Preview *p = output.add_preview();
    p->set_type(machinetalk::PV_DWELL);
    //    p->set_line_number(line_number);
    p->set_time(time);
    send_preview(p_client);

}

void MESSAGE(char *comment) {
    maybe_new_line();
    if(interp_error) return;
    // PyObject *result =
    //     callmethod(callback, "message", "s", comment);
    // if(result == NULL) interp_error ++;
    // Py_XDECREF(result);

    machinetalk::Preview *p = output.add_preview();
    p->set_type(machinetalk::PV_MESSAGE);
    //    p->set_line_number(line_number);
    p->set_text(comment);
    send_preview(p_client);

}

void LOG(char *s) {}
void LOGOPEN(char *f) {}
void LOGAPPEND(char *f) {}
void LOGCLOSE() {}

void COMMENT(const char *comment) {
    maybe_new_line();
    if(interp_error) return;
    // PyObject *result =
    //     callmethod(callback, "comment", "s", comment);
    // if(result == NULL) interp_error ++;
    // Py_XDECREF(result);

    machinetalk::Preview *p = output.add_preview();
    p->set_type(machinetalk::PV_COMMENT);
    //    p->set_line_number(line_number);
    p->set_text(comment);
    send_preview(p_client);

}

void SET_TOOL_TABLE_ENTRY(int pocket, int toolno, EmcPose offset, double diameter,
                          double frontangle, double backangle, int orientation) {
}

void USE_TOOL_LENGTH_OFFSET(EmcPose offset) {
    tool_offset = offset;
    maybe_new_line();
    if(interp_error) return;

    // PyObject *result = callmethod(callback, "tool_offset", "ddddddddd", offset.tran.x, offset.tran.y, offset.tran.z,
    //     offset.a, offset.b, offset.c, offset.u, offset.v, offset.w);
    // if(result == NULL) interp_error ++;
    // Py_XDECREF(result);

    machinetalk::Preview *p = output.add_preview();
    p->set_type(machinetalk::PV_USE_TOOL_OFFSET);
    //    p->set_line_number(line_number);

    machinetalk::Position *pos = p->mutable_pos();
    pos->set_x(offset.tran.x);
    pos->set_y(offset.tran.y);
    pos->set_z(offset.tran.z);
    pos->set_a(offset.a);
    pos->set_b(offset.b);
    pos->set_c(offset.c);
    pos->set_u(offset.u);
    pos->set_v(offset.v);
    pos->set_w(offset.w);
    send_preview(p_client);
}

void SET_FEED_REFERENCE(double reference) { }
void SET_CUTTER_RADIUS_COMPENSATION(double radius) {}
void START_CUTTER_RADIUS_COMPENSATION(int direction) {}
void STOP_CUTTER_RADIUS_COMPENSATION(int direction) {}
void START_SPEED_FEED_SYNCH() {}
void START_SPEED_FEED_SYNCH(int spindle, double sync, bool vel) {}
void STOP_SPEED_FEED_SYNCH() {}
void START_SPINDLE_COUNTERCLOCKWISE(int spindle, int wait_for_at_speed) {}
void START_SPINDLE_CLOCKWISE(int spindle, int wait_for_at_speed) {}
void SET_SPINDLE_MODE(int spindle, double mode) {}
void STOP_SPINDLE_TURNING(int spindle) {}
void SET_SPINDLE_SPEED(int spindle, double rpm) {}
void ORIENT_SPINDLE(int spindle, double orientation, int mode) {}
void WAIT_SPINDLE_ORIENT_COMPLETE(int spindle, double timeout) {}
void PROGRAM_STOP() {}
void PROGRAM_END() {}
void FINISH() {}
void ON_RESET() {}
void PALLET_SHUTTLE() {}
void SELECT_TOOL(int tool) {}
void SELECT_POCKET(int pocket, int tool) {}
void UPDATE_TAG(StateTag tag) {}
void OPTIONAL_PROGRAM_STOP() {}
void START_CHANGE() {}
int  GET_EXTERNAL_TC_FAULT() {return 0;}
int  GET_EXTERNAL_TC_REASON() {return 0;}


extern bool GET_BLOCK_DELETE(void) {
    int bd = 0;
    if(interp_error) return 0;
    PyObject *result =
        callmethod(callback, "get_block_delete", "");
    if(result == NULL) {
        interp_error++;
    } else {
        bd = PyObject_IsTrue(result);
    }
    Py_XDECREF(result);
    return bd;
}

void CANON_ERROR(const char *fmt, ...) {};
void CLAMP_AXIS(CANON_AXIS axis) {}
bool GET_OPTIONAL_PROGRAM_STOP() { return false;}
void SET_OPTIONAL_PROGRAM_STOP(bool state) {}
void SPINDLE_RETRACT_TRAVERSE() {}
void SPINDLE_RETRACT() {}
void STOP_CUTTER_RADIUS_COMPENSATION() {}
void USE_NO_SPINDLE_FORCE() {}
void SET_BLOCK_DELETE(bool enabled) {}

void DISABLE_FEED_OVERRIDE() {}
void DISABLE_FEED_HOLD() {}
void ENABLE_FEED_HOLD() {}
void DISABLE_SPEED_OVERRIDE(int spindle) {}
void ENABLE_FEED_OVERRIDE() {}
void ENABLE_SPEED_OVERRIDE(int spindle) {}
void MIST_OFF() {}
void FLOOD_OFF() {}
void MIST_ON() {}
void FLOOD_ON() {}
void CLEAR_AUX_OUTPUT_BIT(int bit) {}
void SET_AUX_OUTPUT_BIT(int bit) {}
void SET_AUX_OUTPUT_VALUE(int index, double value) {}
void CLEAR_MOTION_OUTPUT_BIT(int bit) {}
void SET_MOTION_OUTPUT_BIT(int bit) {}
void SET_MOTION_OUTPUT_VALUE(int index, double value) {}
void TURN_PROBE_ON() {}
void TURN_PROBE_OFF() {}
int UNLOCK_ROTARY(int line_no, int axis) {return 0;}
int LOCK_ROTARY(int line_no, int axis) {return 0;}
void INTERP_ABORT(int reason,const char *message) {}
void PLUGIN_CALL(int len, const char *call) {}
void IO_PLUGIN_CALL(int len, const char *call) {}

void STRAIGHT_PROBE(int line_number,
                    double x, double y, double z,
                    double a, double b, double c,
                    double u, double v, double w, unsigned char probe_type) {
    _pos_x=x; _pos_y=y; _pos_z=z;
    _pos_a=a; _pos_b=b; _pos_c=c;
    _pos_u=u; _pos_v=v; _pos_w=w;
    maybe_new_line(line_number);
    if(interp_error) return;
    // PyObject *result =
    //     callmethod(callback, "straight_probe", "fffffffff",
    //                         x, y, z, a, b, c, u, v, w);
    // if(result == NULL) interp_error ++;
    // Py_XDECREF(result);

    machinetalk::Preview *p = output.add_preview();
    p->set_type(machinetalk::PV_STRAIGHT_PROBE);
    p->set_line_number(line_number);

    machinetalk::Position *pos = p->mutable_pos();
    pos->set_x(x);
    pos->set_y(y);
    pos->set_z(z);
    pos->set_a(a);
    pos->set_b(b);
    pos->set_c(c);
    pos->set_u(u);
    pos->set_v(v);
    pos->set_w(w);
    send_preview(p_client);
}

void RIGID_TAP(int line_number,
               double x, double y, double z, double scale) {
    if(metric) { x /= 25.4; y /= 25.4; z /= 25.4; }
    maybe_new_line(line_number);
    if(interp_error) return;
    PyObject *result =
        callmethod(callback, "rigid_tap", "fff",
            x, y, z);
    if(result == NULL) interp_error ++;
    Py_XDECREF(result);
}
double GET_EXTERNAL_MOTION_CONTROL_TOLERANCE() { return 0.1; }
double GET_EXTERNAL_MOTION_CONTROL_NAIVECAM_TOLERANCE() { return 0.1; }
double GET_EXTERNAL_PROBE_POSITION_X() { return _pos_x; }
double GET_EXTERNAL_PROBE_POSITION_Y() { return _pos_y; }
double GET_EXTERNAL_PROBE_POSITION_Z() { return _pos_z; }
double GET_EXTERNAL_PROBE_POSITION_A() { return _pos_a; }
double GET_EXTERNAL_PROBE_POSITION_B() { return _pos_b; }
double GET_EXTERNAL_PROBE_POSITION_C() { return _pos_c; }
double GET_EXTERNAL_PROBE_POSITION_U() { return _pos_u; }
double GET_EXTERNAL_PROBE_POSITION_V() { return _pos_v; }
double GET_EXTERNAL_PROBE_POSITION_W() { return _pos_w; }
double GET_EXTERNAL_PROBE_VALUE() { return 0.0; }
int GET_EXTERNAL_PROBE_TRIPPED_VALUE() { return 0; }
double GET_EXTERNAL_POSITION_X() { return _pos_x; }
double GET_EXTERNAL_POSITION_Y() { return _pos_y; }
double GET_EXTERNAL_POSITION_Z() { return _pos_z; }
double GET_EXTERNAL_POSITION_A() { return _pos_a; }
double GET_EXTERNAL_POSITION_B() { return _pos_b; }
double GET_EXTERNAL_POSITION_C() { return _pos_c; }
double GET_EXTERNAL_POSITION_U() { return _pos_u; }
double GET_EXTERNAL_POSITION_V() { return _pos_v; }
double GET_EXTERNAL_POSITION_W() { return _pos_w; }
void INIT_CANON() {}

void SET_PARAMETER_FILE_NAME(const char *name)
{
  strncpy(_parameter_file_name, name, PARAMETER_FILE_NAME_LENGTH);
}

void GET_EXTERNAL_PARAMETER_FILE_NAME(char *name, int max_size) {
    PyObject *result = PyObject_GetAttrString(callback, "parameter_file");
    if(!result) { name[0] = 0; return; }
    const char *s = PyUnicode_AsUTF8(result);
    if(!s) { name[0] = 0; return; }
    memset(name, 0, max_size);
    strncpy(name, s, max_size - 1);
}
CANON_UNITS GET_EXTERNAL_LENGTH_UNIT_TYPE() { return CANON_UNITS_INCHES; }
CANON_TOOL_TABLE GET_EXTERNAL_TOOL_TABLE(int pocket) {
    CANON_TOOL_TABLE t = {-1,0,{{0,0,0},0,0,0,0,0,0},0,0,0,0};
    if(interp_error) return t;

    PyObject *result = callmethod(callback, "get_tool", "i", pocket);
    PyErr_Clear(); // necessary because ParseTuple fails
    int ok = PyArg_ParseTuple(result, "iddddddddddddi", &t.toolno,
                              &t.offset.tran.x, &t.offset.tran.y, &t.offset.tran.z,
                              &t.offset.a, &t.offset.b, &t.offset.c,
                              &t.offset.u, &t.offset.v, &t.offset.w,
                              &t.diameter, &t.frontangle, &t.backangle,
                              &t.orientation);

    if (result == NULL || !ok)
    {
        interp_error ++;
    }

    Py_XDECREF(result);
    return t;
}

int GET_EXTERNAL_DIGITAL_INPUT(int index, int def) { return def; }
double GET_EXTERNAL_ANALOG_INPUT(int index, double def) { return def; }
int WAIT(int index, int input_type, int wait_type, double timeout) { return 0;}

static void user_defined_function(int num, double arg1, double arg2) {
    if(interp_error) return;
    maybe_new_line();
    PyObject *result =
        callmethod(callback, "user_defined_function",
                            "idd", num, arg1, arg2);
    if(result == NULL) interp_error++;
    Py_XDECREF(result);
}

void SET_FEED_REFERENCE(CANON_FEED_REFERENCE ref) {}
int GET_EXTERNAL_QUEUE_EMPTY() { return true; }
CANON_DIRECTION GET_EXTERNAL_SPINDLE(int spindle) { return CANON_STOPPED; }
int GET_EXTERNAL_TOOL_SLOT() { return 0; }
int GET_EXTERNAL_SELECTED_TOOL_SLOT() { return 0; }
double GET_EXTERNAL_FEED_RATE() { return 1; }
double GET_EXTERNAL_TRAVERSE_RATE() { return 0; }
int GET_EXTERNAL_FLOOD() { return 0; }
int GET_EXTERNAL_MIST() { return 0; }
CANON_PLANE GET_EXTERNAL_PLANE() { return CANON_PLANE_XY; }
double GET_EXTERNAL_SPEED(int spindle) { return 0; }
int GET_EXTERNAL_POCKETS_MAX() { return CANON_POCKETS_MAX; }
void DISABLE_ADAPTIVE_FEED() {}
void ENABLE_ADAPTIVE_FEED() {}

int GET_EXTERNAL_FEED_OVERRIDE_ENABLE() {return 1;}
int GET_EXTERNAL_SPINDLE_OVERRIDE_ENABLE(int spindle) {return 1;}
int GET_EXTERNAL_ADAPTIVE_FEED_ENABLE() {return 0;}
int GET_EXTERNAL_FEED_HOLD_ENABLE() {return 1;}

int GET_EXTERNAL_OFFSET_APPLIED() {return 0;}
EmcPose GET_EXTERNAL_OFFSETS() {
    EmcPose e;
    e.tran.x = 0;
    e.tran.y = 0;
    e.tran.z = 0;
    e.a      = 0;
    e.b      = 0;
    e.c      = 0;
    e.u      = 0;
    e.v      = 0;
    e.w      = 0;
    return e;
};

int GET_EXTERNAL_AXIS_MASK() {
    if(interp_error) return 7;
    PyObject *result =
        callmethod(callback, "get_axis_mask", "");
    if(!result) { interp_error ++; return 7 /* XYZABC */; }
    if(!PyLong_Check(result)) { interp_error ++; return 7 /* XYZABC */; }
    int mask = PyLong_AS_LONG(result);
    Py_DECREF(result);
    return mask; // XYZABC mask;
}

double GET_EXTERNAL_TOOL_LENGTH_XOFFSET() {
    return tool_offset.tran.x;
}
double GET_EXTERNAL_TOOL_LENGTH_YOFFSET() {
    return tool_offset.tran.y;
}
double GET_EXTERNAL_TOOL_LENGTH_ZOFFSET() {
    return tool_offset.tran.z;
}
double GET_EXTERNAL_TOOL_LENGTH_AOFFSET() {
    return tool_offset.a;
}
double GET_EXTERNAL_TOOL_LENGTH_BOFFSET() {
    return tool_offset.b;
}
double GET_EXTERNAL_TOOL_LENGTH_COFFSET() {
    return tool_offset.c;
}
double GET_EXTERNAL_TOOL_LENGTH_UOFFSET() {
    return tool_offset.u;
}
double GET_EXTERNAL_TOOL_LENGTH_VOFFSET() {
    return tool_offset.v;
}
double GET_EXTERNAL_TOOL_LENGTH_WOFFSET() {
    return tool_offset.w;
}

static bool PyLong_CheckAndError(const char *func, PyObject *p)  {
    if(PyLong_Check(p)) return true;
    PyErr_Format(PyExc_TypeError,
            "%s: Expected int, got %s", func, p->ob_type->tp_name);
    return false;
}

static bool PyFloat_CheckAndError(const char *func, PyObject *p)  {
    if(PyFloat_Check(p)) return true;
    PyErr_Format(PyExc_TypeError,
            "%s: Expected float, got %s", func, p->ob_type->tp_name);
    return false;
}

double GET_EXTERNAL_ANGLE_UNITS() {
    PyObject *result =
        callmethod(callback, "get_external_angular_units", "");
    if(result == NULL) interp_error++;

    double dresult = 1.0;
    if(!result || !PyFloat_CheckAndError("get_external_angle_units", result)) {
        interp_error++;
    } else {
        dresult = PyFloat_AsDouble(result);
    }
    Py_XDECREF(result);
    return dresult;
}

double GET_EXTERNAL_LENGTH_UNITS() {
    PyObject *result =
        callmethod(callback, "get_external_length_units", "");
    if(result == NULL) interp_error++;

    double dresult = 0.03937007874016;
    if(!result || !PyFloat_CheckAndError("get_external_length_units", result)) {
        interp_error++;
    } else {
        dresult = PyFloat_AsDouble(result);
    }
    Py_XDECREF(result);
    return dresult;
}

static bool check_abort() {
    PyObject *result =
        callmethod(callback, "check_abort", "");
    if(!result) return 1;
    if(PyObject_IsTrue(result)) {
        Py_DECREF(result);
        PyErr_Format(PyExc_KeyboardInterrupt, "Load aborted");
        return 1;
    }
    Py_DECREF(result);
    return 0;
}

USER_DEFINED_FUNCTION_TYPE USER_DEFINED_FUNCTION[USER_DEFINED_FUNCTION_NUM];

CANON_MOTION_MODE motion_mode;
void SET_MOTION_CONTROL_MODE(CANON_MOTION_MODE mode, double tolerance) { motion_mode = mode; }
void SET_MOTION_CONTROL_MODE(double tolerance) { }
void SET_MOTION_CONTROL_MODE(CANON_MOTION_MODE mode) { motion_mode = mode; }
CANON_MOTION_MODE GET_EXTERNAL_MOTION_CONTROL_MODE() { return motion_mode; }
void SET_NAIVECAM_TOLERANCE(double tolerance) { }

#define RESULT_OK (result == INTERP_OK || result == INTERP_EXECUTE_FINISH)
static PyObject *parse_file(PyObject *self, PyObject *args) {
    char *f;
    char *unitcode=0, *initcode=0, *interpname=0;
    int error_line_offset = 0;
    struct timeval t0, t1;
    int wait = 1;
    if(!PyArg_ParseTuple(args, "sO|sss", &f, &callback, &unitcode, &initcode, &interpname))
        return NULL;

    if(pinterp) {
        delete pinterp;
        pinterp = 0;
    }
    if(interpname && *interpname)
        pinterp = interp_from_shlib(interpname);
    if(!pinterp)
        pinterp = new Interp;

    for(int i=0; i<USER_DEFINED_FUNCTION_NUM; i++)
        USER_DEFINED_FUNCTION[i] = user_defined_function;

    gettimeofday(&t0, NULL);

    interp_error = 0;
    last_sequence_number = -1;

    _pos_x = _pos_y = _pos_z = _pos_a = _pos_b = _pos_c = 0;
    _pos_u = _pos_v = _pos_w = 0;
    _pl = CANON_PLANE_XY;

    note_printf(istat, "open '%s'", f);
    publish_istat(machinetalk::INTERP_RUNNING);
    preview_start();
    interp_new.init();
    interp_new.open(f);
    maybe_new_line();

    machinetalk::Preview *p = output.add_preview();
    p->set_type(machinetalk::PV_SOURCE_CONTEXT);
    p->set_stype(machinetalk::ST_NGC_FILE);
    p->set_filename(f);
    p->set_line_number(interp_new.sequence_number());

    int result = INTERP_OK;
    if(unitcode) {
        result = interp_new.read(unitcode);
        if(!RESULT_OK) goto out_error;
        result = interp_new.execute();
    }
    if(initcode && RESULT_OK) {
        result = interp_new.read(initcode);
        if(!RESULT_OK) goto out_error;
        result = interp_new.execute();
    }
    while(!interp_error && RESULT_OK) {
        error_line_offset = 1;
        result = interp_new.read();
        gettimeofday(&t1, NULL);
        if(t1.tv_sec > t0.tv_sec + wait) {
            if(check_abort()) return NULL;
            t0 = t1;
        }
        if(!RESULT_OK) break;
        error_line_offset = 0;
        result = interp_new.execute();
    }

out_error:
    preview_end();
    send_preview(p_client, true);
    publish_istat(machinetalk::INTERP_IDLE);

    if(pinterp) pinterp->close();
    if(interp_error) {
        if(!PyErr_Occurred()) {
            PyErr_Format(PyExc_RuntimeError,
                    "interp_error > 0 but no Python exception set");
        }
        return NULL;
    }
    PyErr_Clear();
    maybe_new_line();
    if(PyErr_Occurred()) { interp_error = 1; goto out_error; }
    PyObject *retval = PyTuple_New(2);
    PyTuple_SetItem(retval, 0, PyLong_FromLong(result));
    PyTuple_SetItem(retval, 1, PyLong_FromLong(last_sequence_number + error_line_offset));
    return retval;
}


static int maxerror = -1;

static char savedError[LINELEN+1];
static PyObject *rs274_strerror(PyObject *s, PyObject *o) {
    int err;
    if(!PyArg_ParseTuple(o, "i", &err)) return NULL;
    interp_new.error_text(err, savedError, LINELEN);
    return PyUnicode_FromString(savedError);
}

static PyObject *rs274_calc_extents(PyObject *self, PyObject *args) {
    double min_x = 9e99, min_y = 9e99, min_z = 9e99,
           min_xt = 9e99, min_yt = 9e99, min_zt = 9e99,
           max_x = -9e99, max_y = -9e99, max_z = -9e99,
           max_xt = -9e99, max_yt = -9e99, max_zt = -9e99;
    for(int i=0; i<PySequence_Length(args); i++) {
        PyObject *si = PyTuple_GetItem(args, i);
        if(!si) return NULL;
        int j;
        double xs, ys, zs, xe, ye, ze, xt, yt, zt;
        for(j=0; j<PySequence_Length(si); j++) {
            PyObject *sj = PySequence_GetItem(si, j);
            PyObject *unused;
            int r;
            if(PyTuple_Size(sj) == 4)
                r = PyArg_ParseTuple(sj,
                    "O(dddOOOOOO)(dddOOOOOO)(ddd):calc_extents item",
                    &unused,
                    &xs, &ys, &zs, &unused, &unused, &unused, &unused, &unused, &unused,
                    &xe, &ye, &ze, &unused, &unused, &unused, &unused, &unused, &unused,
                    &xt, &yt, &zt);
            else
                r = PyArg_ParseTuple(sj,
                    "O(dddOOOOOO)(dddOOOOOO)O(ddd):calc_extents item",
                    &unused,
                    &xs, &ys, &zs, &unused, &unused, &unused, &unused, &unused, &unused,
                    &xe, &ye, &ze, &unused, &unused, &unused, &unused, &unused, &unused,
                    &unused, &xt, &yt, &zt);
            Py_DECREF(sj);
            if(!r) return NULL;
            max_x = std::max(max_x, xs);
            max_y = std::max(max_y, ys);
            max_z = std::max(max_z, zs);
            min_x = std::min(min_x, xs);
            min_y = std::min(min_y, ys);
            min_z = std::min(min_z, zs);
            max_xt = std::max(max_xt, xs+xt);
            max_yt = std::max(max_yt, ys+yt);
            max_zt = std::max(max_zt, zs+zt);
            min_xt = std::min(min_xt, xs+xt);
            min_yt = std::min(min_yt, ys+yt);
            min_zt = std::min(min_zt, zs+zt);
        }
        if(j > 0) {
            max_x = std::max(max_x, xe);
            max_y = std::max(max_y, ye);
            max_z = std::max(max_z, ze);
            min_x = std::min(min_x, xe);
            min_y = std::min(min_y, ye);
            min_z = std::min(min_z, ze);
            max_xt = std::max(max_xt, xe+xt);
            max_yt = std::max(max_yt, ye+yt);
            max_zt = std::max(max_zt, ze+zt);
            min_xt = std::min(min_xt, xe+xt);
            min_yt = std::min(min_yt, ye+yt);
            min_zt = std::min(min_zt, ze+zt);
        }
    }
    return Py_BuildValue("[ddd][ddd][ddd][ddd]",
        min_x, min_y, min_z,  max_x, max_y, max_z,
        min_xt, min_yt, min_zt,  max_xt, max_yt, max_zt);
}

#if PY_VERSION_HEX < 0x02050000
#define PyObject_GetAttrString(o,s) \
    PyObject_GetAttrString((o),const_cast<char*>((s)))
#define PyArg_VaParse(o,f,a) \
    PyArg_VaParse((o),const_cast<char*>((f)),(a))
#endif

static bool get_attr(PyObject *o, const char *attr_name, int *v) {
    PyObject *attr = PyObject_GetAttrString(o, attr_name);
    if(attr && PyLong_CheckAndError(attr_name, attr)) {
        *v = PyLong_AS_LONG(attr);
        Py_DECREF(attr);
        return true;
    }
    Py_XDECREF(attr);
    return false;
}

static bool get_attr(PyObject *o, const char *attr_name, double *v) {
    PyObject *attr = PyObject_GetAttrString(o, attr_name);
    if(attr && PyFloat_CheckAndError(attr_name, attr)) {
        *v = PyFloat_AsDouble(attr);
        Py_DECREF(attr);
        return true;
    }
    Py_XDECREF(attr);
    return false;
}

static bool get_attr(PyObject *o, const char *attr_name, const char *fmt, ...) {
    bool result = false;
    va_list ap;
    va_start(ap, fmt);
    PyObject *attr = PyObject_GetAttrString(o, attr_name);
    if(attr) result = PyArg_VaParse(attr, fmt, ap);
    va_end(ap);
    Py_XDECREF(attr);
    return result;
}

static void unrotate(double &x, double &y, double c, double s) {
    double tx = x * c + y * s;
    y = -x * s + y * c;
    x = tx;
}

static void rotate(double &x, double &y, double c, double s) {
    double tx = x * c - y * s;
    y = x * s + y * c;
    x = tx;
}

static PyObject *rs274_arc_to_segments(PyObject *self, PyObject *args) {
    PyObject *canon;
    double x1, y1, cx, cy, z1, a, b, c, u, v, w;
    double o[9], n[9], g5xoffset[9], g92offset[9];
    int rot, plane;
    int X, Y, Z;
    double rotation_cos, rotation_sin;
    int max_segments = 128;

    if(!PyArg_ParseTuple(args, "Oddddiddddddd|i:arcs_to_segments",
        &canon, &x1, &y1, &cx, &cy, &rot, &z1, &a, &b, &c, &u, &v, &w, &max_segments)) return NULL;
    if(!get_attr(canon, "lo", "ddddddddd:arcs_to_segments lo", &o[0], &o[1], &o[2],
                    &o[3], &o[4], &o[5], &o[6], &o[7], &o[8]))
        return NULL;
    if(!get_attr(canon, "plane", &plane)) return NULL;
    if(!get_attr(canon, "rotation_cos", &rotation_cos)) return NULL;
    if(!get_attr(canon, "rotation_sin", &rotation_sin)) return NULL;
    if(!get_attr(canon, "g5x_offset_x", &g5xoffset[0])) return NULL;
    if(!get_attr(canon, "g5x_offset_y", &g5xoffset[1])) return NULL;
    if(!get_attr(canon, "g5x_offset_z", &g5xoffset[2])) return NULL;
    if(!get_attr(canon, "g5x_offset_a", &g5xoffset[3])) return NULL;
    if(!get_attr(canon, "g5x_offset_b", &g5xoffset[4])) return NULL;
    if(!get_attr(canon, "g5x_offset_c", &g5xoffset[5])) return NULL;
    if(!get_attr(canon, "g5x_offset_u", &g5xoffset[6])) return NULL;
    if(!get_attr(canon, "g5x_offset_v", &g5xoffset[7])) return NULL;
    if(!get_attr(canon, "g5x_offset_w", &g5xoffset[8])) return NULL;
    if(!get_attr(canon, "g92_offset_x", &g92offset[0])) return NULL;
    if(!get_attr(canon, "g92_offset_y", &g92offset[1])) return NULL;
    if(!get_attr(canon, "g92_offset_z", &g92offset[2])) return NULL;
    if(!get_attr(canon, "g92_offset_a", &g92offset[3])) return NULL;
    if(!get_attr(canon, "g92_offset_b", &g92offset[4])) return NULL;
    if(!get_attr(canon, "g92_offset_c", &g92offset[5])) return NULL;
    if(!get_attr(canon, "g92_offset_u", &g92offset[6])) return NULL;
    if(!get_attr(canon, "g92_offset_v", &g92offset[7])) return NULL;
    if(!get_attr(canon, "g92_offset_w", &g92offset[8])) return NULL;

    if(plane == 1) {
        X=0; Y=1; Z=2;
    } else if(plane == 3) {
        X=2; Y=0; Z=1;
    } else {
        X=1; Y=2; Z=0;
    }
    n[X] = x1;
    n[Y] = y1;
    n[Z] = z1;
    n[3] = a;
    n[4] = b;
    n[5] = c;
    n[6] = u;
    n[7] = v;
    n[8] = w;
    for(int ax=0; ax<9; ax++) o[ax] -= g5xoffset[ax];
    unrotate(o[0], o[1], rotation_cos, rotation_sin);
    for(int ax=0; ax<9; ax++) o[ax] -= g92offset[ax];

    double theta1 = rtapi_atan2(o[Y]-cy, o[X]-cx);
    double theta2 = rtapi_atan2(n[Y]-cy, n[X]-cx);

    if(rot < 0) {
        while(theta2 - theta1 > -CIRCLE_FUZZ) theta2 -= 2*M_PI;
    } else {
        while(theta2 - theta1 < CIRCLE_FUZZ) theta2 += 2*M_PI;
    }

    // if multi-turn, add the right number of full circles
    if(rot < -1) theta2 += 2*M_PI*(rot+1);
    if(rot > 1) theta2 += 2*M_PI*(rot-1);

    int steps = std::max(3, int(max_segments * rtapi_fabs(theta1 - theta2) / M_PI));
    double rsteps = 1. / steps;
    PyObject *segs = PyList_New(steps);

    double dtheta = theta2 - theta1;
    double d[9] = {0, 0, 0, n[3]-o[3], n[4]-o[4], n[5]-o[5], n[6]-o[6], n[7]-o[7], n[8]-o[8]};
    d[Z] = n[Z] - o[Z];

    double tx = o[X] - cx, ty = o[Y] - cy, dc = rtapi_cos(dtheta*rsteps), ds = rtapi_sin(dtheta*rsteps);
    for(int i=0; i<steps-1; i++) {
        double f = (i+1) * rsteps;
        double p[9];
        rotate(tx, ty, dc, ds);
        p[X] = tx + cx;
        p[Y] = ty + cy;
        p[Z] = o[Z] + d[Z] * f;
        p[3] = o[3] + d[3] * f;
        p[4] = o[4] + d[4] * f;
        p[5] = o[5] + d[5] * f;
        p[6] = o[6] + d[6] * f;
        p[7] = o[7] + d[7] * f;
        p[8] = o[8] + d[8] * f;
        for(int ax=0; ax<9; ax++) p[ax] += g92offset[ax];
        rotate(p[0], p[1], rotation_cos, rotation_sin);
        for(int ax=0; ax<9; ax++) p[ax] += g5xoffset[ax];
        PyList_SET_ITEM(segs, i,
            Py_BuildValue("ddddddddd", p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7], p[8]));
    }
    for(int ax=0; ax<9; ax++) n[ax] += g92offset[ax];
    rotate(n[0], n[1], rotation_cos, rotation_sin);
    for(int ax=0; ax<9; ax++) n[ax] += g5xoffset[ax];
    PyList_SET_ITEM(segs, steps-1,
        Py_BuildValue("ddddddddd", n[0], n[1], n[2], n[3], n[4], n[5], n[6], n[7], n[8]));
    return segs;
}

static PyObject *bind_sockets(PyObject *self, PyObject *args) {
    char *preview_uri, *status_uri;
    if(!PyArg_ParseTuple(args, "ss", &preview_uri, &status_uri))
        return NULL;
    int rc;
    rc = zsock_bind(z_preview, "%s", preview_uri);
    if(!rc) {
	PyErr_Format(PyExc_RuntimeError,
		     "binding preview socket to '%s' failed", preview_uri);
	return NULL;
    }
    rc = zsock_bind(z_status, "%s", status_uri);
    if(!rc) {
	PyErr_Format(PyExc_RuntimeError,
		     "binding status socket to '%s' failed", status_uri);
	return NULL;
    }
    // usleep(300 *1000); // avoid slow joiner syndrome

    return Py_BuildValue("(ss)",
			 zsock_last_endpoint(z_preview),
			 zsock_last_endpoint(z_status));
}

static PyMethodDef gcode_methods[] = {
    {"parse", (PyCFunction)parse_file, METH_VARARGS, "Parse a G-Code file"},
    {"strerror", (PyCFunction)rs274_strerror, METH_VARARGS,
        "Convert a numeric error to a string"},
    {"calc_extents", (PyCFunction)rs274_calc_extents, METH_VARARGS,
        "Calculate information about extents of gcode"},
    {"arc_to_segments", (PyCFunction)rs274_arc_to_segments, METH_VARARGS,
        "Convert an arc to straight segments"},
    {"bind", (PyCFunction)bind_sockets, METH_VARARGS, "pass an IP address and return a tuple (status uri, preview uri)"},

    {NULL}
};

static struct PyModuleDef preview_moduledef = {
    PyModuleDef_HEAD_INIT,                    /* m_base    */
    "preview",                                /* m_name    */
    "rs274ngc preview module",                /* m_doc     */
    -1,                                       /* m_size    */
    gcode_methods                             /* m_methods */
};

PyMODINIT_FUNC PyInit_preview(void);

PyMODINIT_FUNC PyInit_preview(void)
{

    PyObject *m = PyModule_Create(&preview_moduledef);
    PyType_Ready(&LineCodeType);
    PyModule_AddObject(m, "linecode", (PyObject*)&LineCodeType);
    PyObject_SetAttrString(m, "MAX_ERROR", PyLong_FromLong(maxerror));
    PyObject_SetAttrString(m, "MIN_ERROR",
            PyLong_FromLong(INTERP_MIN_ERROR));
    z_init();
    Py_AtExit(z_shutdown);
    return m;
}

// vim:ts=8:sts=4:sw=4:et: