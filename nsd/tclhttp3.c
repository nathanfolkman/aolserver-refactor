/*
 * tclhttp3.c --
 *
 *	Tcl commands for HTTP/3 (QUIC) observability.
 */

#include "nsd.h"

#if HAVE_NGHTTP3

/*
 *----------------------------------------------------------------------
 *
 * PutHttp3StatsDict --
 *
 *	Appends all NsHttp3Stats fields to a new dict object.
 *
 *----------------------------------------------------------------------
 */

static Tcl_Obj *
PutHttp3StatsDict(Tcl_Interp *interp, const NsHttp3Stats *st)
{
    Tcl_Obj *dict = Tcl_NewDictObj();

    Tcl_DictObjPut(interp, dict, Tcl_NewStringObj("packets_recv", -1),
		   Tcl_NewWideIntObj((Tcl_WideInt) st->packets_recv));
    Tcl_DictObjPut(interp, dict, Tcl_NewStringObj("packets_sent", -1),
		   Tcl_NewWideIntObj((Tcl_WideInt) st->packets_sent));
    Tcl_DictObjPut(interp, dict, Tcl_NewStringObj("bytes_recv_udp", -1),
		   Tcl_NewWideIntObj((Tcl_WideInt) st->bytes_recv_udp));
    Tcl_DictObjPut(interp, dict, Tcl_NewStringObj("bytes_sent_udp", -1),
		   Tcl_NewWideIntObj((Tcl_WideInt) st->bytes_sent_udp));
    Tcl_DictObjPut(interp, dict, Tcl_NewStringObj("conn_accepted", -1),
		   Tcl_NewWideIntObj((Tcl_WideInt) st->conn_accepted));
    Tcl_DictObjPut(interp, dict, Tcl_NewStringObj("conn_closed", -1),
		   Tcl_NewWideIntObj((Tcl_WideInt) st->conn_closed));
    Tcl_DictObjPut(interp, dict, Tcl_NewStringObj("handshake_completed", -1),
		   Tcl_NewWideIntObj((Tcl_WideInt) st->handshake_completed));
    Tcl_DictObjPut(interp, dict, Tcl_NewStringObj("handshake_fail", -1),
		   Tcl_NewWideIntObj((Tcl_WideInt) st->handshake_fail));
    Tcl_DictObjPut(interp, dict, Tcl_NewStringObj("streams_dispatched", -1),
		   Tcl_NewWideIntObj((Tcl_WideInt) st->streams_dispatched));
    Tcl_DictObjPut(interp, dict, Tcl_NewStringObj("read_pkt_err", -1),
		   Tcl_NewWideIntObj((Tcl_WideInt) st->read_pkt_err));
    Tcl_DictObjPut(interp, dict, Tcl_NewStringObj("send_fail", -1),
		   Tcl_NewWideIntObj((Tcl_WideInt) st->send_fail));
    Tcl_DictObjPut(interp, dict, Tcl_NewStringObj("version_negotiation_sent", -1),
		   Tcl_NewWideIntObj((Tcl_WideInt) st->version_negotiation_sent));
    Tcl_DictObjPut(interp, dict, Tcl_NewStringObj("defer_appends", -1),
		   Tcl_NewWideIntObj((Tcl_WideInt) st->defer_appends));
    Tcl_DictObjPut(interp, dict, Tcl_NewStringObj("defer_max_depth", -1),
		   Tcl_NewWideIntObj((Tcl_WideInt) st->defer_max_depth));
    return dict;
}

/*
 *----------------------------------------------------------------------
 *
 * NsTclHttp3ObjCmd --
 *
 *	Implements "ns_http3". Subcommands:
 *	  stats ?-driver fullname?  — dict of counters (global or one driver).
 *
 *----------------------------------------------------------------------
 */

int
NsTclHttp3ObjCmd(ClientData clientData, Tcl_Interp *interp, int objc,
		 Tcl_Obj *const *objv)
{
    NsHttp3Stats st;
    Driver *drv;
    char *sub;

    (void) clientData;
    if (objc < 2) {
	Tcl_WrongNumArgs(interp, 1, objv, "subcommand ?args?");
	return TCL_ERROR;
    }
    sub = Tcl_GetString(objv[1]);

    if (strcmp(sub, "stats") == 0) {
	if (objc == 2) {
	    NsHttp3StatsGet(&st);
	    Tcl_SetObjResult(interp, PutHttp3StatsDict(interp, &st));
	    return TCL_OK;
	}
	if (objc == 4 && strcmp(Tcl_GetString(objv[2]), "-driver") == 0) {
	    drv = NsDriverFind(Tcl_GetString(objv[3]));
	    if (drv == NULL) {
		Tcl_AppendResult(interp, "no such driver: ",
			Tcl_GetString(objv[3]), NULL);
		return TCL_ERROR;
	    }
	    NsHttp3DriverStatsGet(drv, &st);
	    Tcl_SetObjResult(interp, PutHttp3StatsDict(interp, &st));
	    return TCL_OK;
	}
	Tcl_WrongNumArgs(interp, 2, objv, "?-driver fullname?");
	return TCL_ERROR;
    }

    Tcl_SetResult(interp, "bad subcommand: should be stats", TCL_STATIC);
    return TCL_ERROR;
}

#else /* !HAVE_NGHTTP3 */

int
NsTclHttp3ObjCmd(ClientData clientData, Tcl_Interp *interp, int objc,
		 Tcl_Obj *const *objv)
{
    (void) clientData;
    (void) objc;
    (void) objv;
    Tcl_SetResult(interp, "ns_http3: not available (built without HTTP/3)",
		  TCL_STATIC);
    return TCL_ERROR;
}

#endif /* HAVE_NGHTTP3 */
