project(
    'i3sd',
    'c',
    version: '0.1.0',
    default_options: [
        'c_std=c23',
        'warning_level=3',
        'werror=true',
        'buildtype=debugoptimized',
    ],
)

cc = meson.get_compiler('c')
m = cc.find_library('m', required: true)
luajit = dependency('luajit')
yyjson = dependency('yyjson')
xxhash = dependency('libxxhash')
systemd = dependency('libsystemd')

conf = configuration_data()
conf.set_quoted('I3SD_VERSION', meson.project_version())
conf.set_quoted(
    'I3SD_LUA_DIR',
    get_option('prefix') / get_option('datadir') / 'i3sd/lua',
)
conf.set_quoted('I3SD_BUILD_EXECUTABLE', meson.current_build_dir() / 'i3sd')
conf.set_quoted('I3SD_SOURCE_LUA_DIR', meson.current_source_dir() / 'lua')
configure_file(output: 'config.h', configuration: conf)

inc = include_directories('include', '.')
core_sources = files(
    'src/buffer.c',
    'src/click.c',
    'src/output.c',
    'src/timer.c',
    'src/utf8.c',
)

core = static_library(
    'i3sd-core',
    core_sources,
    include_directories: inc,
    dependencies: [yyjson],
)

executable(
    'i3sd',
    'src/main.c',
    include_directories: inc,
    link_with: core,
    dependencies: [luajit, yyjson, xxhash, systemd, m],
    install: true,
)

test_core = executable(
    'test-core',
    'tests/test_core.c',
    include_directories: inc,
    link_with: core,
    dependencies: [yyjson],
)
test('core', test_core)
test(
    'config-check',
    find_program('tests/check_config.sh'),
    args: [
        meson.current_build_dir() / 'i3sd',
        meson.current_source_dir() / 'tests/config/basic.lua',
    ],
)
test(
    'example-configs',
    find_program('tests/check_examples.sh'),
    args: [meson.current_build_dir() / 'i3sd', meson.current_source_dir()],
)
test(
    'spawn-callback',
    find_program('tests/test_rofi.sh'),
    args: [
        meson.current_build_dir() / 'i3sd',
        meson.current_source_dir() / 'tests/config/rofi.lua',
        meson.current_source_dir() / 'tests/fake_rofi.sh',
    ],
)

install_data(
    'lua/i3sd/modules/clock.lua',
    'lua/i3sd/modules/cpu.lua',
    'lua/i3sd/modules/filesystem.lua',
    'lua/i3sd/modules/memory.lua',
    'lua/i3sd/modules/power_profiles.lua',
    'lua/i3sd/modules/systemd.lua',
    install_dir: get_option('datadir') / 'i3sd/lua/i3sd/modules',
)
