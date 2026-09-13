-- Finds the holes in every class: bytes no reflected property accounts for.
--
-- This is the example worth reading, because it is something no built-in emitter does and
-- it took eighty lines. Padding is where unreflected members hide -- native-only fields,
-- and in an obfuscated build, whatever the obfuscator moved. A dumper that can only print
-- what the engine already tells you is not much of a dumper; being able to ask a new
-- question of a finished dump, without rebuilding the tool, is the point of the plugin API.
--
-- Most gaps are ordinary alignment, so the report says which is which rather than
-- implying every hole is a finding. A gap is called alignment when it is smaller than the
-- alignment of whatever follows it and ends exactly on that boundary -- which is precisely
-- what a compiler inserts, and what a hidden member is not.

local function is_alignment(gap_start, next_offset, next_size)
  local align = next_size
  if align > 8 then align = 8 end
  if align < 1 then align = 1 end

  local gap = next_offset - gap_start
  return gap < align and next_offset % align == 0
end

local function holes(record)
  -- Properties are declared, not necessarily in offset order, and a bitfield shares a
  -- byte with its neighbours -- so what matters is the occupied span, not the count.
  local spans = {}
  for i = 1, #record.properties do
    local p = record.properties[i]
    spans[#spans + 1] = { first = p.offset, last = p.offset + math.max(p.size, 1) }
  end
  table.sort(spans, function(a, b) return a.first < b.first end)

  local found = {}
  local cursor = record.inherited_size

  for i = 1, #spans do
    local span = spans[i]
    if span.first > cursor then
      found[#found + 1] = {
        at = cursor,
        bytes = span.first - cursor,
        alignment = is_alignment(cursor, span.first, span.last - span.first),
      }
    end
    if span.last > cursor then cursor = span.last end
  end

  if record.size > cursor then
    found[#found + 1] = { at = cursor, bytes = record.size - cursor, trailing = true }
  end
  return found
end

local function emit(dump)
  local lines = {
    "# Unaccounted bytes",
    "",
    "Ranges inside a class that no reflected property covers.",
    "",
    "A gap is marked `align` when it is smaller than the alignment of the member that",
    "follows and ends on that boundary -- ordinary compiler padding. The ones without",
    "that mark are the interesting ones: bytes the reflection data does not explain.",
    "",
    "PLACEHOLDER_TOTALS",
    "",
  }

  local suspicious_types, suspicious_bytes, aligned_bytes = 0, 0, 0
  local packages = dump.packages

  for i = 1, #packages do
    local package = packages[i]
    local reported = {}

    for j = 1, #package.classes do
      local record = package.classes[j]
      local found = holes(record)

      local unexplained = 0
      for k = 1, #found do
        local hole = found[k]
        if hole.trailing or hole.alignment then
          aligned_bytes = aligned_bytes + hole.bytes
        else
          unexplained = unexplained + hole.bytes
        end
      end

      if unexplained > 0 then
        local parts = {}
        for k = 1, #found do
          local hole = found[k]
          local tag = ""
          if hole.trailing then tag = " trailing"
          elseif hole.alignment then tag = " align" end
          parts[#parts + 1] = string.format("0x%04X..0x%04X (%d%s)",
            hole.at, hole.at + hole.bytes, hole.bytes, tag)
        end
        reported[#reported + 1] = string.format("| `%s` | %d | %d | %s |",
          record.name, record.size, unexplained, table.concat(parts, ", "))
        suspicious_types = suspicious_types + 1
        suspicious_bytes = suspicious_bytes + unexplained
      end
    end

    if #reported > 0 then
      lines[#lines + 1] = "## " .. package.name
      lines[#lines + 1] = ""
      lines[#lines + 1] = "| Class | Size | Unexplained | Ranges |"
      lines[#lines + 1] = "|---|---|---|---|"
      for k = 1, #reported do lines[#lines + 1] = reported[k] end
      lines[#lines + 1] = ""
    end
  end

  for i = 1, #lines do
    if lines[i] == "PLACEHOLDER_TOTALS" then
      lines[i] = string.format(
        "**%d classes with %d unexplained bytes.** A further %d bytes across the dump are "
        .. "alignment or trailing padding and are not listed.",
        suspicious_types, suspicious_bytes, aligned_bytes)
      break
    end
  end

  zircon.write("padding.md", table.concat(lines, "\n"))
  zircon.log(string.format("padding: %d classes with %d unexplained bytes (%d bytes of "
                           .. "alignment ignored)",
                           suspicious_types, suspicious_bytes, aligned_bytes))
end

return {
  name = "padding",
  description = "classes with bytes no reflected property covers (Lua)",
  emit = emit,
}
