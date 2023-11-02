project('mtest','c')

add_global_arguments('-O2', language : 'c')

src_files = ['main.c', 'topology.c', 'workq.c','cli.c']

thread_dep = dependency('threads')

executable('mtest', sources : src_files ,dependencies : thread_dep)
