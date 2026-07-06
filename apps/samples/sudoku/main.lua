-- Sudoku: button-only 9x9 puzzle game.

local puzzles = require("puzzles")
local sudoku = require("sudoku")
local draw = require("draw")

local DIFFICULTIES = { "easy", "medium", "hard" }
local DIFF_LABELS = { easy = "Easy", medium = "Medium", hard = "Hard" }
local MENU_ITEMS = { "Easy", "Medium", "Hard" }

local state = "menu"
local menu_sel = 1
local difficulty = "easy"
local puzzle_index = 1
local givens_board = nil
local givens_str = ""
local board = nil
local cursor_r, cursor_c = 1, 1
local entry_mode = false
local entry_digit = 1
local start_ms = 0
local elapsed_ms = 0
local flash_msg = nil
local flash_until = 0

local function save_game()
  if not board then
    return
  end
  local lines = {
    "puzzle_id=" .. difficulty .. "_" .. tostring(puzzle_index),
    "board=" .. sudoku.to_string(board),
    "elapsed_ms=" .. tostring(cp.sys.millis() - start_ms),
  }
  cp.fs.write("save.txt", table.concat(lines, "\n"))
end

local function load_game()
  if not cp.fs.exists("save.txt") then
    return false
  end
  local data = cp.fs.read("save.txt")
  if not data then
    return false
  end
  local pid = data:match("puzzle_id=([%w_]+)")
  local bstr = data:match("board=(%d+)")
  local el = data:match("elapsed_ms=(%d+)")
  if not pid or not bstr then
    return false
  end
  local diff, idx = pid:match("^(%w+)_(%d+)$")
  if not diff or not puzzles[diff] or not puzzles[diff][tonumber(idx)] then
    return false
  end
  difficulty = diff
  puzzle_index = tonumber(idx)
  givens_str = puzzles[diff][puzzle_index].givens
  givens_board = sudoku.parse(givens_str)
  board = sudoku.parse(bstr)
  elapsed_ms = tonumber(el) or 0
  start_ms = cp.sys.millis() - elapsed_ms
  state = "play"
  return true
end

local function start_puzzle(diff, idx)
  difficulty = diff
  puzzle_index = idx
  givens_str = puzzles[diff][idx].givens
  givens_board = sudoku.parse(givens_str)
  board = sudoku.copy(givens_board)
  cursor_r, cursor_c = 1, 1
  entry_mode = false
  start_ms = cp.sys.millis()
  state = "play"
  save_game()
end

local function is_given(r, c)
  return givens_board[r][c] ~= 0
end

local function move_cursor(dr, dc)
  cursor_r = ((cursor_r - 1 + dr) % 9) + 1
  cursor_c = ((cursor_c - 1 + dc) % 9) + 1
end

local function show_flash(msg, ms)
  flash_msg = msg
  flash_until = cp.sys.millis() + (ms or 800)
end

local function current_popup()
  if flash_msg and cp.sys.millis() < flash_until then
    return flash_msg
  end
  return nil
end

local function play_hints()
  if entry_mode then
    return {
      back = "Cancel",
      confirm = "Place",
      left = "",
      right = "",
    }
  end
  return {
    back = "Back",
    confirm = "Edit",
    left = "",
    right = "",
  }
end

local function draw_play()
  local conflicts = sudoku.get_conflicts(board)
  local elapsed = cp.sys.millis() - start_ms
  draw.draw_board(board, givens_board, conflicts, cursor_r, cursor_c, {
    entry_mode = entry_mode,
    entry_digit = entry_digit,
    subtitle = draw.play_subtitle(DIFF_LABELS[difficulty] or difficulty, puzzle_index, elapsed),
    hints = play_hints(),
    popup = current_popup(),
  })
end

local function commit_digit()
  if is_given(cursor_r, cursor_c) then
    show_flash("Fixed cell", 900)
    entry_mode = false
    return
  end
  board[cursor_r][cursor_c] = entry_digit
  entry_mode = false
  save_game()
  if sudoku.is_complete(board) then
    elapsed_ms = cp.sys.millis() - start_ms
    state = "win"
    cp.fs.write("save.txt", "")
  end
end

local function next_puzzle()
  local list = puzzles[difficulty]
  puzzle_index = (puzzle_index % #list) + 1
  start_puzzle(difficulty, puzzle_index)
end

local function cycle_entry_digit(delta)
  entry_digit = entry_digit + delta
  if entry_digit > 9 then
    entry_digit = 0
  elseif entry_digit < 0 then
    entry_digit = 9
  end
end

-- Main loop
if load_game() then
  cp.sys.log("sudoku: resumed save")
else
  state = "menu"
end

while true do
  if state == "menu" then
    draw.draw_menu(menu_sel, MENU_ITEMS)
    local btn = cp.input.wait()
    if btn == "back" then
      cp.sys.exit()
    elseif btn == "up" then
      menu_sel = ((menu_sel - 2) % 3) + 1
    elseif btn == "down" then
      menu_sel = (menu_sel % 3) + 1
    elseif btn == "confirm" then
      start_puzzle(DIFFICULTIES[menu_sel], 1)
    end
  elseif state == "play" then
    draw_play()
    local btn = cp.input.wait()
    if entry_mode then
      if btn == "back" then
        entry_mode = false
      elseif btn == "up" then
        cycle_entry_digit(1)
      elseif btn == "down" then
        cycle_entry_digit(-1)
      elseif btn == "confirm" then
        commit_digit()
      elseif btn == "left" then
        cycle_entry_digit(-1)
      elseif btn == "right" then
        cycle_entry_digit(1)
      end
    else
      if btn == "back" then
        save_game()
        for i, diff in ipairs(DIFFICULTIES) do
          if diff == difficulty then
            menu_sel = i
            break
          end
        end
        state = "menu"
      elseif btn == "up" then
        move_cursor(-1, 0)
      elseif btn == "down" then
        move_cursor(1, 0)
      elseif btn == "left" then
        move_cursor(0, -1)
      elseif btn == "right" then
        move_cursor(0, 1)
      elseif btn == "confirm" then
        if is_given(cursor_r, cursor_c) then
          show_flash("Fixed cell", 900)
        else
          entry_digit = board[cursor_r][cursor_c]
          if entry_digit == 0 then
            entry_digit = 1
          end
          entry_mode = true
        end
      end
    end
  elseif state == "win" then
    local conflicts = sudoku.get_conflicts(board)
    draw.draw_win(board, givens_board, conflicts, cursor_r, cursor_c, elapsed_ms, {
      back = "Menu",
      confirm = "Next",
      left = "",
      right = "",
    })
    local btn = cp.input.wait()
    if btn == "back" then
      state = "menu"
      cp.fs.write("save.txt", "")
    elseif btn == "confirm" then
      next_puzzle()
    end
  end
end
