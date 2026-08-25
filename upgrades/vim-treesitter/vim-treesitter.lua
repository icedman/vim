local cpath = package.cpath
local source = debug.getinfo(1, "S").source:sub(2)
local script_dir = source:match("^(.*[/\\])") or "./"
package.cpath = script_dir .. "?.so;" .. cpath .. ";" .. vim.fn.expand("~/.vim/lua/vim-treesitter/?.so")

local module = require("treesitter")

package.cpath = cpath

local debug_nodes = false
local props = {}
local parse_timers = {}


local scope_hl_map = {
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
	{ "comment", "Comment" },
	{ "string", "String" },
	{ "number", "Number" },
	{ "tag", "StorageClass" },
	{ "attribute", "Boolean" },
	{ "value", "String" },
}

function ts_highlight_buffer()
	local buf = vim.buffer()
	local n = buf.number
	local r = vim.window().line
	local c = vim.window().column

	for hl, _ in pairs(props) do
		vim.command("silent! call prop_remove({'types': ['TS_" .. hl .. "'], 'all': 1}, 1, " .. #buf .. ")")
	end

	for i = 0, #buf - 1, 1 do
		local nodes = module.query_tree(n, i, 0)
		for j, node in ipairs(nodes) do
			local start_row = math.floor(node[1])
			local start_column = math.floor(node[2])
			local end_row = math.floor(node[3])
			local end_column = math.floor(node[4])
			local node_type = node[5]

			if debug_nodes and i + 1 == r and start_column + 1 < c and end_column > c then
				print(node_type)
			end

			local hl = nil
			for j, map in ipairs(scope_hl_map) do
				if string.find(node_type, map[1]) then
					hl = map[2]
				end
			end

			if hl and start_row == end_row and end_column > start_column then
				if not props[hl] then
					vim.command("highlight default link TS_" .. hl .. " " .. hl)
					vim.command("call prop_type_add('TS_" .. hl .. "', {'highlight': 'TS_" .. hl .. "', 'priority': 999})")
					props[hl] = true
				end
				vim.command(
					"call prop_add("
						.. i + 1
						.. ","
						.. start_column + 1
						.. ", { 'length': "
						.. end_column - start_column
						.. ", 'type': '"
						.. "TS_"
						.. hl
						.. "'})"
				)
			end

			if debug_nodes and start_row ~= end_row and i + 1 == r and start_column + 1 == c then
				print(
					start_row + 1
						.. ","
						.. start_column + 1
						.. " - "
						.. end_row + 1
						.. ","
						.. end_column
						.. " "
						.. node_type
				)
			end
		end
	end
end

function ts_poll_parse(timer)
	local pending = parse_timers[timer]
	if not pending then
		vim.fn.timer_stop(timer)
		return
	end
	if module.parse_ready(pending.buffer, pending.generation) then
		vim.fn.timer_stop(timer)
		parse_timers[timer] = nil
		if vim.buffer().number == pending.buffer then
			ts_highlight_buffer()
		end
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
	local timer = tonumber(vim.eval("timer_start(10, function('TSParsePoll'), {'repeat': -1})"))
	parse_timers[timer] = { buffer = buf.number, generation = generation }
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
	module.log_tree()
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
vim.command("au CursorMoved,CursorMovedI * :lua ts_highlight_buffer()")
vim.command("au TextChanged,TextChangedI * :lua ts_parse_buffer()")
vim.command("command TSLogTree 0 % :lua ts_log_tree()")
vim.command("command TSDebugNodes 0 % :lua ts_debug_nodes()")

vim.command("command TSOuterBlock 0 % :lua ts_outer_block()")
vim.command("noremap grn :TSOuterBlock<CR>")
vim.command("command TSInnerBlock 0 % :lua ts_inner_block()")
vim.command("noremap grm :TSInnerBlock<CR>")
