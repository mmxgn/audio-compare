project('audio-compare', 'c', version: '0.4.0', default_options: ['c_std=c11'])

add_project_arguments('-DAPP_VERSION="@0@"'.format(meson.project_version()), language: 'c')

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

# Logic checks for the UI/state fixes; includes the sources, stubs the player.
test('ui-state', executable('check', 'test/check.c', dependencies: deps, install: false))

# Real player.c: tearing down a failed branch must not hang the main loop.
# Needs an audio sink to preroll against; exits 77 (skipped) without one.
test('teardown', executable('teardown', 'test/teardown.c', 'src/player.c',
  include_directories: include_directories('src'), dependencies: deps, install: false),
  is_parallel: false, timeout: 60)

# The real analysis pipeline from track.c, without a window or display.
test('analysis', executable('analysis', 'test/analysis.c', 'src/track.c',
  include_directories: include_directories('src'), dependencies: deps, install: false))
