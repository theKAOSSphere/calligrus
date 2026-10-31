-- calligrus: live Markdown preview in a separate calligrus window (spec 15).
--
-- One shared `calligrus --editor` process per Neovim session. We talk to it on
-- its stdin, one JSON object per line:
--   open   {doc, path, text}   show this buffer now
--   text   {doc, text}         the buffer changed
--   cursor {doc, line}         scroll the preview to this source line
--   close  {doc}               the buffer is gone
--   quit                       exit
-- calligrus talks back with one message, read from the job's stdout (spec 23):
--   goto   {doc, line}         the user Ctrl+clicked in the preview: move the cursor there
-- Everything else is "send messages when Vim events happen".

local M = {}

local config = {
  -- A program name/path, or a list like { "python", "stub.py" } (program + leading arguments).
  exe = "calligrus",
  -- While typing, send the whole text at most this often (ms).
  debounce = 150,
  -- Buffers with these 'filetype's can be previewed (`:CalligrusToggle!` ignores that).
  filetypes = { "markdown", "org" },
  -- While the preview window is open, every buffer with one of `filetypes`
  -- that you switch to is previewed too (no :CalligrusToggle per buffer).
  -- false: only buffers you toggled on.
  follow = true,
  -- Ctrl+click in the preview moves the cursor to that source line.
  -- false: ignore the preview's `goto` messages.
  goto = true,
}

-- The running process (a job id) or nil.
local job = nil
-- Set while WE stop the process, so on_exit doesn't complain about it.
local stopping = false
-- previewed[bufnr] = true for buffers whose preview is on.
local previewed = {}
-- muted[bufnr] = true for buffers you toggled off: `follow` leaves them alone.
local muted = {}
-- One debounce timer per buffer and kind: timers.text[buf], timers.cursor[buf].
local timers = { text = {}, cursor = {} }
local augroup = vim.api.nvim_create_augroup("Calligrus", { clear = true })
local uv = vim.uv or vim.loop

local function notify(msg, level)
  vim.notify("calligrus: " .. msg, level or vim.log.levels.INFO)
end

-- The command line that starts calligrus in editor mode.
local function command()
  local exe = config.exe
  local cmd = type(exe) == "table" and vim.deepcopy(exe) or { exe }
  table.insert(cmd, "--editor")
  return cmd
end

-- The document id we use in the protocol: simply the buffer number.
local function doc_id(buf)
  return tostring(buf)
end

local function stop_timer(kind, buf)
  local t = timers[kind][buf]
  if t then
    t:stop()
    t:close()
    timers[kind][buf] = nil
  end
end

local function stop_all_timers()
  for kind, per_buf in pairs(timers) do
    for buf in pairs(per_buf) do
      stop_timer(kind, buf)
    end
  end
end

-- Show buffer `buf` in a window of the current tabpage (the current window if it
-- shows it, else any window that does, else the current window) and put the
-- cursor on `line` (clamped), at the first non-blank, with folds opened.
local function goto_line(buf, line)
  local win
  if vim.api.nvim_win_get_buf(0) == buf then
    win = vim.api.nvim_get_current_win()
  else
    for _, w in ipairs(vim.api.nvim_tabpage_list_wins(0)) do
      if vim.api.nvim_win_get_buf(w) == buf then
        win = w
        break
      end
    end
    if win then
      vim.api.nvim_set_current_win(win)
    else
      win = vim.api.nvim_get_current_win()
      vim.api.nvim_win_set_buf(win, buf)
    end
  end
  line = math.max(1, math.min(math.floor(line), vim.api.nvim_buf_line_count(buf)))
  vim.api.nvim_win_set_cursor(win, { line, 0 })
  vim.api.nvim_win_call(win, function()
    vim.cmd("normal! ^")
    vim.cmd("normal! zv")
  end)
end

-- One message from calligrus's stdout (a line of JSON). Unknown or malformed: ignored.
local function handle_message(line)
  if not config.goto then
    return
  end
  local ok, msg = pcall(vim.json.decode, line)
  if not ok or type(msg) ~= "table" or msg.type ~= "goto" then
    return
  end
  local buf = type(msg.doc) == "string" and tonumber(msg.doc) or nil
  if not buf or not previewed[buf] or not vim.api.nvim_buf_is_valid(buf) or type(msg.line) ~= "number" then
    return
  end
  -- (The cursor move this causes sends a normal `cursor` message; calligrus never answers it.)
  local done, err = pcall(goto_line, buf, msg.line)
  if not done then
    notify("cannot go to line " .. msg.line .. ": " .. tostring(err), vim.log.levels.WARN)
  end
end

-- Send one protocol message; false if there is no process (or it is gone).
local function send(msg)
  if not job then
    return false
  end
  local ok, line = pcall(vim.json.encode, msg)
  if not ok then
    return false -- e.g. a buffer with invalid UTF-8: skip this update
  end
  return pcall(vim.fn.chansend, job, line .. "\n")
end

local function buffer_text(buf)
  return table.concat(vim.api.nvim_buf_get_lines(buf, 0, -1, false), "\n")
end

local function send_open(buf)
  local path = vim.api.nvim_buf_get_name(buf)
  if path ~= "" then
    path = vim.fn.fnamemodify(path, ":p")
  end
  local format = vim.bo[buf].filetype == "org" and "org" or "markdown"
  send({ type = "open", doc = doc_id(buf), path = path, format = format, text = buffer_text(buf) })
end

local function send_text(buf)
  send({ type = "text", doc = doc_id(buf), text = buffer_text(buf) })
end

-- The cursor line of the window showing `buf` (the current window if it shows it).
local function cursor_line(buf)
  local win = vim.api.nvim_get_current_win()
  if vim.api.nvim_win_get_buf(win) ~= buf then
    win = vim.fn.bufwinid(buf)
    if win == -1 then
      return nil
    end
  end
  return vim.api.nvim_win_get_cursor(win)[1]
end

local function send_cursor(buf)
  local line = cursor_line(buf)
  if line then
    send({ type = "cursor", doc = doc_id(buf), line = line })
  end
end

-- Vim's search pattern -> plain text for the preview's find, plus case
-- sensitivity. Returns "" for patterns that are real regexes: the preview
-- can only match plain text, and a wrong highlight is worse than none.
function M.search_text(pat)
  local case = nil
  if pat:find("\\C", 1, true) then case = true end
  if pat:find("\\c", 1, true) then case = false end
  pat = pat:gsub("\\[cC]", "")
  local very_nomagic = pat:find("\\V", 1, true) ~= nil
  local very_magic = pat:find("\\v", 1, true) ~= nil
  pat = pat:gsub("\\[vV]", ""):gsub("\\<", ""):gsub("\\>", "")
  if very_magic then
    pat = pat:gsub("[<>]", "") -- \v<word> is a whole-word match
    if pat:find("[%.%*%[%]~%^%$\\{}%+=%?|%(%)@%%]") then return "", false end
  elseif very_nomagic then
    if pat:find("\\", 1, true) then return "", false end
  else
    if pat:find("[%.%*%[~\\]") or pat:sub(1, 1) == "^" or pat:sub(-1) == "$" then return "", false end
  end
  if case == nil then
    case = not vim.o.ignorecase or (vim.o.smartcase and pat:find("%u") ~= nil)
  end
  return pat, case
end

-- What the preview should highlight now: the last search while it's shown.
local function current_search()
  if not vim.o.hlsearch or vim.v.hlsearch == 0 then
    return "", false
  end
  return M.search_text(vim.fn.getreg("/"))
end

-- last_search[buf] = "case|text" as last sent, so we only send changes.
local last_search = {}
local function send_search(buf, force)
  if not previewed[buf] then
    return
  end
  local text, case = current_search()
  local key = tostring(case) .. "|" .. text
  if force or last_search[buf] ~= key then
    last_search[buf] = key
    send({ type = "search", doc = doc_id(buf), text = text, case = case })
  end
end

-- Run `fn` once, `ms` after the LAST call for (kind, buf): rapid events collapse into one.
local function debounced(kind, buf, ms, fn)
  stop_timer(kind, buf)
  local t = uv.new_timer()
  timers[kind][buf] = t
  t:start(ms, 0, vim.schedule_wrap(function()
    if timers[kind][buf] == t then
      stop_timer(kind, buf)
      if previewed[buf] and vim.api.nvim_buf_is_valid(buf) then
        fn(buf)
      end
    end
  end))
end

-- Run `fn` `ms` after the FIRST call for (kind, buf); calls in between just
-- ride along. So while you type non-stop the preview still updates every
-- `ms` (a debounce would wait until you pause), and the last edit always
-- gets sent because `fn` reads the buffer when the timer fires.
local function throttled(kind, buf, ms, fn)
  if timers[kind][buf] then
    return
  end
  local t = uv.new_timer()
  timers[kind][buf] = t
  t:start(ms, 0, vim.schedule_wrap(function()
    if timers[kind][buf] == t then
      stop_timer(kind, buf)
      if previewed[buf] and vim.api.nvim_buf_is_valid(buf) then
        fn(buf)
      end
    end
  end))
end

-- Start the shared process if it isn't running. Returns true if it is.
local function ensure_job()
  if job then
    return true
  end
  stopping = false
  -- stdout arrives in arbitrary chunks: `data` is a list split at newlines, so its
  -- first item continues the unfinished line from the last call and its last
  -- item is the new unfinished line ("" when the chunk ended with a newline).
  local partial = ""
  local id = vim.fn.jobstart(command(), {
    stdin = "pipe",
    on_stdout = function(_, data)
      for i, piece in ipairs(data) do
        partial = partial .. piece
        if i < #data then
          local line = partial:gsub("\r$", "")
          partial = ""
          if line ~= "" then
            vim.schedule(function() handle_message(line) end)
          end
        end
      end
    end,
    on_exit = function(_, code)
      -- Called on the main loop. Whatever the reason, the previews are over.
      local was_stopped = stopping
      job = nil
      stopping = false
      previewed = {}
      stop_all_timers()
      if not was_stopped then
        notify("the preview window closed (exit code " .. code .. "); previews are off", vim.log.levels.WARN)
      end
    end,
  })
  if id <= 0 then
    local what = (id == -1) and "cannot run `" .. table.concat(command(), " ") .. "` (not found? see `exe` in setup())" or "cannot start the process"
    notify(what, vim.log.levels.ERROR)
    return false
  end
  job = id
  return true
end

local function is_previewable(buf)
  return vim.tbl_contains(config.filetypes, vim.bo[buf].filetype)
end

-- :CalligrusToggle[!] -- preview the current buffer on/off.
function M.toggle(force)
  local buf = vim.api.nvim_get_current_buf()
  if previewed[buf] then
    send({ type = "close", doc = doc_id(buf) })
    previewed[buf] = nil
    muted[buf] = true
    stop_timer("text", buf)
    stop_timer("cursor", buf)
    notify("preview off")
    return
  end
  if not force and not is_previewable(buf) then
    notify("'filetype' is not one of " .. table.concat(config.filetypes, ", ") .. " (use :CalligrusToggle! to preview anyway)", vim.log.levels.WARN)
    return
  end
  if not ensure_job() then
    return
  end
  muted[buf] = nil
  previewed[buf] = true
  send_open(buf)
  send_cursor(buf)
  send_search(buf, true)
end

-- :CalligrusStop -- quit the process; all previews turn off.
function M.stop()
  if not job then
    return
  end
  stopping = true
  send({ type = "quit" })
  -- Don't wait for it to read the message: closing stdin also makes it exit.
  pcall(vim.fn.chanclose, job, "stdin")
  previewed = {}
  stop_all_timers()
end

-- :CalligrusOpenFile -- level 1: just open the file in a new, unconnected calligrus window.
function M.open_file()
  local path = vim.api.nvim_buf_get_name(0)
  if path == "" then
    notify("this buffer has no file", vim.log.levels.WARN)
    return
  end
  local cmd = type(config.exe) == "table" and vim.deepcopy(config.exe) or { config.exe }
  table.insert(cmd, vim.fn.fnamemodify(path, ":p"))
  if vim.fn.jobstart(cmd, { detach = true }) <= 0 then
    notify("cannot run `" .. table.concat(cmd, " ") .. "`", vim.log.levels.ERROR)
  end
end

-- For tests and :checkhealth-style questions.
function M.is_running()
  return job ~= nil
end

function M.setup(opts)
  config = vim.tbl_deep_extend("force", config, opts or {})
  -- (A list for `exe` must replace, not merge with, the default string: tbl_deep_extend does that.)
  vim.api.nvim_clear_autocmds({ group = augroup })
  local function on(events, fn)
    vim.api.nvim_create_autocmd(events, { group = augroup, callback = fn })
  end

  -- Switching to a previewed buffer shows it (the one window follows the
  -- current buffer). With `follow`, any Markdown buffer joins in while the
  -- window is open. FileType too: a newly opened file gets its 'filetype'
  -- only after BufEnter.
  on({ "BufEnter", "FileType" }, function(a)
    if a.buf ~= vim.api.nvim_get_current_buf() then
      return
    end
    if config.follow and job and not previewed[a.buf] and not muted[a.buf] and is_previewable(a.buf) then
      previewed[a.buf] = true
    end
    if previewed[a.buf] then
      send_open(a.buf)
      send_cursor(a.buf)
      send_search(a.buf, true)
    end
  end)
  -- Search highlight: `/` `?` and `:nohlsearch` end on the command line (run
  -- after the command has happened: vim.schedule); `n` `N` `*` `#` move the
  -- cursor; `:set nohlsearch` is an option change.
  on("CmdlineLeave", function()
    vim.schedule(function() send_search(vim.api.nvim_get_current_buf()) end)
  end)
  on("OptionSet", function(a)
    if a.match == "hlsearch" then
      send_search(vim.api.nvim_get_current_buf())
    end
  end)
  -- TextChangedP: typing while the completion menu is open.
  on({ "TextChanged", "TextChangedI", "TextChangedP" }, function(a)
    if previewed[a.buf] then
      throttled("text", a.buf, config.debounce, send_text)
    end
  end)
  on({ "CursorMoved", "CursorMovedI" }, function(a)
    if previewed[a.buf] then
      debounced("cursor", a.buf, 50, send_cursor)
      send_search(a.buf)
    end
  end)
  on({ "BufDelete", "BufWipeout" }, function(a)
    muted[a.buf] = nil
    if previewed[a.buf] then
      send({ type = "close", doc = doc_id(a.buf) })
      previewed[a.buf] = nil
      stop_timer("text", a.buf)
      stop_timer("cursor", a.buf)
    end
  end)
  on("VimLeavePre", function()
    M.stop()
  end)
end

-- Work with defaults even if the user never calls setup().
M.setup()

return M
