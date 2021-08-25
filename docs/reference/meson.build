version_xml = configure_file(
  input: 'version.xml.in',
  output: 'version.xml',
  configuration: conf)

# HACK: need to find valac'ed umockdev.c for gtk-doc; https://github.com/mesonbuild/meson/issues/3892
# this isn't predictable between meson versions
umockdev_c = run_command('sh', '-ec', 'find -name umockdev.c| xargs dirname').stdout().strip()
umockdev_ioctl_c = run_command('sh', '-ec', 'find -name umockdev-ioctl.c| xargs dirname').stdout().strip()

gnome = import('gnome')
gnome.gtkdoc('umockdev',
  main_xml: 'umockdev-docs.xml',
  src_dir: [meson.build_root(), umockdev_c, umockdev_ioctl_c],
  content_files: [version_xml],
  ignore_headers: ['uevent_sender.h', 'ioctl_tree.h', 'debug.h'],
  dependencies: [glib, gobject, declare_dependency(link_with : [umockdev_lib])],
  install: true,
)
