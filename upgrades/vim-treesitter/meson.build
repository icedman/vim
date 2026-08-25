project('vim-treesitter', 'c', 'cpp',
  version : '0.1',
  default_options : ['warning_level=3', 'cpp_std=c++20']
)



add_global_arguments('-Wno-unused-parameter', language: 'c')
add_global_arguments('-fpermissive', language: 'c')
add_global_arguments('-fPIC', language: 'c')

tree_sitter_files = [
    'libs/tree-sitter/lib/src/lib.c',
]

tree_sitter_includes = [
    'libs/tree-sitter/lib/include'
]

tree_sitter_grammar_files = []

tree_sitter_grammar_files += 'libs/tree-sitter-grammars/tree-sitter-c/src/parser.c'
tree_sitter_grammar_files += 'libs/tree-sitter-grammars/tree-sitter-cpp/src/parser.c'
tree_sitter_grammar_files += 'libs/tree-sitter-grammars/tree-sitter-cpp/src/scanner.c'
tree_sitter_grammar_files += 'libs/tree-sitter-grammars/tree-sitter-c-sharp/src/parser.c'
tree_sitter_grammar_files += 'libs/tree-sitter-grammars/tree-sitter-c-sharp/src/scanner.c'
tree_sitter_grammar_files += 'libs/tree-sitter-grammars/tree-sitter-css/src/parser.c'
tree_sitter_grammar_files += 'libs/tree-sitter-grammars/tree-sitter-css/src/scanner.c'
tree_sitter_grammar_files += 'libs/tree-sitter-grammars/tree-sitter-html/src/parser.c'
tree_sitter_grammar_files += 'libs/tree-sitter-grammars/tree-sitter-html/src/scanner.c'
tree_sitter_grammar_files += 'libs/tree-sitter-grammars/tree-sitter-java/src/parser.c'
tree_sitter_grammar_files += 'libs/tree-sitter-grammars/tree-sitter-javascript/src/parser.c'
tree_sitter_grammar_files += 'libs/tree-sitter-grammars/tree-sitter-javascript/src/scanner.c'
tree_sitter_grammar_files += 'libs/tree-sitter-grammars/tree-sitter-json/src/parser.c'
tree_sitter_grammar_files += 'libs/tree-sitter-grammars/tree-sitter-python/src/parser.c'
tree_sitter_grammar_files += 'libs/tree-sitter-grammars/tree-sitter-python/src/scanner.c'

lua_dep = dependency('lua', required: false, disabler: true)
threads_dep = dependency('threads')

library('treesitter',
    'module.cpp',
    tree_sitter_files,
    tree_sitter_grammar_files,
    include_directories: [
        tree_sitter_includes
    ],
    dependencies: [
        lua_dep,
        threads_dep
    ],
    name_prefix: '',
    override_options: ['b_lundef=false'],
)