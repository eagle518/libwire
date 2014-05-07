#!/usr/bin/python

lib_srcs = [
        'wire', 'wire_fd', 'wire_pool', 'wire_stack', 'wire_io', 'http_parser', 'wire_channel', 'wire_wait', 'wire_lock', 'coro'
]

test_srcs = {
        'base': ('base',),
        'echo_server': ('echo_server', 'utils'),
        'recurser': ('recurser',),
        'web': ('web', 'utils'),
        'channel': ('channel',),
        'waiters': ('waiters',),
        'bench': ('bench',),
        'locks': ('locks',),
        'asyncio': ('asyncio',),
}

cflags = ['-Iinclude', '-g', '-O0', '-Wall', '-Werror', '-Wextra', '-Wshadow',
          '-Wmissing-prototypes', '-Winit-self', '-pipe', '-DCORO_STACKALLOC=0', '-D_GNU_SOURCE']
ldflags = ['-lrt', '-lpthread']

import os, os.path
import ninja_syntax

if os.path.exists('/usr/include/valgrind/valgrind.h'):
        cflags += ['-DUSE_VALGRIND']

n = ninja_syntax.Writer(file('build.ninja', 'w'))
n.comment('Auto generated by ./configure.py, edit the configure.py script instead')
n.newline()

env_keys = set(['CC', 'AR', 'CFLAGS', 'LDFLAGS'])
configure_env = dict((k, os.environ[k]) for k in os.environ if k in env_keys)
if configure_env:
    config_str = ' '.join([k+'='+configure_env[k] for k in configure_env])
    n.variable('configure_env', config_str+'$ ')
n.newline()

CC = configure_env.get('CC', 'gcc')
n.variable('cc', CC)
AR = configure_env.get('AR', 'ar')
n.variable('ar', AR)
n.newline()

def shell_escape(str):
        """Escape str such that it's interpreted as a single argument by
           the shell."""

        # This isn't complete, but it's just enough to make NINJA_PYTHON work.
        if '"' in str:
                return "'%s'" % str.replace("'", "\\'")
        return str

if 'CFLAGS' in configure_env:
    cflags.append(configure_env['CFLAGS'])
n.variable('cflags', ' '.join(shell_escape(flag) for flag in cflags))

if 'LDFLAGS' in configure_env:
    ldflags.append(configure_env['LDFLAGS'])
n.variable('ldflags', ' '.join(shell_escape(flag) for flag in ldflags))

n.newline()

n.rule('c',
        command='$cc -MMD -MT $out -MF $out.d $cflags -c $in -o $out',
        depfile='$out.d',
        deps='gcc',
        description='CC $out'
)
n.newline()

n.rule('ar',
        command='rm -f $out && $ar crs $out $in',
        description='AR $out',
)
n.newline()

n.rule('link',
        command='$cc -o $out $in $libs $ldflags',
        description='LINK $out'
)
n.newline()

def src(filename):
        return os.path.join('src', filename)
def btest(filename):
        return os.path.join('test', filename)
def built(filename):
        return os.path.join('built', filename)
def cc(filename, src, **kwargs):
        return n.build(built(src(filename)) + '.o', 'c', src(filename) + '.c', **kwargs)

all_targets = []

lib_objs = []
for source in lib_srcs:
        lib_objs += cc(source, src)
lib = n.build('libwire.a', 'ar', lib_objs)
all_targets += lib
n.newline()

test_exec = []
test_objs = {}
for test in test_srcs.keys():
        objs = []
        for source in test_srcs[test]:
                if source in test_objs.keys():
                        objs += test_objs[source]
                else:
                        obj = cc(source, btest)
                        test_objs[source] = obj
                        objs += obj
        test_exec += n.build(test, 'link', objs, implicit=lib, variables=[('libs', lib)])
        all_targets += test_exec
n.newline()

n.rule('configure',
        command='${configure_env} ./configure.py',
        description='CONFIGURE build.ninja',
        generator=True
        )
all_targets += n.build('build.ninja', 'configure', implicit='./configure.py')
n.newline()

n.rule('tags',
        command='ctags $in',
        description='CTAGS $out'
        )
all_targets += n.build('tags', 'tags', [src(name) + '.c' for name in lib_srcs] + [os.path.join('include', name) for name in os.listdir('include')])
n.newline()

n.comment('Create doxygen docs, requires explicit build: ninja doxygen')
n.rule('doxygen',
        command='doxygen $in',
        description='DOXYGEN $in')
n.build('doxygen', 'doxygen', 'doxygen.config')
n.newline()

n.build('all', 'phony', all_targets)
n.default('all')
