#!/usr/bin/env cfkit8.6
# vim: ft=tcl foldmethod=marker foldmarker=<<<,>>> ts=4 shiftwidth=4

set here	[file dirname [file normalize [info script]]]
set parent	[file dirname $here]
tcl::tm::path add $here
lappend auto_path $parent

package require bench

try {
	bench::run_benchmarks $here {*}$argv
} trap {BENCH INVALID_ARG} {errmsg options} {
	puts stderr $errmsg
	exit 1
}

# vim: ft=tcl foldmethod=marker foldmarker=<<<,>>> ts=4 shiftwidth=4
