// Commands exercising the parse_args C API from a separately compiled consumer.
// Included by the jitc cdef in stubs.test (linked directly against the package
// library) and by stubs_consumer.c (a meson-built extension linked through the
// stubs table), so it uses only plain Tcl and parse_args API calls (plus
// defer, which parseargs_bind.def needs).

#include <string.h>
#include <stdbool.h>

static void api_replace(Tcl_Obj** target, Tcl_Obj* replacement) //<<<
{
	if (*target == replacement) return;
	if (replacement) Tcl_IncrRefCount(replacement);
	if (*target) Tcl_DecrRefCount(*target);
	*target = replacement;
}

//>>>
static int api_bind_dict(void* cdata, Tcl_Interp* interp, int objc, Tcl_Obj*const objv[]) //<<<
{
	// bind_dict argspec ?arg ...?
	(void)cdata;
	if (objc < 2) {Tcl_WrongNumArgs(interp, 1, objv, "argspec ?arg ...?"); return TCL_ERROR;}

	Tcl_Obj*	dict = NULL;
	const int	code = ParseArgs_BindDict(interp, objv[1], objc-2, objv+2, &dict);
	if (code == TCL_OK) Tcl_SetObjResult(interp, dict);
	api_replace(&dict, NULL);
	return code;
}

//>>>
static int collect(void* cdata, Tcl_Interp* interp, Tcl_Size decl_idx, Tcl_Obj* name, Tcl_Obj* val) //<<<
{
	Tcl_Obj*	res = cdata;
	return Tcl_ListObjAppendElement(interp, res, Tcl_NewListObj(3, (Tcl_Obj*[]){
		Tcl_NewWideIntObj(decl_idx),
		name,
		val,
	}));
}

//>>>
static int api_bind_cb(void* cdata, Tcl_Interp* interp, int objc, Tcl_Obj*const objv[]) //<<<
{
	// bind_cb argspec ?arg ...?  -> list of {decl_idx name val} for each callback
	(void)cdata;
	if (objc < 2) {Tcl_WrongNumArgs(interp, 1, objv, "argspec ?arg ...?"); return TCL_ERROR;}

	Tcl_Obj*	res = NULL;
	api_replace(&res, Tcl_NewListObj(0, NULL));
	const int	code = ParseArgs_Bind(interp, objv[1], objc-2, objv+2, collect, res);
	if (code == TCL_OK) Tcl_SetObjResult(interp, res);
	api_replace(&res, NULL);
	return code;
}

//>>>
static int api_foo(void* cdata, Tcl_Interp* interp, int objc, Tcl_Obj*const objv[]) //<<<
{
	// cdata is a Tcl_Obj** slot (the same shape as a jitc binding's cdata)
	#define PARSEARGS_SPEC(OPT, ARG) \
		OPT( special,	"-boolean",		BOOL	) \
		ARG( a1,		"-required",	STRING	) \
		ARG( a2,		"-default 42",	INT		) \
		ARG( a3,		"-required",	WIDEINT	) \
		/* line intentionally left blank */
	#include <parseargs_bind.def>

	Tcl_SetObjResult(interp, Tcl_NewListObj(4, (Tcl_Obj*[]){
		Tcl_NewBooleanObj(args.special),
		Tcl_NewStringObj(args.a1, -1),
		Tcl_NewIntObj(args.a2),
		Tcl_NewWideIntObj(args.a3),
	}));
	return TCL_OK;
}

//>>>
static int api_types(void* cdata, Tcl_Interp* interp, int objc, Tcl_Obj*const objv[]) //<<<
{
	// Every out-type, with the argspec built for each call (no cache)
	(void)cdata;
	#define PARSEARGS_SPEC(OPT, ARG) \
		OPT( obj,		"",							OBJ		) \
		OPT( wide,		"-default 5000000000",		WIDEINT	) \
		OPT( dbl,		"-default 1.5",				DOUBLE	) \
		OPT( bytes,		"",							BYTES	) \
		OPT( mode,		"-enum {red green blue} -default green",	INDEX	) \
		OPT( ignored,	"",							SKIP	) \
		OPT( pair,		"-args 2",					LIST	) \
		ARG( str,		"-required",				STRING	) \
		ARG( args,		"",							LIST	) \
		/* line intentionally left blank */
	#define PARSEARGS_OUT	a
	#define PARSEARGS_CACHE	NULL
	#include <parseargs_bind.def>

	Tcl_Obj*	res = Tcl_NewListObj(0, NULL);
	#define ADD(...) Tcl_ListObjAppendElement(NULL, res, Tcl_ObjPrintf(__VA_ARGS__))
	ADD("obj=%s",	a.obj ? Tcl_GetString(a.obj) : "<NULL>");
	ADD("wide=%" TCL_LL_MODIFIER "d", a.wide);
	ADD("dbl=%g",	a.dbl);
	ADD("bytes=%d",	(int)a.bytes_len);
	ADD("mode=%d",	a.mode);
	ADD("pair=%d",	(int)a.pairc);
	if (a.pairc == 2)
		ADD("pair1=%s", Tcl_GetString(a.pairv[1]));
	ADD("str=%s/%d", a.str, (int)strlen(a.str));
	ADD("args=%d",	(int)a.argsc);
	#undef ADD
	Tcl_SetObjResult(interp, res);
	return TCL_OK;
}

//>>>
static int api_custom_error(void* cdata, Tcl_Interp* interp, int objc, Tcl_Obj*const objv[]) //<<<
{
	// PARSEARGS_ON_ERROR, and errors from parse_args itself
	(void)cdata;
	#define PARSEARGS_SPEC(OPT, ARG) \
		ARG( n,	"-required",	INT	) \
		/* line intentionally left blank */
	#define PARSEARGS_CACHE		NULL
	#define PARSEARGS_ON_ERROR	{ Tcl_AppendResult(interp, " (custom)", NULL); return TCL_ERROR; }
	#include <parseargs_bind.def>

	Tcl_SetObjResult(interp, Tcl_NewIntObj(args.n * 2));
	return TCL_OK;
}

//>>>
static void free_cache_slot(void* cdata) //<<<
{
	Tcl_Obj**	slot = cdata;
	api_replace(slot, NULL);
	Tcl_Free((char*)slot);
}

//>>>
static int stubs_api_register(Tcl_Interp* interp, const char* ns) //<<<
{
	static const struct {
		const char*		name;
		Tcl_ObjCmdProc*	proc;
		bool			cache_slot;
	} cmds[] = {
		{"bind_dict",		api_bind_dict},
		{"bind_cb",			api_bind_cb},
		{"foo",				api_foo,			true},
		{"types",			api_types},
		{"custom_error",	api_custom_error},
		{NULL}
	};

	if (Tcl_CreateNamespace(interp, ns, NULL, NULL) == NULL) return TCL_ERROR;
	for (int i=0; cmds[i].name; i++) {
		Tcl_Obj*	cmdname = Tcl_ObjPrintf("%s::%s", ns, cmds[i].name);
		Tcl_IncrRefCount(cmdname);
		if (cmds[i].cache_slot) {
			// Outside of jitc the consumer arranges its own argspec cache slot
			Tcl_Obj**	slot = (Tcl_Obj**)Tcl_Alloc(sizeof(Tcl_Obj*));
			*slot = NULL;
			Tcl_CreateObjCommand(interp, Tcl_GetString(cmdname), cmds[i].proc, slot, free_cache_slot);
		} else {
			Tcl_CreateObjCommand(interp, Tcl_GetString(cmdname), cmds[i].proc, NULL, NULL);
		}
		Tcl_DecrRefCount(cmdname);
	}
	return TCL_OK;
}

//>>>
// vim: ft=c foldmethod=marker foldmarker=<<<,>>> ts=4 shiftwidth=4 noexpandtab
