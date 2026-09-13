-- Every property in the dump as one CSV row.
--
-- The shortest useful thing a Zircon emitter can be, and a reasonable template: walk
-- packages, walk the types in them, read fields off nodes as if they were tables, call
-- zircon.write once at the end.
--
-- Nothing here is a copy of the dump. `dump.packages` is a view; indexing it calls back
-- into the host, so a script that only looks at names never pays for 48000 properties.

local function quote(text)
  if text:find('[",\n]') then
    return '"' .. text:gsub('"', '""') .. '"'
  end
  return text
end

local function render_type(t)
  -- `name` is the referenced enum/struct/class and is the useful half whenever it is
  -- there. An enum also carries its underlying integer type in params, so checking
  -- params first would print "enum<uint8>" and throw the name away.
  if t.name ~= "" then return t.name end
  if #t.params == 0 then return t.kind end

  local parts = {}
  for i = 1, #t.params do parts[#parts + 1] = render_type(t.params[i]) end
  return t.kind .. "<" .. table.concat(parts, ", ") .. ">"
end

local function emit(dump)
  local rows = { "package,type,kind,member,offset,size,type_name,bitfield" }

  local function add_type(package_name, record, kind)
    for i = 1, #record.properties do
      local p = record.properties[i]
      rows[#rows + 1] = table.concat({
        quote(package_name),
        quote(record.name),
        kind,
        quote(p.name),
        string.format("0x%04X", p.offset),
        tostring(p.size),
        quote(render_type(p.type)),
        p.is_bitfield and string.format("0x%02X", p.field_mask) or "",
      }, ",")
    end
  end

  local packages = dump.packages
  for i = 1, #packages do
    local package = packages[i]
    for j = 1, #package.classes do add_type(package.name, package.classes[j], "class") end
    for j = 1, #package.structs do add_type(package.name, package.structs[j], "struct") end
  end

  rows[#rows + 1] = ""
  zircon.write("properties.csv", table.concat(rows, "\n"))
  zircon.log(string.format("csv: %d property rows", #rows - 2))
end

return {
  name = "csv",
  description = "every property as a CSV row (Lua)",
  emit = emit,
}
