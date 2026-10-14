project('parse_args', 'c',
  version:          '0.7.1',
  # A single c_std, not a fallback list ('gnu2y,gnu23,gnu2x'): meson (1.9.1)
  # doesn't resolve the list when this is built as a subproject, and passes it
  # to the compiler verbatim, failing every configure check.  gnu2x is C23 on
  # current gcc / clang; defer support is detected separately (teabase).
  default_options:  ['c_std=gnu2x', 'warning_level=3', 'werror=true'],
  meson_version:    '>=1.3.0',
)

cc = meson.get_compiler('c')
fs = import('fs')
add_project_arguments('-D_DEFAULT_SOURCE',                language: 'c')
add_project_arguments('-Wno-missing-field-initializers',  language: 'c')
add_project_arguments('-DHAVE_CONFIG_H', '-DBUILD_parse_args', language: 'c')
# Only the stubs API (EXTERN / DLLEXPORT) and Parse_args_Init are exported: keep
# the internal helpers from interposing with other packages' symbols when loaded
# RTLD_GLOBAL (as jitc does for package dependencies)
add_project_arguments(cc.get_supported_arguments('-fvisibility=hidden'), language: 'c')

if get_option('debug')
  add_project_arguments(cc.get_supported_arguments(['-ggdb3']), language: 'c')
endif

conf  = configuration_data()
conf.set10('TESTMODE', get_option('testmode'))
deps  = []
inc   = [include_directories('.')]

pkg_sources   = files('parse_args.c', 'teabase/instance_tracking.c', 'teabase/register_config.c')
# parseargs_bind.def goes through teaconf_files (with nothing to substitute) so
# that it lands in the build dir next to parse_args.h (includedir,runtime, for
# jitc cdefs and meson devenv) and is installed with it
teaconf_files = files('parse_args.h.in', 'parseargs_bind.def')
tea_genstubs  = files('parse_args.decls', 'parse_argsStubLib.c')

tea_want_shims = ['tip445']
tea_want_defer = true

subdir('teabase')

# pkg-config metadata so downstream meson / shell consumers can locate
# this install uniformly via `pkg-config --variable=tcl_pkg_path parse_args`
# or `dependency('parse_args').get_variable('tcl_pkg_path')`.
# tcl_dep is set by teabase/init.
pc_conf = configuration_data()
pc_conf.set('PACKAGE_NAME',     meson.project_name())
pc_conf.set('PACKAGE_VERSION',  meson.project_version())
pc_conf.set('PKG_DIR_BASENAME', meson.project_name() + '-' + meson.project_version())
pc_conf.set('STUBLIB',          tea_pkg_stub_name)
configure_file(
  input:          meson.project_name() + '.pc.in',
  output:         meson.project_name() + '.pc',
  configuration:  pc_conf,
  install:        true,
  install_dir:    tcl_dep.get_variable('libdir') / 'pkgconfig',
  install_tag:    meson.is_subproject() ? 'deps' : 'runtime',
)

# Override-dependency for meson subproject consumers. Mirrors the
# tcl_pkg_path variable in the .pc file so downstream
# `dependency('parse_args', fallback: ['parse_args', 'parse_args_dep']).get_variable('tcl_pkg_path')`
# returns the right directory regardless of resolution path.
parse_args_dep = declare_dependency(
  link_with:            stubs_lib,
  sources:              genstubs[1],
  include_directories:  include_directories('teabase'),
  compile_args:         '-DUSE_PARSE_ARGS_STUBS=1',
  variables: {
    'tcl_pkg_path': fs.parent(pkg_so.full_path()),
    'version':      meson.project_version(),
  },
)
meson.override_dependency(meson.project_name(), parse_args_dep)

# Test-only extension consuming the stubs API the way a downstream meson
# extension would (tests/stubs.test)
_tclstub = tcl_dep.type_name() == 'internal' ? disabler() : cc.find_library('tclstub', dirs: [tcl_dep.get_variable('libdir')], required: false)
if _tclstub.found()
  shared_module('parse_argsstubsconsumer',
    'tests/stubs_consumer.c',
    dependencies: [tcl_hdr_dep, _tclstub, parse_args_dep],
    # parseargs_bind.def needs defer: use teabase's defer.h, configured as detected for this build
    c_args: [
      '-DHAVE_NATIVE_DEFER=@0@'.format(conf.get('HAVE_NATIVE_DEFER', 0)),
      '-DHAVE_DEFER_POLYFILL=@0@'.format(conf.get('HAVE_DEFER_POLYFILL', 0)),
    ],
    install:      false,
  )
endif

subdir('doc')
