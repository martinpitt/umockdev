version_xml = configure_file(
  input: 'version.xml.in',
  output: 'version.xml',
  configuration: conf)

gnome = import('gnome')
gnome.gtkdoc('umockdev',
  main_xml: 'umockdev-docs.xml',
  src_dir: [meson.build_root()],
  content_files: [version_xml],
  gobject_typesfile: 'umockdev.types',
  scan_args: ['--ignore-decorators', 'VALA_EXTERN'],
  ignore_headers: ['uevent_sender.h', 'cros_ec.h', 'ioctl_tree.h', 'debug.h'],
  dependencies: [glib, gobject, declare_dependency(link_with : [umockdev_lib])],
  install: true,
)
