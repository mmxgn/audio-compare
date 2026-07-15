project('audio-compare', 'c', version: '0.1.0', default_options: ['c_std=c11'])

deps = [
  dependency('gtk4'),
  dependency('libadwaita-1'),
  dependency('gstreamer-1.0'),
  dependency('gstreamer-app-1.0'),
  dependency('gstreamer-controller-1.0'),
  dependency('libebur128'),
  meson.get_compiler('c').find_library('m'),
]

install_data('data/org.mmxgn.audiocompare.desktop',
  install_dir: join_paths(get_option('datadir'), 'applications'))

install_data('data/icons/hicolor/scalable/apps/org.mmxgn.audiocompare.svg',
  install_dir: join_paths(get_option('datadir'), 'icons', 'hicolor', 'scalable', 'apps'))

executable('audio-compare', [
  'src/main.c',
  'src/track.c',
  'src/player.c',
  'src/waveform.c',
], dependencies: deps, install: true)
