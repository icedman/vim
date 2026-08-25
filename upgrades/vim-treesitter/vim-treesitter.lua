local cpath = package.cpath
local source = debug.getinfo(1, "S").source:sub(2)
local script_dir = source:match("^(.*[/\\])") or "./"
package.cpath = script_dir .. "?.so;" .. cpath .. ";" .. vim.fn.expand("~/.vim/lua/vim-treesitter/?.so")

local module = require("treesitter")

package.cpath = cpath

local debug_nodes = false
local props = {}
local parse_timers = {}
local buffer_timers = {}

local scope_hl_map = {
	{ "comment", "Comment" },
	{ "expression", "Operator" },
	{ "identifier", "Identifier" },
	{ "template", "Boolean" },
	{ "qualified", "StorageClass" },
	{ "descriptor", "StorageClass" },
	{ "primitive", "StorageClass" },
	{ "include", "Include" },
	{ "import", "Include" },
	{ "define", "Define" },
	{ "preproc", "PreProc" },
	{ "struct", "Structure" },
	{ "return", "Keyword" },
	{ "call", "Function" },
	{ "string", "String" },
	{ "number", "Number" },
	{ "tag", "StorageClass" },
	{ "attribute", "Boolean" },
	{ "value", "String" },
}

local function highlight_node(node)
	local start_row = math.floor(node[1])
	local start_column = math.floor(node[2])
	local end_row = math.floor(node[3])
	local end_column = math.floor(node[4])
	local node_type = node[5]
	local hl = nil

	for _, map in ipairs(scope_hl_map) do
		if string.find(node_type, map[1], 1, true) then
			hl = map[2]
		end
	end
	if not hl or start_row ~= end_row or end_column <= start_column then
		return
	end

	if not props[hl] then
		vim.command("highlight default link TS_" .. hl .. " " .. hl)
		vim.command("call prop_type_add('TS_" .. hl .. "', {'highlight': 'TS_" .. hl .. "', 'priority': 999})")
		props[hl] = true
	end
	vim.command(
		"call prop_add("
			.. start_row + 1
			.. ","
			.. start_column + 1
			.. ", {'length': "
			.. end_column - start_column
			.. ", 'type': 'TS_"
			.. hl
			.. "'})"
	)
end

local function begin_highlight(pending)
	local buf = vim.buffer()
	for hl, _ in pairs(props) do
		vim.command("silent! call prop_remove({'types': ['TS_" .. hl .. "'], 'all': 1}, 1, " .. #buf .. ")")
	end
	pending.nodes = module.query_tree(pending.buffer, -1, 0)
	pending.index = 1
	pending.highlighting = true
end

function ts_poll_parse(timer)
	local pending = parse_timers[timer]
	if not pending or vim.buffer().number ~= pending.buffer then
		vim.fn.timer_stop(timer)
		parse_timers[timer] = nil
		buffer_timers[pending and pending.buffer or -1] = nil
		return
	end

	if not pending.highlighting then
		if not module.parse_ready(pending.buffer, pending.generation) then
			return
		end
		begin_highlight(pending)
	end

	local last = math.min(pending.index + 199, #pending.nodes)
	for i = pending.index, last do
		highlight_node(pending.nodes[i])
	end
	pending.index = last + 1
	if pending.index > #pending.nodes then
		vim.fn.timer_stop(timer)
		parse_timers[timer] = nil
		buffer_timers[pending.buffer] = nil
	end
end

function ts_parse_buffer()
	local buf = vim.buffer()
	local fn = vim.fn.expand("%")
	local ext = fn:match("^.+(%..+)$")
	if not ext then
		return
	end

	local generation = module.parse_buffer(ext, buf.number)
	if type(generation) ~= "number" then
		return
	end
	local old_timer = buffer_timers[buf.number]
	if old_timer then
		vim.fn.timer_stop(old_timer)
		parse_timers[old_timer] = nil
	end
	local timer = tonumber(vim.eval("timer_start(10, function('TSParsePoll'), {'repeat': -1})"))
	parse_timers[timer] = { buffer = buf.number, generation = generation }
	buffer_timers[buf.number] = timer
end

function ts_block(r)
	local buf = vim.buffer()
	local n = buf.number

	local nodes = module.query_tree(n, r - 1, 1)

	if #nodes > 1 then
		local node = nodes[#nodes]
		local start_row = math.floor(node[1])
		local start_column = math.floor(node[2])
		local end_row = math.floor(node[3])
		local end_column = math.floor(node[4])
		local node_type = node[5]
		return { start_row + 1, end_row + 1 }
	end
end

function ts_outer_block()
	local res = ts_block(vim.window().line)
	if res then
		vim.command(":" .. res[1])
	end
end

function ts_inner_block()
	local r = vim.window().line
	local res = ts_block(r)
	if not res then
		return
	end

	local current = res[1]
	for i = res[1], res[2], 1 do
		local res2 = ts_block(i + 1)
		if not res[2] then
			return
		end
		if res2[1] ~= current then
			vim.command(":" .. res2[1])
			return
		end
	end
end

function ts_log_tree()
	if module.log_tree(vim.buffer().number) then
		print("Tree-sitter tree written to /tmp/vim-treesitter.log")
	else
		print("Tree-sitter tree is not ready")
	end
end

function ts_debug_nodes()
	if debug_nodes then
		debug_nodes = false
	else
		debug_nodes = true
	end
end

vim.command([[
function! TSParsePoll(timer) abort
  lua ts_poll_parse(tonumber(vim.eval("a:timer")))
endfunction
]])

vim.command("au BufEnter * :lua ts_parse_buffer()")

vim.command("au TextChanged,TextChangedI * :lua ts_parse_buffer()")
vim.command("command TSLogTree 0 % :lua ts_log_tree()")
vim.command("command TSDebugNodes 0 % :lua ts_debug_nodes()")

vim.command("command TSOuterBlock 0 % :lua ts_outer_block()")
vim.command("noremap grn :TSOuterBlock<CR>")
vim.command("command TSInnerBlock 0 % :lua ts_inner_block()")
vim.command("noremap grm :TSInnerBlock<CR>")
