# parse_args C API

The C API uses the same argspec semantics as the Tcl command: the scan rules,
positional binding and errors in SKILL.md all apply. On failure, every function
leaves the error message and errorcode in the interp result and returns
`TCL_ERROR`.

## Getting at it

- **Separately compiled extension (stubs):** use pkg-config, or in meson
  `dependency('parse_args')`. Either supplies `-DUSE_PARSE_ARGS_STUBS=1` and
  the stub library. `#include <parse_args.h>`, then in Init, after
  `Tcl_InitStubs`:
  ```c
  if (ParseArgs_InitStubs(interp) == NULL) return TCL_ERROR;
  ```
- **jitc cdef:** add the part `package {parse_args 0.7}`. The functions are
  linked directly, with no InitStubs needed, and `parseargs_bind.def` is on the
  include path.

## Functions

```c
int ParseArgs_BindDict(Tcl_Interp*, Tcl_Obj* argspec, Tcl_Size objc, Tcl_Obj*const objv[], Tcl_Obj** dictPtr);
```
This is the same as `parse_args $args $argspec dictvar`. `*dictPtr` must be
NULL or a held reference. It is replaced with **replace_tclobj** semantics:
the new dict is held for you, and you must release it.

```c
typedef int (ParseArgs_BindCallback)(void* cdata, Tcl_Interp*, Tcl_Size decl_idx, Tcl_Obj* name, Tcl_Obj* val);
int ParseArgs_Bind(Tcl_Interp*, Tcl_Obj* argspec, Tcl_Size objc, Tcl_Obj*const objv[], ParseArgs_BindCallback* cb, void* cdata);
```
The callback runs once per bound value, including defaults and `0` for
unsupplied `-boolean`s.
- `decl_idx` is the index of the argspec entry, counting options and
  positionals together in argspec order. It is `-1` for a `-multi` group's
  value.
- `name` is the output name: the `-name` setting, or the key without its `-`.
- `val` is only valid during the callback, so take a reference to keep it.
- Returning non-`TCL_OK` aborts the bind.

Keep the `argspec` Tcl_Obj alive across calls, for example in your command's
ClientData. The compiled spec is cached on it, and a fresh object per call
recompiles it every time.

```c
int ParseArgs_BindSlots(Tcl_Interp*, Tcl_Obj** cachePtr, const char* specstr,
                        Tcl_Size objc, Tcl_Obj*const objv[], Tcl_Size slotc, struct ParseArgs_Slot slots[]);
static inline void ParseArgs_ReleaseSlots(Tcl_Size slotc, struct ParseArgs_Slot slots[]);
```
This is the engine behind `parseargs_bind.def`, and you rarely call it
directly.
- If `*cachePtr` is NULL, the spec is built from `specstr` and stored there,
  with a reference that the caller must eventually release. If `cachePtr`
  itself is NULL, the spec is built just for this call.
- `slots[decl_idx]` says how to convert each value. Every argspec entry must
  have a slot.
- Each slot's `hold` must start NULL. Afterwards it keeps the value alive, so
  that pointers into it stay valid until `ParseArgs_ReleaseSlots`.
- **`-multi` and `-alias` are not supported.** Using them is an error, e.g.
  `-multi is not supported by ParseArgs_BindSlots`.

## parseargs_bind.def: a typed struct of the arguments

The usual way to parse args in C. Define an X-macro, then include the
`.def` file **inside the function body**:

A complete minimal jitc example:

```tcl
package require jitc 0.8
set cdef {
    options {-Wall}
    package {parse_args 0.7}
    code {
        #include <math.h>
        OBJCMD(scale) {
            #define PARSEARGS_SPEC(OPT, ARG) \
                OPT( factor, "-default 1.0", DOUBLE ) \
                OPT( round,  "-boolean",     BOOL   ) \
                ARG( value,  "-required",    DOUBLE ) \
                /* line intentionally left blank */
            #include <parseargs_bind.def>

            double r = args.value * args.factor;
            Tcl_SetObjResult(interp, Tcl_NewDoubleObj(args.round ? round(r) : r));
            return TCL_OK;
        }
    }
}
jitc::bind scale $cdef scale    ;# argspec compiled once, cached in the binding
scale -factor 2.5 -round 3      ;# 8.0
```

A fuller example, with every kind of member:

```c
OBJCMD(fetch) {                      // jitc (in an extension: a Tcl_ObjCmdProc body)
    #define PARSEARGS_SPEC(OPT, ARG) \
        OPT( verbose, "-boolean",                   BOOL    ) \
        OPT( timeout, "-default 30",                INT     ) \
        OPT( method,  "-default GET -enum {GET POST}", INDEX ) \
        OPT( body,    "",                           BYTES   ) \
        ARG( url,     "-required",                  STRING  ) \
        ARG( args,    "",                           LIST    ) \
        /* line intentionally left blank */
    #include <parseargs_bind.def>

    // args.verbose  args.timeout  args.method (0|1)  args.body/args.body_len
    // args.url      args.argsc/args.argsv
    return TCL_OK;
}
```

- **Entries.** `OPT(name, settings, type)` declares the option `-name`, and
  `ARG(name, settings, type)` a positional. *name* is both the struct member
  and the argspec key, so it must be a valid C identifier. Options can't
  contain `-` beyond the leading one.
- **Settings.** *settings* is a C string literal of the usual parse_args
  settings, and gets wrapped in `{...}` in the generated argspec. Use `""`
  for none.
- **The trailing line.** End the macro with a blank or comment line, so the
  last `\` continuation is harmless.

| type | member(s) | conversion |
|---|---|---|
| `OBJ` | `Tcl_Obj* name` (NULL if not supplied) | the value, borrowed but held |
| `BOOL` | `bool name` | Tcl_GetBooleanFromObj |
| `INT` | `int name` | Tcl_GetIntFromObj |
| `WIDEINT` | `Tcl_WideInt name` | Tcl_GetWideIntFromObj |
| `DOUBLE` | `double name` | Tcl_GetDoubleFromObj |
| `STRING` | `const char* name` | Tcl_GetString |
| `BYTES` | `const unsigned char* name; Tcl_Size name_len` | Tcl_GetBytesFromObj |
| `LIST` | `Tcl_Size namec; Tcl_Obj** namev` | list elements, for `-args n`, `-args all` and `args` |
| `INDEX` | `int name` | index into the entry's `-enum` list |
| `SKIP` | none | not stored |

- **Unsupplied entries.** An entry that isn't supplied and has no `-default`
  is left **zeroed** (NULL, 0 or false). Unlike Tcl there's no "unset" state,
  so to tell "not given" from "given 0", use `OBJ`, which is NULL when not
  given, or add a sentinel `-default`.
- **Conversion errors.** A failed conversion, such as `INT` given `abc`,
  fails the bind with Tcl's usual message.
- **Lifetime.** Pointer members (STRING, BYTES, LIST, OBJ) stay valid until
  the struct goes out of scope. They are released with **`defer`**, so the
  code must be C23 with defer support. jitc has it. Extensions can use
  teabase's `defer.h`, built with `-DHAVE_NATIVE_DEFER` /
  `-DHAVE_DEFER_POLYFILL` as detected.
- **Several binds in one function.** You can include the `.def` more than
  once in different functions. Every `PARSEARGS_*` setting is `#undef`'d
  after each include.

Overridable before the include (all are reset afterwards):

| macro | default | notes |
|---|---|---|
| `PARSEARGS_OUT` | `args` | name of the struct local |
| `PARSEARGS_INTERP` | `interp` | |
| `PARSEARGS_OBJC` / `PARSEARGS_OBJV` | `objc-1` / `objv+1` | skip the command name; adjust for subcommands (`objc-2`/`objv+2`) |
| `PARSEARGS_CACHE` | `((Tcl_Obj**)cdata)` | where the compiled argspec is cached |
| `PARSEARGS_ON_ERROR` | `return TCL_ERROR` | e.g. `goto done` for cleanup paths |

**`PARSEARGS_CACHE` gotcha.** The default suits jitc. `jitc::bind` gives
each `OBJCMD` a per-binding `Tcl_Obj*` slot as its cdata, and releases it when
the command is deleted. Under `jitc::capply`, cdata is NULL, so the spec is
rebuilt on each call, which is correct but slower. **In a hand-written
extension where cdata is something else, you must define `PARSEARGS_CACHE`.**
Point it at your own `Tcl_Obj*` (initialised to NULL, and released when the
command is deleted), or use `NULL` to rebuild each call. Otherwise your cdata
gets scribbled on.

**Custom types:** define `PARSEARGS_OUTTYPE_<T>(name)` (the member
declaration) and `PARSEARGS_SLOTFOR_<T>(out, name)` (a `struct ParseArgs_Slot`
initializer using `PARSEARGS_SLOT_CUSTOM`, a converter
`int conv(void* cdata, Tcl_Interp*, Tcl_Obj* val, void* addr)` and its cdata).
