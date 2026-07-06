-- Sudoku board helpers.

local M = {}

function M.parse(board_str)
  local board = {}
  for r = 1, 9 do
    board[r] = {}
    for c = 1, 9 do
      local i = (r - 1) * 9 + c
      local ch = board_str:sub(i, i)
      board[r][c] = tonumber(ch) or 0
    end
  end
  return board
end

function M.to_string(board)
  local parts = {}
  for r = 1, 9 do
    for c = 1, 9 do
      parts[#parts + 1] = tostring(board[r][c])
    end
  end
  return table.concat(parts)
end

function M.copy(board)
  local out = {}
  for r = 1, 9 do
    out[r] = {}
    for c = 1, 9 do
      out[r][c] = board[r][c]
    end
  end
  return out
end

local function row_has(board, row, digit, skip_col)
  for c = 1, 9 do
    if c ~= skip_col and board[row][c] == digit then
      return true
    end
  end
  return false
end

local function col_has(board, col, digit, skip_row)
  for r = 1, 9 do
    if r ~= skip_row and board[r][col] == digit then
      return true
    end
  end
  return false
end

local function box_has(board, row, col, digit)
  local br = math.floor((row - 1) / 3) * 3 + 1
  local bc = math.floor((col - 1) / 3) * 3 + 1
  for r = br, br + 2 do
    for c = bc, bc + 2 do
      if not (r == row and c == col) and board[r][c] == digit then
        return true
      end
    end
  end
  return false
end

function M.is_valid_placement(board, row, col, digit)
  if digit == 0 then
    return true
  end
  if row_has(board, row, digit, col) then
    return false
  end
  if col_has(board, col, digit, row) then
    return false
  end
  if box_has(board, row, col, digit) then
    return false
  end
  return true
end

function M.is_complete(board)
  for r = 1, 9 do
    for c = 1, 9 do
      local v = board[r][c]
      if v == 0 or not M.is_valid_placement(board, r, c, v) then
        return false
      end
    end
  end
  return true
end

function M.get_conflicts(board)
  local conflicts = {}
  for r = 1, 9 do
    for c = 1, 9 do
      local v = board[r][c]
      if v ~= 0 then
        board[r][c] = 0
        if not M.is_valid_placement(board, r, c, v) then
          conflicts[r .. "," .. c] = true
        end
        board[r][c] = v
      end
    end
  end
  return conflicts
end

return M
