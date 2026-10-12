# A consumer of libbdf as a Meson subproject (subprojects/bdf.wrap): what the
# CI builds, with subprojects/bdf a link to the checkout it is in.
project('bdf-consumer', 'c', default_options: ['c_std=c11'])
bdf_dep = dependency('bdf')
consumer = executable('consumer', '../../consumer/main.c', dependencies: bdf_dep)
test('consumer', consumer)
