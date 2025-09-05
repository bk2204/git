libgit_rs = static_library('git_rs',
  sources: [
    'lib.rs',
    'varint.rs',
  ],
  rust_crate_type: 'staticlib',
)
libgit_libraries += libgit_rs

# The 'rust' module was only introduced in Meson 1.0. Furthermore, the module
# does not seem to work on macOS as expected right now. As such, we only
# conditionally enable tests.
if meson.version().version_compare('>=1.0.0') and host_machine.system() != 'darwin'
  rustmod = import('rust')
  rustmod.test('rust', libgit_rs)
endif
