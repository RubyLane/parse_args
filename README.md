# NAME

parse_args - Core-style argument parsing for scripts

# SYNOPSIS

**package require parse_args** ?0.7.1?

**parse_args::parse_args** *args* *argspec* ?*varname*?

# DESCRIPTION

The commands provided by the Tcl core follow a pattern of optional,
named arguments (named with a leading “-” to indicate an option) in
addition to positional arguments. They strongly prefer named arguments
when the number of arguments and particularly optional arguments grows
beyond about 3. Tcl scripts however have no access to standard argument
parsing facilities to implement this pattern and have to build their
named argument parsing machinery from scratch. This is slow to do in
script, error prone, and creates noise in the code that distracts from
the core task of the command implementation. This means that
script-defined commands generally present half-baked argument handling
APIs and tend to over-use positional arguments.

This package aims to address this weakness by providing a robust
argument parsing mechanism to script code that can implement the full
set of patterns established by the core Tcl commands, is terse and
readable to specify the arguments, and is fast enough to use in any
situation.

# COMMANDS

**parse_args::parse_args** *args* *argspec*  
Parse the arguments supplied in *args* against the specification given
in *argspec*, setting variables in the current scope as defined in the
*argspec*. If the optional *varname* argument is supplied, it names a
variable that will have a dictionary written to it containing the parsed
arguments and no variables will be created for the arguments. See
**ARGUMENT SPECIFICATION** below for the format of *argspec*.

# ARGUMENT SPECIFICATION

The syntax of the argument specification (given in the *argspec*
argument to the **parse_args** command) is a dictionary, with the keys
corresponding to the names by which the arguments will be available to
the caller, either as variables created by the **parse_args** command or
keys in the dictionary written to *varname* if that was specified. If
the first character of the key is a “-” character it defines a named
option, otherwise a positional parameter. Named options and positional
parameters may be freely interleaved in the *argspec*: positional
parameters are bound in the order they appear in it, and the order of
the named options is irrelevant. (The arguments being parsed still need
to supply any named options before the positional arguments.) Each key
may only appear once: a repeated key is an error. Like in proc argument
definitions the name `args` is special and will consume all remaining
arguments (after the leading positionals are bound and modulo any
reservation for trailing **-required** positionals - see below for
details); equivalently a positional may use **-args all** to opt into
the same behaviour under a different name.

The values of the dictionary contain the definition of the corresponding
argument, such as `-required`, which marks that argument as being
required (whether a named or positional argument). The options that are
available to define the argument are:

**-default** *val*  
Provides a default value for the argument if it wasn’t supplied. Without
this an argument that wasn’t supplied won’t have its corresponding
variable (or dictionary key) set. *val* is permitted to be a value that
is outside of the range of possible values that would be accepted from
the caller, by design.

**-required**  
If this boolean option is given, the argument is flagged as required.
Failing to supply the arg causes the **parse_args** to throw an
exception similar to what would happen if a required positional argument
to a proc was not provided.

**-validate** *cmdprefix*  
Append the value supplied for this argument to the *cmdprefix* and run
it. The value is rejected if an exception is thrown or the value
returned isn’t a Tcl boolean true value.

**-name** *newname*  
Change the name of the variable / dictionary key that will receive the
value for this argument to *newname*. By default the name will be the
argspec key (without the leading “-” for named arguments).

**-boolean**  
Flag this argument as a boolean, similar to **-nocase** to `regexp`. The
variable will always be defined and be a boolean, true if the argument
was supplied and false if it wasn’t. Only valid for named options; using
**-boolean** on a positional argument raises an error at spec compile
time.

**-args** *count*  
By default named arguments consume a single following argument as the
value for that argument. This setting changes that to *count*, which
will cause the following *count* arguments to be gathered into the
variable holding this argument. If *count* is the special value “all”,
then all remaining arguments are gathered into a list as the value of
this argument. A negative *count* is an error, and a *count* of 0 is
only valid for named options (equivalent to **-boolean**); using **-args
0** on a positional argument raises an error at spec compile time.

**-enum** *possible_values*  
Restrict the values that will be accepted for this argument to those in
the *possible_values* list. Similar to using a validator that checks for
inclusion in that list but with additional internal performance
optimizations.

**-#** *free_form_text*  
Embed a comment for this argument. The supplied *free_form_text* is
ignored by **parse_args** and serves only to document the argument to
programmers reading the code. In some future version this text may be
incorporated into a usage error message that is generated when argument
parsing fails.

**-multi**  
Group a set of defined arguments together, so they act like mutually
exclusive flags. Used to implement patterns like the arguments `-ascii`,
`-dictionary`, `-integer` and `-real` arguments to the core **lsort**
command. *name* will be the name of the variable that holds the value of
the flag that was supplied. A **-default** setting on any of the linked
**-multi** arguments (those that share a *name*) will supply a default
value in *name* if none were passed. A **-required** setting on any of
the linked **-multi** arguments will require that the caller supply at
least one of them. If multiple different flags were given, the one last
in the argument list applies, unless **-all** was specified (on any
linked option), in which case all instances are grouped as a list in
*name*, in the order they were specified. **-multi** is only valid for
named options, and can’t be combined with **-enum**; either raises an
error at spec compile time.

**-alias**  
Treat the value supplied for this argument as a variable name and bind
the resulting variable for this argument to the variable of that name in
the call frame above this one (upvar 1).

**-all**  
Collect all the instances of this argument as a list, rather than the
default behaviour of using the last instance as the value.

**-end**  
Treat the remaining arguments as if **--** directly followed this
option - that is: all words in *args* after those consumed by the
current option are treated as positional arguments.

# POSITIONAL BINDING

Positional parameters are bound left-to-right with one element of
forward lookahead. Each positional **p** is processed in order:

- If **p** is `args` (or has **-args all**): *p* consumes all remaining
  input arguments, except that arguments needed to satisfy any *later*
  **-required** positionals are reserved for them. If after that
  reservation *p* would consume zero arguments and no **-default** was
  supplied, the variable for *p* is left unset.

- If **p** is **-required** (without a **-default**): *p* consumes its
  *max(1, arg_count)* arguments. If too few input arguments remain to
  satisfy this it is a usage error.

- Otherwise (optional, or **-required** with a **-default**): *p*
  consumes its *arg_count* arguments only when at least *arg_count* +
  (count needed for later **-required** positionals) arguments remain.
  Otherwise *p*’s **-default** is applied (if any) and consumption skips
  to the next positional.

This means optional positionals between or before **-required**
positionals are skipped automatically when the caller supplies just
enough arguments to satisfy the **-required** ones, rather than greedily
filling the optional and erroring on the missing required. An *argspec*
may contain at most one `args`/**-args all** positional, and any
positional that follows it must be **-required** without a **-default**.

The string representation of an input argument is generated only for
arguments that fall within the named-option scan (i.e. arguments before
the first non-option, before `--`, and before any reservation for
trailing **-required** positionals). Arguments that are reserved for
**-required** trailing positionals are bound by position only and never
have their first character inspected, so shimmer-hostile values -
e.g. large pure bytearrays whose string form is expensive to
materialise - pass through without forcing the conversion. It also means
that such an argument is bound to its positional even if it starts with
“-”.

# ERRORS

An invalid *argspec* is reported when it is first compiled, which is the
first time it is used to parse arguments (or the first time after it has
lost its internal representation). Usage errors are reported with these
errorcodes:

`TCL LOOKUP INDEX option` *name*  
*name* (which starts with “-” and is in the named-option scan) isn’t a
named option in the *argspec*. The message lists the valid options, as
for **Tcl_GetIndexFromObj**.

`TCL LOOKUP INDEX` *option* *value*  
*value* isn’t one of the **-enum** values for *option*.

`TCL WRONGARGS`  
More arguments were supplied than the *argspec* can bind, or a named
option was missing its value(s). The message is a usage generated from
the *argspec*, in the usual **wrong \# args: should be** form but
without a command name, since **parse_args** can’t know which name is
right (it may be parsing a proc’s *args*, values passed into a resumed
coroutine, a script’s *argv*, …). A caller that wants one can add it
when handling the error.

`PARSE_ARGS REQUIRED` *name*  
The **-required** argument *name* wasn’t supplied.

`PARSE_ARGS REQUIRED_ONE_OF` *names*  
None of the **-multi** group *names*, one of which is **-required**, was
supplied.

`PARSE_ARGS WRONGARGS` *name*  
Too few arguments remained to fill the **-required** positional *name*,
which has **-args** *count* greater than 1.

`PARSE_ARGS VALIDATION` *name*  
The **-validate** command prefix rejected the value supplied for *name*.

# C API

The same argument parsing is available to C code, through a Tcl stubs
table. A separately compiled extension depends on **parse_args**
(pkg-config or meson **dependency(‘parse_args’)**, which supply
**-DUSE_PARSE_ARGS_STUBS=1** and the stub library), includes
**parse_args.h** and calls **ParseArgs_InitStubs**(*interp*) in its
Init. Code compiled by **jitc** with a **package {parse_args 0.7.1}**
part calls the functions directly.

int **ParseArgs_BindDict**(*interp*, *argspec*, *objc*, *objv*, *dictPtr*)  
Bind *objc*/*objv* against the *argspec* Tcl_Obj, as the **parse_args**
command does with a *varname*, leaving the result dict in \**dictPtr*
(which must point to NULL or an object reference, which is replaced:
**replace_tclobj** semantics).

int **ParseArgs_Bind**(*interp*, *argspec*, *objc*, *objv*, *cb*, *cdata*)  
Bind *objc*/*objv* against the *argspec* Tcl_Obj, calling *cb*(*cdata*,
*interp*, *decl_idx*, *name*, *val*) for each value bound. *decl_idx* is
the index of the *argspec* entry that the value is for, counting both
options and positional parameters in the order they appear in *argspec*
(or -1 for the value of a **-multi** group), and *name* is its output
name. *val* is only guaranteed to be valid during the callback.

int **ParseArgs_BindSlots**(*interp*, *cachePtr*, *specstr*, *objc*, *objv*, *slotc*, *slots*)  
Bind *objc*/*objv* against the *argspec* in \**cachePtr*, which is
created from the string *specstr* when \**cachePtr* is NULL (and kept
there, with a reference held, for the caller to release), or for this
call only if *cachePtr* is NULL. Each value is converted as directed by
*slots*\[*decl_idx*\], which must have an entry for each *argspec*
entry, and stored at its *addr*. A reference to the value is kept in the
slot’s *hold* member (which must initially be NULL), keeping pointers
into it valid until the caller releases it with
**ParseArgs_ReleaseSlots**(*slotc*, *slots*). **-multi** and **-alias**
are not supported. This is the mechanism behind **parseargs_bind.def**.

All three leave an error message (and errorcode, as for **parse_args**)
in the interp result on failure.

## parseargs_bind.def

**parseargs_bind.def** declares and binds a struct local in a C command
implementation, from an X-macro that declares the *argspec*:

``` c
static int foo(void* cdata, Tcl_Interp* interp, int objc, Tcl_Obj*const objv[])
{
    #define PARSEARGS_SPEC(OPT, ARG) \
        OPT( special,  "-boolean",     BOOL    ) \
        ARG( a1,       "-required",    STRING  ) \
        ARG( a2,       "-default 42",  INT     ) \
        ARG( a3,       "-required",    WIDEINT ) \
        /* line intentionally left blank */
    #include <parseargs_bind.def>

    // args.special, args.a1, args.a2, args.a3 ...
    return TCL_OK;
}
```

Each entry is **OPT**(*name*, *settings*, *type*) for an option
(**-**\_name\_) or **ARG**(*name*, *settings*, *type*) for a positional
parameter. *name* is both the C struct member and the *argspec* name (so
options are limited to names that are valid C identifiers), *settings*
is a string literal with the *argspec* settings for the entry, and
*type* selects the member’s C type and the conversion:

| *type*      | Member(s)                                          | Conversion                                              |
|-------------|----------------------------------------------------|---------------------------------------------------------|
| **OBJ**     | Tcl_Obj\* *name*                                   | The value itself (NULL if not supplied)                 |
| **BOOL**    | bool *name*                                        | Tcl_GetBooleanFromObj                                   |
| **INT**     | int *name*                                         | Tcl_GetIntFromObj                                       |
| **WIDEINT** | Tcl_WideInt *name*                                 | Tcl_GetWideIntFromObj                                   |
| **DOUBLE**  | double *name*                                      | Tcl_GetDoubleFromObj                                    |
| **STRING**  | const char\* *name*                                | Tcl_GetString                                           |
| **BYTES**   | const unsigned char\* *name*, Tcl_Size *name*\_len | Tcl_GetBytesFromObj                                     |
| **LIST**    | Tcl_Size *name*c, Tcl_Obj\*\* *name*v              | Tcl_ListObjGetElements (for **-args** *n* and **args**) |
| **INDEX**   | int *name*                                         | Index of the value in the entry’s **-enum** choices     |
| **SKIP**    | (none)                                             | Not stored                                              |

Further types can be added by defining
**PARSEARGS_OUTTYPE\_**\_type\_(*name*) (the member declaration) and
**PARSEARGS_SLOTFOR\_**\_type\_(*out*, *name*) (the **struct
ParseArgs_Slot** initializer, using **PARSEARGS_SLOT_CUSTOM** and a
converter function). Members for entries that are not supplied and have
no **-default** are left zeroed.

Values are held until the struct goes out of scope, so pointer members
stay valid until then. The release is done with **defer**, so the code
including **parseargs_bind.def** must be compiled as C23 with **defer**
support (as **jitc** cdefs are). These macros may be defined before the
include to override the defaults, and are all undefined afterwards:

**PARSEARGS_OUT**  
The name of the struct local, default **args**.

**PARSEARGS_INTERP**, **PARSEARGS_OBJC**, **PARSEARGS_OBJV**  
Defaults **interp**, **objc-1** and **objv+1**.

**PARSEARGS_CACHE**  
A **Tcl_Obj\*\*** in which to cache the compiled *argspec* across calls,
or NULL to build it for each call. The default,
**((Tcl_Obj\*\*)cdata)**, is the per-binding slot that **jitc::bind**
provides as the ClientData of a jitc **OBJCMD** (and jitc releases it
when the command is deleted). Under **jitc::capply** cdata is NULL, so
the *argspec* is built for each call. Other code must supply its own
slot, and release it.

**PARSEARGS_ON_ERROR**  
The statement to run when binding fails, default **return TCL_ERROR**.

# EXAMPLES

Mimic the argument handling of the core `glob` command:

``` tcl
proc glob args {
    parse_args::parse_args $args {
        -directory  {}
        -join       {-boolean}
        -nocomplain {-boolean}
        -path       {}
        -tails      {-boolean}
        -types      {-default {}}
        args        {-name patterns}
    }

    if {$join} {
        set patterns    [list [file join {*}$patterns]]
    }

    if {[llength $patterns] == 0 && $nocomplain} return

    foreach pattern $patterns {
        if {[info exists directory]} {
            ...
        }
    }
    ...
}
```

Mimic `regex`:

``` tcl
proc regexp args {
    parse_args::parse_args $args {
        -about      {-boolean}
        -expanded   {-boolean}
        -indices    {-boolean}
        -line       {-boolean}
        -linestop   {-boolean}
        -lineanchor {-boolean}
        -nocase     {-boolean}
        -all        {-boolean}
        -inline     {-boolean}
        -start      {-default 0}
        exp         {-required}
        string      {-required}
        matchvar    {}
        args        {-name submatchvars}
    }
    ...
}
```

`lsort`:

``` tcl
proc lsort args {
    parse_args::parse_args $args {
        -ascii      {-name compare_as -multi -default ascii}
        -dictionary {-name compare_as -multi}
        -integer    {-name compare_as -multi}
        -real       {-name compare_as -multi}

        -command    {}

        -increasing {-name order -multi -default increasing}
        -decreasing {-name order -multi}

        -indices    {-boolean}
        -index      {}
        -stride     {-default 1}
        -nocase     {-boolean}
        -unique     {-boolean}
        list        {-required}
    }

    if {![info exists command]} {
        switch -- $compare_as {
            ascii {
                set command {string compare}
                if {$nocase} {
                    lappend command -nocase
                }
            }
            dictionary {
                ...
            }
            integer - real {
                set command tcl::mathop::-
            }
        }
    }
}
```

`lsearch`:

``` tcl
proc lsearch args {
    parse_args::parse_args $args {
        -exact      {-name matchtype -multi}
        -glob       {-name matchtype -multi -default glob}
        -regexp     {-name matchtype -multi}

        -sorted     {-boolean}
        -all        {-boolean}
        -inline     {-boolean}
        -not        {-boolean}
        -start      {-default 0}

        -ascii      {-name compare_as -multi -default ascii}
        -dictionary {-name compare_as -multi}
        -integer    {-name compare_as -multi}
        -real       {-name compare_as -multi}

        -nocase     {-boolean}

        -decreasing {-name order -multi}
        -increasing {-name order -multi -default increasing}
        -bisect     {-boolean}

        -index      {}
        -subindices {-boolean}
    }

    if {$sorted && $matchtype in {glob regexp}} {
        error "-sorted is mutually exclusive with -glob and -regexp"
    }
}
```

A Tk widget - `entry`:

``` tcl
proc entry {widget args} {
    parse_args::parse_args $args {
        -disabledbackground {-default {}}
        -disabledforeground {-default {}}
        -invalidcommand     {-default {}}
        -readonlybackground {-default {}}
        -show               {}
        -state              {-default normal -enum {
            normal disabled readonly
        }}
        -validate           {-default none -enum {
            none focus focusin focusout key all
        }}
        -validatecommand    {-default {}}
        -width              {-default 0}
        -textvariable       {}
    }
}
```

# SEE ALSO

The paper presented at the 2016 Tcl Conference which discusses the
approach and design choices made in this package in much greater depth:
https://www.tcl-lang.org/community/tcl2016/assets/talk33/parse_args-paper.pdf

# BUGS

Please open an issue on the github tracker for the project if you
encounter any problems: https://github.com/RubyLane/parse_args/issues

# LICENSE

This package is Copyright 2015-2026 Cyan Ogilvie, and is made available
under the same license terms as the Tcl Core
