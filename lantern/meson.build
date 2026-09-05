project(
    'ir',
    'c',
    version: '0.1.0',
    license: 'MIT',
    meson_version: '>=1.1.0',
    default_options: [
        'c_std=c2x',
        'warning_level=2',
        'default_library=static',
    ],
)

cc = meson.get_compiler('c')

subdir('th')

tdesc_inc = include_directories('tdesc')
ir_inc    = include_directories('include')

subdir('fe')
subdir('src')
subdir('test')
