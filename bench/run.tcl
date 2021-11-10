#!/usr/bin/env tclsh
# vim: ft=tcl foldmethod=marker foldmarker=<<<,>>> ts=4 shiftwidth=4

set big	[string repeat a [expr {int(1e8)}]]	;# Allocate 100MB to pre-expand the zippy pool
unset big

set here	[file dirname [file normalize [info script]]]
tcl::tm::path add $here

package require bench

try {
	bench::run_benchmarks $here {*}$argv
} trap {BENCH INVALID_ARG} {errmsg options} {
	puts stderr $errmsg
	exit 1
}

# vim: ft=tcl foldmethod=marker foldmarker=<<<,>>> ts=4 shiftwidth=4
