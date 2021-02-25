project('umockdev', 'c', 'vala',
  version: run_command('sed', '-rn', '1 { s/^.*\[([0-9.]+)\].*/\\1/; p }', 'NEWS').stdout().strip(),
  license: 'LGPLv2.1+')

srcdir = meson.current_source_dir() / 'src'
testsdir = meson.current_source_dir() / 'tests'
lib_version = '0.3.0'

add_project_arguments(
  '-Werror',
  '-Werror=missing-prototypes',
  '-Werror=strict-prototypes',
  '-Werror=nested-externs',
  '-Werror=pointer-arith',
  '-Werror=implicit-function-declaration',
  '-Werror=pointer-arith',
  '-Werror=init-self',
  '-Werror=format-security',
  '-Werror=format=2',
  #'-Werror=missing-include-dirs',
  '-Werror=unused-variable',
  '-Werror=return-type',
  '-Werror=uninitialized',
  language: 'c')

# let's not clutter the code with too many #ifdefs
if get_option('b_ndebug') == 'true'
  add_project_arguments('-Wno-error=unused-but-set-variable', language: 'c')
endif

conf = configuration_data()
cc = meson.get_compiler('c')
valac = meson.get_compiler('vala')
g_ir_compiler = find_program('g-ir-compiler', required: false)

conf.set('PACKAGE_NAME', meson.project_name())
conf.set_quoted('VERSION', meson.project_version())

#
# dependencies
#

dl = cc.find_library('dl')

glib = dependency('glib-2.0', version: '>= 2.32.0')
gobject = dependency('gobject-2.0', version: '>= 2.32.0')
gio = dependency('gio-2.0', version: '>= 2.32.0')
gio_unix = dependency('gio-unix-2.0', version: '>= 2.32.0')
libudev = dependency('libudev')
gudev = dependency('gudev-1.0', required: false)
python = find_program('python3', 'python', required: false)

vapi_posix = valac.find_library('posix')
vapi_linux = valac.find_library('linux')
vapi_linux_fixes = valac.find_library('linux_fixes', dirs: srcdir)
vala_libudev = cc.find_library('udev')
vala_libutil = cc.find_library('util')

# local VAPIs
vapi_config = valac.find_library('config', dirs: srcdir)
vapi_ioctl = valac.find_library('ioctl', dirs: srcdir)
vapi_assertions = valac.find_library('assertions', dirs: testsdir)

#
# system API checks
#

# glibc's ioctl takes an 'unsigned long' instead of the POSIX 'int' for the request parameter
if cc.compiles('''
        #include <sys/ioctl.h>
        extern int ioctl (int, int, ...);
        void main(void) {}
''', name: 'ioctl request parameter type is int')
  conf.set('IOCTL_REQUEST_TYPE', 'int')
else
  conf.set('IOCTL_REQUEST_TYPE', 'unsigned long')
endif

#
# preload library
#

preload_lib = shared_library('umockdev-preload',
  ['src/libumockdev-preload.c',
   'src/debug.c',
   'src/ioctl_tree.c'],
  c_args: ['-fvisibility=default'],
  version: '0.0.0',
  dependencies: [dl],
  install: true)

#
# umockdev client library
#

umockdev_utils_lib = static_library('umockdev-utils',
  'src/umockdev-utils.vala',
  vala_vapi: 'umockdev-utils.vapi',
  dependencies: [glib, vapi_posix],
  include_directories: include_directories('src'))

umockdev_lib = shared_library('umockdev',
  ['src/umockdev.vala',
   'src/umockdev-ioctl.vala',
   'src/uevent_sender.vapi',
   'src/uevent_sender.c',
   'src/ioctl_tree.vapi',
   'src/ioctl_tree.c',
   'src/debug.c'],
  vala_vapi: 'umockdev-1.0.vapi',
  vala_gir: 'UMockdev-1.0.gir',
  dependencies: [glib, gobject, gio, gio_unix, vapi_posix, vapi_linux, vapi_linux_fixes, vala_libudev, vala_libutil, vapi_ioctl],
  link_with: [umockdev_utils_lib],
  link_depends: ['src/umockdev.map'],
  link_args: [
    '-Wl,-export-dynamic',
    '-Wl,--no-undefined',
    '-Wl,--version-script,@0@/umockdev.map'.format(srcdir),
  ],
  include_directories: include_directories('src'),
  version: lib_version,
  install: true,
  install_dir: [true, 'include/umockdev-1.0', true, true])

# no way to insert "Rename to:" into annotations, so hack it
hacked_gir = custom_target('UMockdev-1.0 hacked gir',
  command: ['sed', '/name="add_devicev"/ s/icev">$/icev" shadows="add_device">/', meson.current_build_dir() / 'UMockdev-1.0.gir'],
  capture: true,
  depends: umockdev_lib,
  output: 'UMockdev-1.0-hacked.gir')

if g_ir_compiler.found()
umockdev_typelib = custom_target('UMockdev-1.0 typelib',
  command: [g_ir_compiler, '--output', '@OUTPUT@', '-l', 'libumockdev.so.0', '@INPUT@'],
  input: hacked_gir,
  output: 'UMockdev-1.0.typelib',
  install: true,
  install_dir: get_option('libdir') / 'girepository-1.0')
endif

pkgconfig = import('pkgconfig')
pkgconfig.generate(umockdev_lib,
  filebase: 'umockdev-1.0',
  requires: ['glib-2.0', 'gobject-2.0'],
  subdirs: ['umockdev-1.0'],
  description: 'Mock hardware devices for creating unit tests')

#
# programs
#

install_data('src/umockdev-wrapper', install_dir: get_option('bindir'))

umockdev_run_exe = executable('umockdev-run',
  'src/umockdev-run.vala',
  dependencies: [glib, gobject, gio, vapi_posix, vapi_config],
  link_with: [umockdev_lib, umockdev_utils_lib],
  install: true)

umockdev_record_exe = executable('umockdev-record',
  ['src/umockdev-record.vala',
   'src/umockdev-ioctl.vala',
   'src/ioctl_tree.vapi',
   'src/ioctl_tree.c',
   'src/debug.c'],
  dependencies: [glib, gobject, gio_unix, vapi_posix, vapi_config, vapi_ioctl],
  link_with: [umockdev_utils_lib],
  include_directories: include_directories('src'),
  install: true)

#
# tests
#

test_env = environment()
test_env.set('G_DEBUG', 'fatal-warnings,fatal-criticals,gc-friendly')
test_env.set('MALLOC_CHECK_', '3')
test_env.set('TOP_SRCDIR', meson.current_source_dir())
test_env.prepend('LD_LIBRARY_PATH', meson.current_build_dir())
test_env.prepend('PATH', meson.current_build_dir())
test_env.prepend('GI_TYPELIB_PATH', meson.current_build_dir())

installed_env = environment()
test_env.set('TOP_SRCDIR', meson.current_source_dir())

add_test_setup('default',
  exe_wrapper: srcdir / 'umockdev-wrapper',
  # podman often hangs for a while in overlayfs
  timeout_multiplier: 5,
  env: test_env,
  is_default: true)

add_test_setup('valgrind',
  exe_wrapper: [srcdir / 'umockdev-wrapper', 'valgrind', '-q', '--leak-check=full', '--show-possibly-lost=no',
    '--errors-for-leak-kinds=definite', '--error-exitcode=33',
    '--trace-children=yes', '--trace-children-skip=/bin*,/usr/bin/*,/usr/local/bin/*'],
  timeout_multiplier: 10,
  env: test_env)

add_test_setup('installed',
  exe_wrapper: 'umockdev-wrapper',
  # podman often hangs for a while in overlayfs
  timeout_multiplier: 5,
  env: test_env)

test_chatter_exe = executable('chatter', 'tests/chatter.c')
test_chatter_stream_exe = executable('chatter-socket-stream', 'tests/chatter-socket-stream.c')
test_readbyte_exe = executable('readbyte', 'tests/readbyte.c')

if gudev.found()
  test('umockdev', executable('test-umockdev',
    'tests/test-umockdev.c',
    dependencies: [glib, libudev, gudev],
    link_with: [umockdev_lib]),
  depends: [preload_lib])

  test('umockdev-vala', executable('test-umockdev-vala',
      'tests/test-umockdev-vala.vala',
      dependencies: [glib, gobject, gio, gudev, vapi_posix, vapi_assertions, vapi_ioctl],
      link_with: [umockdev_lib]),
    depends: [preload_lib],
    suite: 'fails-valgrind')
endif

test('ioctl-tree', executable('test-ioctl-tree',
  ['tests/test-ioctl-tree.c',
   'src/ioctl_tree.c',
   'src/debug.c'],
  include_directories: include_directories('src'),
  dependencies: [glib]))

test('umockdev-run', executable('test-umockdev-run',
    'tests/test-umockdev-run.vala',
    dependencies: [glib, gobject, gio, vapi_posix, vapi_assertions, vapi_config],
    link_with: [umockdev_lib]),
  depends: [umockdev_run_exe, preload_lib, test_chatter_exe, test_chatter_stream_exe])

test('umockdev-record', executable('test-umockdev-record',
    'tests/test-umockdev-record.vala',
    dependencies: [glib, gobject, gio, gio_unix, vapi_posix, vapi_linux, vapi_assertions, vapi_config, vala_libutil],
    link_with: [umockdev_lib]),
  depends: [umockdev_record_exe, preload_lib, test_readbyte_exe, test_chatter_exe, test_chatter_stream_exe],
  suite: 'fails-valgrind')

test('static-code', files('tests/test-static-code'))

if python.found()
if g_ir_compiler.found()
  test('umockdev.py', python,
    args: ['-Wd', '-Werror::PendingDeprecationWarning', '-Werror::DeprecationWarning',
           testsdir / 'test-umockdev.py'],
    depends: [preload_lib, umockdev_typelib],
    suite: 'fails-valgrind')
endif
endif

#
# outputs
#

configure_file(output: 'config.h', configuration: conf)

if get_option('gtk_doc')
  subdir('docs/reference')
endif
