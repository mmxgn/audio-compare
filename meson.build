project('audio-compare', 'c', version: '0.1.0', default_options: ['c_std=c11'])

deps = [
  dependency('gtk4'),
  dependency('libadwaita-1'),
  dependency('gstreamer-1.0'),
  dependency('gstreamer-app-1.0'),
]

executable('audio-compare', [
  'src/main.c',
  'src/track.c',
  'src/waveform.c',
], dependencies: deps, install: true)
