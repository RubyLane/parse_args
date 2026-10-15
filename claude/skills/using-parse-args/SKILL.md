---
name: using-parse-args
description: Parse proc arguments in Tcl with the parse_args package - core-style named options (-flag, -opt value), positional parameters, defaults, -required, -enum, -validate, mutually exclusive -multi flag groups, -all, -alias (upvar) and -end passthrough - and the exact rules for where option scanning stops and how positionals bind. Also covers the C API (ParseArgs_Bind*, parseargs_bind.def for jitc). Use when writing or reviewing Tcl code that calls parse_args::parse_args, when designing a Tcl proc's argument interface, or when the user mentions parse_args or argspecs.
---

# parse_args

`parse_args::parse_args args argspec ?varname?` parses a list of words against
a dict-shaped *argspec*. It sets variables in the caller's frame, or, when given
*varname*, writes a dict to that variable instead. It is implemented in C and
compiles and caches the argspec on the argspec object, so it is fast enough to
use in any proc.

```tcl
package require parse_args
namespace import parse_args::parse_args   ;# optional, the command is exported

proc fetch args {
    parse_args $args {
        -timeout    {-default 30 -validate {string is integer -strict}}
        -method     {-default GET -enum {GET HEAD POST PUT DELETE}}
        -headers    {-default {}}
        -follow     {-boolean}
        -body       {}
        url         {-required}
    }
    if {[info exists body]} { ... }     ;# options with no -default may be unset
    ...
}
fetch -method POST -body $json -follow https://example.com/
```

Keys starting with `-` are named options, and all other keys are positional
parameters. Positionals bind in argspec order. The order of options in the
argspec doesn't matter, and options and positionals may be interleaved in it.
The caller still has to supply all options before any positionals. A key may
appear only once.

## Per-argument settings

| Setting | Meaning |
|---|---|
| `-default v` | Value when not supplied. **Not** checked by `-validate`/`-enum` (intentionally may be out of range, e.g. `{}` as a "not given" sentinel). |
| `-required` | Error if not supplied. Combined with `-default` it can never fail, so it's effectively just optional. |
| `-boolean` | Options only. Always set: `1` if the flag is present, `0` if not. Takes no value word. (`-args 0` is the same thing.) |
| `-args n` | Consume *n* words and set a list of them. `-args all` consumes the rest, so the remaining words aren't scanned for options. |
| `-name v` | Variable/dict key to use instead of the key (minus `-`). |
| `-enum list` | Value must be one of these. **Exact match, no prefixes.** Error: `bad -s "x": must be a or b`. |
| `-validate cmdprefix` | `{*}cmdprefix $value` must return a true boolean (or `""`), and must not throw. |
| `-multi` | Options only: mutually exclusive flag group (see below). |
| `-all` | Repeated option collects a list of every value instead of last-wins. |
| `-alias` | Value is a variable name in the caller's caller, which gets bound by `upvar 1`. |
| `-end` | After this option (and its value), stop option scanning; the rest is positional. |
| `-# text` | Comment, ignored. |

Unknown settings, `-boolean`/`-multi`/`-args 0` on a positional, and a
spec with an odd number of words are errors **when the spec is first
used**, not when the proc is defined. So exercise every proc at least once in
a test.

## Unset vs set: what you get back

- If an argument is not supplied and has no `-default`, its variable is
  **left unset**, and in dict mode the key is absent. Test with
  `info exists`/`dict exists`. `-boolean` arguments are always set.
- A positional named `args` has an implicit `-default {}`, so it is always
  set, as for a proc. A positional using `-args all` under another name is left
  **unset** when it gets no words, unless you give it `-default {}`.
- parse_args only *sets* variables, it never unsets them. If a local
  already exists, for example because the proc set it before calling
  parse_args, or because parse_args runs repeatedly in a loop or coroutine,
  then an omitted argument without a default keeps its **old value**. Use
  `-default`, or `unset -nocomplain` first, when re-parsing in the same frame.
- If two options share a `-name` without `-multi`, the later one silently wins.
  Don't do that.

## Where option scanning stops (the subtle part)

Options are recognised by scanning from the first word. The scan stops at the
first of these:

1. A word that doesn't start with `-`. This includes `""`, and that word is
   the first positional.
2. `--`. It is consumed. Only the first one is special; a later `--` is an
   ordinary value.
3. The end of the words consumed by an option marked `-end`.
4. **The reserved tail**: the last *n* words, where *n* is the total number of
   words needed by `-required` positionals that have no `-default`. These are
   never treated as options, even when they look like one.

Inside the scan, any word starting with `-` must be an exact option name.
Otherwise it's an error: `bad option "-foo": must be -a, -b, or -c`, with
errorcode `TCL LOOKUP INDEX option -foo`. **Prefix abbreviations are not
accepted** (`-dir` is not `-directory`).

A value-taking option consumes the next word whatever it looks like:
`-x -y` sets x to `-y`, and `-x --` sets x to `--`.

**If the argspec has no options at all, there is no scan.** Every word is
positional, including `-foo` and `--`:

```tcl
parse_args {-foo} {name {-required}}    ;# name = -foo
parse_args {--}   {name {}}             ;# name = --
parse_args {-- -foo} {name {-required}} ;# error: wrong # args (-- is a value, not a separator)
```

The reserved tail, with argspec `{-x {} name {-required}}`:

```tcl
parse_args {-foo}       $spec   ;# name = -foo (the reserved tail word is never an option)
parse_args {-x 1 -foo}  $spec   ;# x = 1, name = -foo
parse_args {-foo bar}   $spec   ;# error: bad option "-foo" (-foo is in the scan, bar is the tail)
parse_args {-- -foo}    $spec   ;# name = -foo
```

**Practical rule for callers:** when a positional value might start with `-`
(user data, file names, negative numbers) and the command has options, pass
`--` before it: `mycmd -opt v -- $value`. For a command with exactly one trailing
`-required` positional this isn't needed, because the tail is reserved anyway.
Writing `--` is still harmless and clearer.

For the proc author, a spec with only `-required` trailing positionals
(the `regexp`/`lsort` shape) is robust to leading-dash values. Optional
positionals and `args` are not reserved, so their values are exposed to the
scan.

## Positional binding

Positionals are processed left to right, with lookahead for the `-required`
positionals still to come:

- **`-required` without a default** consumes 1 word, or *n* for `-args n`.
  If too few words are left, the error is `argument b is required`
  (`PARSE_ARGS REQUIRED b`), not `wrong # args`.
- **Optional** (including `-required -default`) consumes its words only if
  enough are left over for all later required positionals. Otherwise it gets
  its default, or stays unset. Optional positionals therefore get **skipped**,
  rather than greedily filled and then failing:
  ```tcl
  parse_args {1 2}   {a {} b {-required} c {-required}}   ;# b=1 c=2, a unset
  parse_args {1 2 3} {a {} b {-required} c {-required}}   ;# a=1 b=2 c=3
  ```
- **`args` / `-args all`** takes everything except what later required
  positionals need, so `{a {} args {} z {-required}}` with `1 2 3 4` gives
  a=1, args={2 3}, z=4.
- There can be at most one `args`/`-args all` positional, and every positional
  after it must be `-required` with no `-default`. Any other layout is a spec
  error.
- Leftover words give `wrong # args: should be "?a? ?b?"` (`TCL WRONGARGS`).
- Options after the first positional are *not* options. They are bound
  as positional values, or are an arity error:
  `parse_args {x -a 1} {-a {} p {} args {}}` gives p=x, args={-a 1}.

## Mutually exclusive flags: -multi

Options sharing a `-name` and marked `-multi` form a group. The variable holds
the **name of the flag given, without its `-`**:

```tcl
parse_args $args {
    -ascii      {-name compare_as -multi -default ascii}
    -dictionary {-name compare_as -multi}
    -integer    {-name compare_as -multi}
    -increasing {-name order -multi -default increasing}
    -decreasing {-name order -multi}
    list        {-required}
}
# lsort-like: [mysort -integer -decreasing $l] gives compare_as=integer, order=decreasing
```

- `-default` or `-required` on **any** member applies to the group. A
  required group that gets no member fails with
  `one of -ascii -dictionary -integer are required`
  (`PARSE_ARGS REQUIRED_ONE_OF {...}`).
- If several members are given, the last one wins, as for lsort. With `-all`
  on any member, the variable is the list of all members given, in order. Use
  that to enforce strict exclusivity yourself:
  `if {[llength $matchtype] > 1} {error "only one of -exact, -glob, -regexp"}`.
- `-multi` can't be combined with `-enum`, and is only valid on options. Don't
  combine it with `-args`. Mutual exclusion is about *which* flag was given;
  if a member needs a value, use separate options and check in code.
- In 0.7.1 and earlier, a group with `-all` ignored its `-default`.

## Validators

`-validate` evaluates `{*}$cmdprefix $value` **in the proc's own frame**, so
`upvar 1`/`uplevel 1` in the validator reach the proc's locals, and
unqualified command names resolve in the proc's namespace. The value passes
if the result is a true boolean or the empty string. A false result, a
non-boolean result or an error rejects it, with
`Validation failed for "-n": <result or error message>`
(`PARSE_ARGS VALIDATION -n`). The validator's own error code is not kept.

- For `-args n`, `-args all` and `args`, the validator gets the **whole list**
  once.
- With `-all`, it is called once **per occurrence**.
- It is never called for a `-default`.
- `-enum` is checked before `-validate`.
- In 0.7.1 and earlier, a `-validate` on an `-args n`, `-args all` or `args`
  argument could crash the interpreter (use-after-free) unless the validator
  kept a reference to the value. If you must support old versions, avoid
  validators on multi-word arguments.

A false result gives an unhelpful message, `Validation failed for "-port": 0`.
For user-facing commands, make the validator **throw** a descriptive error
instead; its message is used as the reason:

```tcl
-start {-default 0 -validate {apply {v {
    if {!([string is integer -strict $v] && $v >= 0)} {error "must be a non-negative integer"}
}}}}
# -start -1  gives  Validation failed for "-start": must be a non-negative integer
```

Short idioms:

```tcl
-port   {-validate {string is integer -strict}}
-count  {-validate {apply {v {expr {[string is integer -strict $v] && $v > 0}}}}}
-file   {-validate {file readable}}
```

## Other patterns

**Collect repeats:** `-header {-args 2 -all}` with `-header a 1 -header b 2`
gives header=`{{a 1} {b 2}}`.

**Variable-name arguments** (like `-textvariable`): `-var {-alias}` makes
`$var` an `upvar 1` link to the named variable in the proc's caller, and
`-alias` works on positionals too. **Not supported in dict mode.** There,
supplying the argument raises `-alias is not yet supported in dictionary mode`
(errorcode `NONE`). Leaving it out is not an error.

**Wrapper passthrough with -end:** let the wrapper take its own options, then
pass everything after a marker option through untouched:

```tcl
proc wrap args {
    parse_args $args {
        -verbose {-boolean}
        --args   {-boolean -end -name passthru}
        rest     {-args all -default {}}
    }
    inner {*}$rest
}
wrap -verbose --args -x 1 foo   ;# verbose=1, rest={-x 1 foo}
```

`-args all` on an *option* has the same effect: `-cmd {-args all}`.

**Dict mode** (third argument) is useful for forwarding or inspecting the
parsed set:
`parse_args $args $spec opts`, then `dict exists $opts timeout`.
`-boolean` keys are always present. No variables are created.

**Mixed signature:** `proc entry {widget args} { parse_args $args {...} }` is
fine. Fixed leading parameters stay ordinary proc parameters.

**Scripts:** `parse_args $argv {...}` at global level sets globals.

## Errors

| errorcode | When | message |
|---|---|---|
| `TCL LOOKUP INDEX option -x` | unknown option in the scan | `bad option "-x": must be -a` / `-a or -b` / `-a, -b, or -c` |
| `TCL LOOKUP INDEX -opt val` | `-enum` mismatch | `bad -opt "val": must be ...` |
| `TCL WRONGARGS` | too many words, or an option missing its value | `wrong # args: should be "?-x x? a ?b?"` |
| `PARSE_ARGS REQUIRED -x` / `PARSE_ARGS REQUIRED b` | required option / positional missing | `option -x is required` / `argument b is required` |
| `PARSE_ARGS REQUIRED_ONE_OF {...}` | required `-multi` group missing | |
| `PARSE_ARGS WRONGARGS name` | required positional with `-args n>1` given some but too few words | `Expecting 2 arguments for name` |
| `PARSE_ARGS VALIDATION name` | `-validate` rejected | |
| `NONE` | bad argspec (on first use) | `Cannot use -boolean on positional argument "a"` etc. |

The `wrong # args` usage string has **no command name**, because parse_args can't know it. Add the name if you
want one:

```tcl
try {
    parse_args $args $spec
} trap {TCL WRONGARGS} {msg} {
    return -code error -errorcode {TCL WRONGARGS} \
        [string map [list "should be \"" "should be \"[lindex [info level 0] 0] "] $msg]
}
```

## Performance

- Write the argspec as a **literal** in the proc body. The compiled form is
  cached on the argspec object, and a literal keeps it for the life of the
  proc. Building the spec per call (`[list ...]`, `[subst ...]`, `"..."` with
  substitutions) recompiles it on every call. If it has to be computed, compute
  it once into a namespace variable and pass that.
- Words in the reserved tail, and all words when the spec has no options,
  are never converted to strings. This makes large binary (bytearray) values
  safe to pass positionally without shimmering.

## C API

C extensions and jitc cdefs can bind with the same semantics via
`ParseArgs_BindDict`, `ParseArgs_Bind`, `ParseArgs_BindSlots` and the
`parseargs_bind.def` X-macro, which gives you a typed struct of the args. See
[c-api.md](c-api.md).
