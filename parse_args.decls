library parse_args
interface parse_args

# Bind objc/objv against argspec, calling cb for each bound value
declare 0 generic {
	int ParseArgs_Bind(Tcl_Interp* interp, Tcl_Obj* argspec, Tcl_Size objc, Tcl_Obj*const objv[], ParseArgs_BindCallback* cb, void* cdata)
}
# Bind objc/objv against argspec, leaving the result in *dict (replace_tclobj semantics)
declare 1 generic {
	int ParseArgs_BindDict(Tcl_Interp* interp, Tcl_Obj* argspec, Tcl_Size objc, Tcl_Obj*const objv[], Tcl_Obj** dict)
}
# Bind objc/objv against the argspec cached in *cache (built from specstr if
# NULL, or a temporary if cache is NULL), converting each value into
# slots[decl_idx].  Used by parseargs_bind.def
declare 2 generic {
	int ParseArgs_BindSlots(Tcl_Interp* interp, Tcl_Obj** cache, const char* specstr, Tcl_Size objc, Tcl_Obj*const objv[], Tcl_Size slotc, struct ParseArgs_Slot slots[])
}

# vim: ft=tcl
