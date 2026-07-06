-- Sudoku themed display helpers.

local M = {}

local PAD = 20
local HINT_LINE_H = 20

local function layout()
  local sw = cp.display.width()
  local ch = cp.display.content_height()
  local picker_cell = math.max(24, math.floor((sw - 2 * PAD) / 9))
  local picker_block_h = picker_cell + HINT_LINE_H
  local grid_size = math.min(sw - 2 * PAD, ch - picker_block_h - 8)
  local cell = math.floor(grid_size / 9)
  grid_size = cell * 9
  local ox = math.floor((sw - grid_size) / 2)
  local oy = 0
  local picker_y = oy + grid_size + 8
  return {
    ox = ox,
    oy = oy,
    cell = cell,
    grid_size = grid_size,
    picker_y = picker_y,
    picker_cell = picker_cell,
  }
end

local function format_time(elapsed_ms)
  local total_secs = math.floor(elapsed_ms / 1000)
  local mins = math.floor(total_secs / 60)
  local secs = total_secs % 60
  if mins > 0 then
    return string.format("%d:%02d", mins, secs)
  end
  return string.format("0:%02d", secs)
end

local function draw_digit_picker(lay, entry_digit)
  local sw = cp.display.width()
  local strip_w = lay.picker_cell * 9
  local strip_x = math.floor((sw - strip_w) / 2)
  local y = lay.picker_y

  for d = 1, 9 do
    local cx = strip_x + (d - 1) * lay.picker_cell
    if d == entry_digit then
      cp.display.fill_rect(cx + 1, y + 1, lay.picker_cell - 2, lay.picker_cell - 2)
    else
      cp.display.rect(cx + 1, y + 1, lay.picker_cell - 2, lay.picker_cell - 2, 1)
    end
    local tw = 8
    local tx = cx + math.floor((lay.picker_cell - tw) / 2)
    local ty = y + math.floor((lay.picker_cell - 12) / 2)
    cp.display.text(tx, ty, tostring(d))
  end

  cp.display.center(y + lay.picker_cell + 4, "Up/Down/Left/Right: digit · 0 clears")
end

function M.draw_menu(selected, items)
  cp.display.clear()
  cp.ui.list({ items = items, selected = selected })
  cp.display.refresh({
    subtitle = "Choose difficulty",
    hints = { back = "Exit", confirm = "Start", left = "", right = "" },
  })
end

function M.draw_board(board, givens, conflicts, cursor_r, cursor_c, opts)
  opts = opts or {}
  cp.display.clear()

  local lay = layout()
  cp.display.grid({
    ox = lay.ox,
    oy = lay.oy,
    cell = lay.cell,
    board = board,
    givens = givens,
    conflicts = conflicts,
    cursor = { r = cursor_r, c = cursor_c },
    entry_mode = opts.entry_mode or false,
    entry_digit = opts.entry_digit or 0,
  })

  if opts.entry_mode then
    draw_digit_picker(lay, opts.entry_digit or 1)
  else
    cp.display.center(lay.picker_y + 6, "Up/Down: row · Left/Right: col")
  end

  if opts.popup then
    cp.ui.popup(opts.popup)
  end

  cp.display.refresh({
    subtitle = opts.subtitle or "",
    hints = opts.hints or {},
  })
end

function M.draw_win(board, givens, conflicts, cursor_r, cursor_c, elapsed_ms, hints)
  M.draw_board(board, givens, conflicts, cursor_r, cursor_c, {
    subtitle = "Puzzle complete",
    hints = hints,
    popup = "Solved! Time: " .. format_time(elapsed_ms),
  })
end

function M.format_time(elapsed_ms)
  return format_time(elapsed_ms)
end

function M.play_subtitle(difficulty_label, puzzle_index, elapsed_ms)
  return difficulty_label .. " · #" .. tostring(puzzle_index) .. " · " .. format_time(elapsed_ms)
end

return M
