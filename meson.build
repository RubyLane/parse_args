project('parse_args', 'c',
  version:          '0.6.2',
  default_options:  ['c_std=c11', 'warning_level=3', 'werror=true'],
  meson_version:    '>=1.3.0',
)

cc = meson.get_compiler('c')
fs = import('fs')
add_project_arguments('-D_DEFAULT_SOURCE', language: 'c')

if get_option('debug')
  add_project_arguments(cc.get_supported_arguments(['-ggdb3']), language: 'c')
endif

add_project_arguments('-Wno-missing-field-initializers', language: 'c')

conf = configuration_data()

tea_want_shims = ['tip445']

add_project_arguments('-DHAVE_CONFIG_H', language: 'c')

deps = []
inc = [include_directories('generic')]

pkg_sources = files(
  'generic/parse_args.c',
)

subdir('teabase')

# pkg-config metadata so downstream meson / shell consumers can locate
# this install uniformly via `pkg-config --variable=tcl_pkg_path parse_args`
# or `dependency('parse_args').get_variable('tcl_pkg_path')`.
# tcl_dep is set by teabase/init.
pc_conf = configuration_data()
pc_conf.set('PACKAGE_NAME',     meson.project_name())
pc_conf.set('PACKAGE_VERSION',  meson.project_version())
pc_conf.set('PKG_DIR_BASENAME', meson.project_name() + '-' + meson.project_version())
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
  variables: {
    'tcl_pkg_path': fs.parent(pkg_so.full_path()),
    'version':      meson.project_version(),
  },
)
meson.override_dependency(meson.project_name(), parse_args_dep)

subdir('doc')
