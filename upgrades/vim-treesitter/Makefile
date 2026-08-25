all: prebuild build install

.PHONY: all prebuild build install uninstall clean

prebuild:
	meson setup build --reconfigure

build:
	meson compile -C build
	cp build/treesitter.so ./

install:
	mkdir -p ~/.vim/lua/vim-treesitter
	cp treesitter.so ~/.vim/lua/vim-treesitter
	cp vim-treesitter.lua ~/.vim/lua/vim-treesitter

uninstall:
	rm -rf ~/.vim/lua/vim-treesitter

clean:
	rm -rf build treesitter.so
