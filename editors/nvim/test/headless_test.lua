-- Run: nvim --headless -n -u NONE --cmd "set rtp+=<editors/nvim>" -c "luafile test/headless_test.lua"
-- with CALLIGRUS_STUB_LOG set to a scratch file. Prints PASS/FAIL lines, exits non-zero on failure.
vim.cmd("runtime plugin/calligrus.lua")
local here = vim.fn.fnamemodify(debug.getinfo(1, "S").source:sub(2), ":p:h")
local log = os.getenv("CALLIGRUS_STUB_LOG")
os.remove(log)
-- The stub writes lines we append to this file to its stdout (what the app's `goto` looks like).
local out = log .. ".out"
os.remove(out)
vim.env.CALLIGRUS_STUB_OUT = out
require("calligrus").setup({ exe = { "python", here .. "/stub.py" }, debounce = 50 })

local failures = 0
local function check(name, ok)
  print((ok and "PASS " or "FAIL ") .. name)
  if not ok then failures = failures + 1 end
end
local function lines()
  local out = {}
  local f = io.open(log, "rb")
  if not f then return out end
  for l in f:lines() do out[#out + 1] = l end
  f:close()
  return out
end
local function types()
  local out = {}
  for _, l in ipairs(lines()) do
    local ok, m = pcall(vim.json.decode, l)
    if ok and type(m) == "table" then out[#out + 1] = m end
  end
  return out
end
local function wait_for(pred) return vim.wait(3000, pred, 20) end
local function count(t) local n = 0 for _, m in ipairs(types()) do if m.type == t then n = n + 1 end end return n end

local file = here .. "/sample.md"
vim.cmd("edit " .. vim.fn.fnameescape(file))
vim.bo.filetype = "markdown"
vim.api.nvim_buf_set_lines(0, 0, -1, false, { "# Title", "", "line three \"quoted\" é", "", "last" })
local buf = vim.api.nvim_get_current_buf()

-- toggle on -> open (+ cursor)
vim.cmd("CalligrusToggle")
check("process started", wait_for(function() return count("open") == 1 end))
local open = types()[1]
check("args include --editor", lines()[1] == "ARGS --editor")
check("open doc/path/text", open and open.doc == tostring(buf) and open.path:find("sample.md", 1, true) and open.text == "# Title\n\nline three \"quoted\" é\n\nlast")
check("open format", open and open.format == "markdown")
check("cursor sent on toggle", wait_for(function() return count("cursor") >= 1 end))

-- cursor move -> cursor with the new line
vim.api.nvim_win_set_cursor(0, { 3, 0 })
vim.cmd("doautocmd CursorMoved")
check("cursor line 3", wait_for(function()
  local t = types()
  return t[#t].type == "cursor" and t[#t].line == 3
end))

-- edits: three quick changes -> one coalesced text message with the final text
local before = count("text")
for i = 1, 3 do
  vim.api.nvim_buf_set_lines(0, 4, 5, false, { "last " .. i })
  vim.cmd("doautocmd TextChanged")
end
check("text sent", wait_for(function() return count("text") > before end))
vim.wait(300)
check("text debounced to one message", count("text") == before + 1)
local texts = {}
for _, m in ipairs(types()) do if m.type == "text" then texts[#texts + 1] = m end end
check("text is the latest", texts[#texts].text:sub(-6) == "last 3")

-- second buffer: BufEnter of the previewed one sends open again
vim.cmd("enew")
local opens = count("open")
vim.cmd("buffer " .. buf)
check("BufEnter -> open", wait_for(function() return count("open") == opens + 1 end))

-- follow: another Markdown buffer is previewed without :CalligrusToggle
vim.cmd("enew")
local other = vim.api.nvim_get_current_buf()
vim.bo.filetype = "markdown"
check("follow: new markdown buffer opens", wait_for(function() return count("open") == opens + 2 end))
-- ... unless you toggled it off: then switching back to it leaves it alone
vim.cmd("CalligrusToggle")
vim.cmd("buffer " .. buf)
vim.cmd("buffer " .. other)
vim.wait(100)
check("follow: a muted buffer stays off", count("open") == opens + 3)
opens = opens + 2

-- non-markdown buffer is refused without !
vim.cmd("enew")
vim.bo.filetype = "text"
vim.cmd("CalligrusToggle")
vim.wait(100)
check("non-markdown refused", count("open") == opens + 1)

-- goto (Ctrl+click in the preview): lines the app writes to stdout move the cursor
local function emit(s)
  local f = io.open(out, "ab")
  f:write(s .. "\n")
  f:close()
end
local function goto_msg(doc, line) return '{"type":"goto","doc":"' .. doc .. '","line":' .. line .. '}\\n' end
local function cur() return vim.api.nvim_win_get_cursor(0) end
vim.api.nvim_buf_set_lines(buf, 0, -1, false, { "# Title", "", "    indented three", "", "last" })
vim.cmd("enew") -- some other buffer is current
vim.bo.filetype = "text"
emit(goto_msg(buf, 3))
check("goto: shows the buffer in the current window", wait_for(function() return vim.api.nvim_get_current_buf() == buf end))
check("goto: line and first non-blank", wait_for(function() return cur()[1] == 3 and cur()[2] == 4 end))
vim.cmd("doautocmd CursorMoved")
check("goto: cursor message follows", wait_for(function()
  local t = types()
  return t[#t].type == "cursor" and t[#t].line == 3
end))
vim.wait(200)
check("goto: no message loop", types()[#types()].type == "cursor")

-- folds at the target are opened
vim.cmd("setlocal foldmethod=manual")
vim.cmd("1,2fold")
vim.cmd("normal! gg")
check("fold closed before", vim.fn.foldclosed(2) ~= -1)
emit(goto_msg(buf, 2))
check("goto: opens folds", wait_for(function() return cur()[1] == 2 and vim.fn.foldclosed(2) == -1 end))

-- a window already showing the buffer is used (no new window, no buffer switch in the other)
vim.cmd("split")
vim.cmd("enew")
local wins = #vim.api.nvim_tabpage_list_wins(0)
local other_win = vim.api.nvim_get_current_win()
emit(goto_msg(buf, 5))
check("goto: uses the window that shows the buffer", wait_for(function()
  return vim.api.nvim_get_current_buf() == buf and vim.api.nvim_get_current_win() ~= other_win
end))
check("goto: no extra window", #vim.api.nvim_tabpage_list_wins(0) == wins)
check("goto: other window untouched", vim.api.nvim_win_get_buf(other_win) ~= buf)
vim.cmd("only")

-- a line past the end is clamped; line 0 too
vim.cmd("normal! gg")
emit(goto_msg(buf, 999))
check("goto: past the end clamps", wait_for(function() return cur()[1] == 5 end))
emit(goto_msg(buf, 0))
check("goto: line 0 clamps to 1", wait_for(function() return cur()[1] == 1 end))

-- a message split over two writes, then ignored input: malformed, unknown type, wrong doc
vim.cmd("normal! G")
emit('{"type":"goto","doc":"' .. buf .. '","li')
vim.wait(150)
check("goto: partial line waits", cur()[1] == 5)
emit('ne":2}\\n')
check("goto: split message is joined", wait_for(function() return cur()[1] == 2 end))
vim.cmd("normal! G")
emit("this is not json\\n")
emit('{"type":"goto","doc":"' .. buf .. '","line":\\n')
emit('{"type":"goto","doc":"' .. buf .. '","line":"2"}\\n')
emit('{"type":"mystery","doc":"' .. buf .. '","line":2}\\n')
emit("[1,2]\\n")
emit(goto_msg(buf + 100, 2))
emit(goto_msg(other, 2)) -- a real buffer, but not previewed (muted)
emit(goto_msg("abc", 2))
vim.wait(400)
check("goto: malformed/unknown/wrong doc ignored", vim.api.nvim_get_current_buf() == buf and cur()[1] == 5)
emit(goto_msg(buf, 1)) -- still alive after the junk
check("goto: works after junk", wait_for(function() return cur()[1] == 1 end))

-- two messages in one write
emit(goto_msg(buf, 2) .. goto_msg(buf, 4))
check("goto: two messages in one chunk", wait_for(function() return cur()[1] == 4 end))

-- goto = false: ignored
require("calligrus").setup({ goto = false })
emit(goto_msg(buf, 1))
vim.wait(400)
check("goto = false: ignored", cur()[1] == 4)
require("calligrus").setup({ goto = true })
emit(goto_msg(buf, 1))
check("goto = true again", wait_for(function() return cur()[1] == 1 end))
require("calligrus").setup({ exe = { "python", here .. "/stub.py" }, debounce = 50 })

-- wiping the previewed buffer -> close
local closes = count("close")
vim.cmd("bwipeout! " .. buf)
check("BufWipeout -> close", wait_for(function() return count("close") == closes + 1 end))

-- stop -> process sees quit then EOF
vim.cmd("CalligrusStop")
check("quit sent", wait_for(function() return count("quit") == 1 end))
check("process exited", wait_for(function() return not require("calligrus").is_running() end))
check("stdin reached EOF", wait_for(function() local l = lines() return l[#l] == "EOF" end))

-- toggling again restarts it
local opens_before = count("open")
vim.cmd("enew")
vim.bo.filetype = "markdown"
vim.cmd("CalligrusToggle")
check("restart after stop", wait_for(function() return count("open") == opens_before + 1 end))
vim.cmd("CalligrusStop")
vim.wait(300)

print(failures == 0 and "ALL PASSED" or (failures .. " FAILED"))
vim.cmd(failures == 0 and "qa!" or "cquit")
