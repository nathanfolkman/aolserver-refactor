/*
 * tclhttp2.c --
 *
 *	Tcl commands for HTTP/2 (nghttp2) observability.
 */

#include "nsd.h"

#if HAVE_NGHTTP2

/*
 *----------------------------------------------------------------------
 *
 * PutHttp2StatsDict --
 *
 *	Appends all NsHttp2Stats fields to a new dict object.
 *
 *----------------------------------------------------------------------
 */

static Tcl_Obj *
PutHttp2StatsDict(Tcl_Interp *interp, const NsHttp2Stats *st)
{
    Tcl_Obj *dict = Tcl_NewDictObj();

    Tcl_DictObjPut(interp, dict, Tcl_NewStringObj("feed_ok", -1),
		   Tcl_NewWideIntObj((Tcl_WideInt) st->feed_ok));
    Tcl_DictObjPut(interp, dict, Tcl_NewStringObj("feed_mem_recv_err", -1),
		   Tcl_NewWideIntObj((Tcl_WideInt) st->feed_mem_recv_err));
    Tcl_DictObjPut(interp, dict, Tcl_NewStringObj("trysend_recoveries", -1),
		   Tcl_NewWideIntObj((Tcl_WideInt) st->trysend_recoveries));
    Tcl_DictObjPut(interp, dict, Tcl_NewStringObj("sessions_created", -1),
		   Tcl_NewWideIntObj((Tcl_WideInt) st->sessions_created));
    Tcl_DictObjPut(interp, dict, Tcl_NewStringObj("sessions_destroyed", -1),
		   Tcl_NewWideIntObj((Tcl_WideInt) st->sessions_destroyed));
    Tcl_DictObjPut(interp, dict, Tcl_NewStringObj("streams_dispatched", -1),
		   Tcl_NewWideIntObj((Tcl_WideInt) st->streams_dispatched));
    Tcl_DictObjPut(interp, dict, Tcl_NewStringObj("rst_stream_sent", -1),
		   Tcl_NewWideIntObj((Tcl_WideInt) st->rst_stream_sent));
    Tcl_DictObjPut(interp, dict, Tcl_NewStringObj("session_send_fail", -1),
		   Tcl_NewWideIntObj((Tcl_WideInt) st->session_send_fail));
    Tcl_DictObjPut(interp, dict, Tcl_NewStringObj("bytes_sent", -1),
		   Tcl_NewWideIntObj((Tcl_WideInt) st->bytes_sent));
    Tcl_DictObjPut(interp, dict, Tcl_NewStringObj("ping_recv", -1),
		   Tcl_NewWideIntObj((Tcl_WideInt) st->ping_recv));
    Tcl_DictObjPut(interp, dict, Tcl_NewStringObj("goaway_recv", -1),
		   Tcl_NewWideIntObj((Tcl_WideInt) st->goaway_recv));
    Tcl_DictObjPut(interp, dict, Tcl_NewStringObj("rst_stream_recv", -1),
		   Tcl_NewWideIntObj((Tcl_WideInt) st->rst_stream_recv));
    Tcl_DictObjPut(interp, dict, Tcl_NewStringObj("goaway_sent", -1),
		   Tcl_NewWideIntObj((Tcl_WideInt) st->goaway_sent));
    Tcl_DictObjPut(interp, dict, Tcl_NewStringObj("ping_sent", -1),
		   Tcl_NewWideIntObj((Tcl_WideInt) st->ping_sent));
    Tcl_DictObjPut(interp, dict, Tcl_NewStringObj("ping_ack_sent", -1),
		   Tcl_NewWideIntObj((Tcl_WideInt) st->ping_ack_sent));
    Tcl_DictObjPut(interp, dict, Tcl_NewStringObj("defer_appends", -1),
		   Tcl_NewWideIntObj((Tcl_WideInt) st->defer_appends));
    Tcl_DictObjPut(interp, dict, Tcl_NewStringObj("defer_max_depth", -1),
		   Tcl_NewWideIntObj((Tcl_WideInt) st->defer_max_depth));
    Tcl_DictObjPut(interp, dict, Tcl_NewStringObj("trysend_drain_reads", -1),
		   Tcl_NewWideIntObj((Tcl_WideInt) st->trysend_drain_reads));
    Tcl_DictObjPut(interp, dict, Tcl_NewStringObj("bytes_fed", -1),
		   Tcl_NewWideIntObj((Tcl_WideInt) st->bytes_fed));
    return dict;
}

/*
 *----------------------------------------------------------------------
 *
 * NsTclHttp2ObjCmd --
 *
 *	Implements "ns_http2". Subcommands:
 *	  stats ?-driver fullname?  — dict of counters (global or one driver).
 *
 * Results:
 *	Tcl result.
 *
 * Side effects:
 *	None.
 *
 *----------------------------------------------------------------------
 */

int
NsTclHttp2ObjCmd(ClientData clientData, Tcl_Interp *interp, int objc,
		 Tcl_Obj *const *objv)
{
    NsHttp2Stats st;
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
	    NsHttp2StatsGet(&st);
	    Tcl_SetObjResult(interp, PutHttp2StatsDict(interp, &st));
	    return TCL_OK;
	}
	if (objc == 4 && strcmp(Tcl_GetString(objv[2]), "-driver") == 0) {
	    drv = NsDriverFind(Tcl_GetString(objv[3]));
	    if (drv == NULL) {
		Tcl_AppendResult(interp, "no such driver: ",
			Tcl_GetString(objv[3]), NULL);
		return TCL_ERROR;
	    }
	    NsHttp2DriverStatsGet(drv, &st);
	    Tcl_SetObjResult(interp, PutHttp2StatsDict(interp, &st));
	    return TCL_OK;
	}
	Tcl_WrongNumArgs(interp, 2, objv, "?-driver fullname?");
	return TCL_ERROR;
    }

    Tcl_SetResult(interp, "bad subcommand: should be stats", TCL_STATIC);
    return TCL_ERROR;
}

#else /* !HAVE_NGHTTP2 */

int
NsTclHttp2ObjCmd(ClientData clientData, Tcl_Interp *interp, int objc,
		 Tcl_Obj *const *objv)
{
    (void) clientData;
    (void) objc;
    (void) objv;
    Tcl_SetResult(interp, "ns_http2: not available (built without nghttp2)",
		  TCL_STATIC);
    return TCL_ERROR;
}

#endif /* HAVE_NGHTTP2 */
