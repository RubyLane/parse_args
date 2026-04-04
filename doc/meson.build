pandoc = find_program('pandoc', native: true, required: false)

if pandoc.found()
  doc_md = configure_file(
    input:          'parse_args.md.in',
    output:         '@BASENAME@',
    configuration:  teaconf,
  )

  doc_targets = []

  doc_targets += custom_target('htmldoc',
    input:    doc_md,
    output:   'parse_args.html',
    command:  [pandoc, '--standalone', '--metadata', 'title=parse_args',
      '--from', 'markdown', '--to', 'html',
      '@INPUT@', '--output', '@OUTPUT@'],
    install:          true,
    install_dir:      get_option('datadir') / 'doc',
    build_by_default: true,
  )
  doc_targets += custom_target('manpage',
    input:    doc_md,
    output:   'parse_args.n',
    command:  [pandoc, '--standalone', '--from', 'markdown', '--to', 'man',
      '@INPUT@', '--output', '@OUTPUT@'],
    install:          true,
    install_dir:      get_option('mandir'),
    build_by_default: true,
  )

  doc_targets += custom_target('readme',
    input:    doc_md,
    output:   'README.md',
    command:  [pandoc, '--from', 'markdown', '--to', 'gfm',
      '@INPUT@', '--output', '@OUTPUT@'],
    install_dir:      meson.project_source_root(),
    install_tag:      'source',
    build_by_default: false,
  )

  alias_target('doc', doc_targets)
endif
