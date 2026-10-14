// A minimal extension consuming parse_args through its stubs table, built the
// way a downstream meson extension would (dependency('parse_args') -> stub lib +
// -DUSE_PARSE_ARGS_STUBS=1).  Not installed: only loaded by tests/stubs.test.

#include <tcl.h>
#include <parse_args.h>
#include <defer.h>		// parseargs_bind.def needs defer

#include "stubs_api.c"

#ifndef USE_PARSE_ARGS_STUBS
#	error "Expected to be built against the parse_args stubs (USE_PARSE_ARGS_STUBS)"
#endif

DLLEXPORT int Parse_argsstubsconsumer_Init(Tcl_Interp* interp)
{
	if (Tcl_InitStubs(interp, TCL_VERSION, 0) == NULL) return TCL_ERROR;
	if (ParseArgs_InitStubs(interp) == NULL) return TCL_ERROR;
	return stubs_api_register(interp, "::parse_args_stubs");
}
