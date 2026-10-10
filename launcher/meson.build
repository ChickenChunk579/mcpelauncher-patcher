project('mcpe-launcher', 'c',
  version: '0.1.0',
  meson_version: '>= 0.63.0',
  default_options: [
    'warning_level=2',
    'c_std=c11',
    'buildtype=release',
  ],
)

gnome = import('gnome')

deps = [
  dependency('libadwaita-1'),
  dependency('zlib'),
]

app_id = 'dev.rhys.mcpe-launcher'

gresources = gnome.compile_resources('mcpe-launcher-resources',
  'data/mcpe-launcher.gresource.xml',
  source_dir: 'data',
  c_name: 'mcpe_launcher')

src = files(
  'src/main.c',
  'src/zip.c',
  'src/axml.c',
  'src/apk.c',
  'src/versions.c',
  'src/launch.c',
  'src/window.c',
)

executable('mcpe-launcher',
  src,
  gresources,
  dependencies: deps,
  install: true,
)

install_data('data/mcpe-launcher.desktop',
  install_dir: get_option('datadir') / 'applications',
)
install_data('data/mcpe-launcher.metainfo.xml',
  install_dir: get_option('datadir') / 'metainfo',
)
install_data('data/icons/hicolor/scalable/apps/@0@.svg'.format(app_id),
  install_dir: get_option('datadir') / 'icons' / 'hicolor' / 'scalable' / 'apps',
)

summary({
  'prefix': get_option('prefix'),
  'binary': get_option('prefix') / 'bin' / 'mcpe-launcher',
}, section: 'Install location')