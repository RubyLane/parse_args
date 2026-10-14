package require tcltest
namespace import ::tcltest::*

::tcltest::configure -singleproc 1 {*}$argv -testdir [file dirname [info script]]

set failed [::tcltest::runAllTests]

::tcltest::cleanupTests 0

if {$failed} {
	puts $::tcltest::outputChannel "[file tail [info script]]: $failed test(s) failed"
	close stderr
	error "test run failed"
}
