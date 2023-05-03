% parse_args(3) 0.5 | Advanced argument parsing for Tcl
% Cyan Ogilvie
% 0.5


# NAME

parse\_args - Core-style argument parsing for scripts


# SYNOPSIS

**package require parse_args** ?0.5?

**parse_args::parse_args** *args* *argspec* ?*varname*?


# DESCRIPTION

The commands provided by the Tcl core follow a pattern of optional, named arguments
(named with a leading "-" to indicate an option) in addition to positional arguments.
They strongly prefer named arguments when the number of arguments and particularly
optional arguments grows beyond about 3.  Tcl scripts however have no access to
standard argument parsing facilities to implement this pattern and have to build
their named argument parsing machinery from scratch.  This is slow to do in script,
error prone, and creates noise in the code that distracts from the core task of the
command implementation.  This means that script-defined commands generally present
half-baked argument handling APIs and tend to over-use positional arguments.

This package aims to address this weakness by providing a robust argument parsing
mechanism to script code that can implement the full set of patterns established
by the core Tcl commands, is terse and readable to specify the arguments, and is
fast enough to use in any situation.


# COMMANDS

**parse_args::parse_args** *args* *argspec*
:   Parse the arguments supplied in *args* against the specification given in *argspec*,
    setting variables in the current scope as defined in the *argspec*.  If the
    optional *varname* argument is supplied, it names a variable that will have
    a dictionary written to it containing the parsed arguments and no variables will
    be created for the arguments.  See **ARGUMENT SPECIFICATION** below for the
    format of *argspec*.


# ARGUMENT SPECIFICATION

The syntax of the argument specification (given in the *argspec* argument to the
**parse_args** command) is a dictionary, with the keys corresponding to the names
by which the arguments will be available to the caller, either as variables created
by the **parse_args** command or keys in the dictionary written to *varname* if
that was specified.  If the first character of the key is a "-" character it
defines a named option, otherwise a positional parameter.  Like in proc argument
definitions the name `args` is special, and will consume all remaining arguments.

The values of the dictionary contain the definition of the corresponding argument,
such as `-required`, which marks that argument as being required (whether a named
or positional argument).  The options that are available to define the argument
are:

**-default** *val*
:   Provides a default value for the argument if it wasn't supplied.  Without
    this an argument that wasn't supplied won't have its corresponding variable
    (or dictionary key) set.  *val* is permitted to be a value that is outside
    of the range of possible values that would be accepted from the caller, by
    design.

**-required**
:   If this boolean option is given, the argument is flagged as required.  Failing
    to supply the arg causes the **parse_args** to throw an exception similar
    to what would happen if a required positional argument to a proc was not
    provided.

**-validate** *cmdprefix*
:   Append the value supplied for this argument to the *cmdprefix* and run it.
    The value is rejected if an exception is thrown or the value returned isn't
    a Tcl boolean true value.

**-name** *newname*
:   Change the name of the variable / dictionary key that will receive the value
    for this argument to *newname*.  By default the name will be the argspec key
    (without the leading "-" for named arguments).

**-boolean**
:   Flag this argument as a boolean, similar to **-nocase** to `regexp`.  The
    variable will always be defined and be a boolean, true if the argument was
    supplied and false if it wasn't.

**-args** *count*
:   By default named arguments consume a single following argument as the value
    for that argument.  This setting changes that to *count*, which will cause
    the following *count* arguments to be gathered into the variable holding
    this argument.  If *count* is the special value "all", then all remaining
    arguments are gathered into a list as the value of this argument.

**-enum** *possible_values*
:   Restrict the values that will be accepted for this argument to those in the
    *possible_values* list.  Similar to using a validator that checks for
    inclusion in that list but with additional internal performance optimizations.

**-#** *free_form_text*
:   Embed a comment for this argument.  The supplied *free_form_text* is ignored
    by **parse_args** and serves only to document the argument to programmers
    reading the code.  In some future version this text may be incorporated
    into a usage error message that is generated when argument parsing fails.

**-multi** *name*
:   Group a set of defined arguments together, so they act like mutually exclusive
    flags.  Used to implement patterns like the arguments `-ascii`, `-dictionary`,
    `-integer` and `-real` arguments to the core **lsort** command.
    *name* will be the name of the variable that holds the value of the flag that
    was supplied.  A **-default** setting on any of the linked **-multi** arguments
    (those that share a *name*) will supply a default value in *name* if none were
    passed.  A **-required** setting on any of the linked **-multi** arguments
    will require that the caller supply at least one of them.  If multiple
    different flags were given, the one last in the argument list applies, unless
    **-all** was specified (on any linked option), in which case all instances
    are grouped as a list in *name*, in the order they were specified.

**-alias**
:   Treat the value supplied for this argument as a variable name and bind the
    resulting variable for this argument to the variable of that name in the
    call frame above this one (upvar 1).

**-all**
:   Collect all the instances of this argument as a list, rather than the default
    behaviour of using the last instance as the value.

**-end**
:   Treat the remaining arguments as if **\-\-** directly followed this option - that
    is: all words in *args* after those consumed by the current option are treated
    as positional arguments.

# EXAMPLES

Mimic the argument handling of the core `glob` command:

~~~tcl
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
~~~

Mimic `regex`:

~~~tcl
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
~~~

`lsort`:

~~~tcl
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
~~~

`lsearch`:

~~~tcl
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
~~~

A Tk widget - `entry`:

~~~tcl
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
~~~


# SEE ALSO

The paper presented at the 2016 Tcl Conference which discusses the approach
and design choices made in this package in much greater depth:
https://www.tcl-lang.org/community/tcl2016/assets/talk33/parse_args-paper.pdf


# BUGS

Please open an issue on the github tracker for the project if you encounter
any problems: https://github.com/RubyLane/parse_args/issues


# LICENSE

This package is Copyright 2023 Cyan Ogilvie, and is made available under the
same license terms as the Tcl Core

