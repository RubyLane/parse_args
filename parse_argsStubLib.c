#if HAVE_CONFIG_H
#	include <config.h>
#endif

#undef USE_TCL_STUBS
#undef USE_PARSE_ARGS_STUBS
#define USE_TCL_STUBS 1
#define USE_PARSE_ARGS_STUBS 1

#include <parse_args.h>

MODULE_SCOPE const Parse_argsStubs*	parse_argsStubsPtr;
const Parse_argsStubs*				parse_argsStubsPtr = NULL;

#undef Parse_argsInitializeStubs
MODULE_SCOPE const char* Parse_argsInitializeStubs(Tcl_Interp* interp)
{
	const char*	got = Tcl_PkgRequireEx(interp, PACKAGE_NAME, PACKAGE_VERSION, 0, &parse_argsStubsPtr);

	if (got && !parse_argsStubsPtr) {
		// Something provided the package without a stubs table
		Tcl_SetObjResult(interp, Tcl_ObjPrintf("%s %s was provided without a stubs table", PACKAGE_NAME, got));
		return NULL;
	}

	return got;
}
