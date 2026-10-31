-- A small class
local Account = {}
Account.__index = Account

local MAX_BALANCE = 1e6

function Account.new(owner, balance)
  local self = setmetatable({}, Account)
  self.owner = owner
  self.balance = balance or 0
  return self
end

function Account:deposit(v)
  if v <= 0 or self.balance + v > MAX_BALANCE then
    error("invalid amount: " .. tostring(v))
  end
  self.balance = self.balance + v
end

for i, name in ipairs({ "ann", "bob" }) do
  local a = Account.new(name, i * 10)
  a:deposit(5.5)
  print(a.owner, a.balance, nil, true)
end
