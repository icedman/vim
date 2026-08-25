# vim-treesitter
A treesitter-based syntax highlighter for vim.

The plugin acquires Vim's immutable `buffer_snapshot` on the editor thread and
parses the complete snapshot on a persistent worker thread.  New parse requests
for a buffer replace queued older requests, and a stale parse result is never
published over a newer revision.

# install

```sh
make
```

Tree-sitter and the bundled grammars are vendored under `libs/`; the build does
not download dependencies.  The Vim executable must export its
internal `buf_snapshot_*` symbols (for example, link Vim with
`LDFLAGS=-Wl,--export-dynamic` on ELF systems), since the Lua module consumes
the snapshot API directly.

*.vimrc*

```sh
luafile ~/.vim/lua/vim-treesitter/vim-treesitter.lua
```

# built-in parsers

```cpp
std::map<std::string, std::function<const TSLanguage *()>> ts_languages = {
    {".c", tree_sitter_c},
    {".cpp", tree_sitter_cpp},
    {".hpp", tree_sitter_cpp},
    {".h", tree_sitter_cpp},
    {".cs", tree_sitter_c_sharp},
    {".css", tree_sitter_css},
    {".html", tree_sitter_html},
    {".xml", tree_sitter_html},
    {".java", tree_sitter_java},
    {".js", tree_sitter_javascript},
    {".jsx", tree_sitter_javascript},
    {".json", tree_sitter_json},
    {".py", tree_sitter_python},
};
```

# commands

* TSDebugNodes ~ show node type at cursor
* TSOuterBlock ~ move cursor to containing node block

# warning

* This plugin is just a proof of concept - from a novice lua coder, and much worse - from a novice vim user
