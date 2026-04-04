project('parse_args', 'c',
  version:          '0.6',
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
subdir('doc')
