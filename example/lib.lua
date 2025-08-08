local M = {}

package.path = package.path .. ';./?.lua'

M.colors = require('color-table')

--- Convert a number to a hex string in #RRGGBB format
function M.num_to_hex(num)
    return string.format("#%06X", num)
end

--- Convert a hex string (#RRGGBB or RRGGBB) to a number
function M.hex_to_num(hex)
    local clean = hex:gsub("^#", "")
    return tonumber(clean, 16)
end

-- String prefix
function M.strip_prefix(hex)
    return hex:gsub("^#", "")
end

local function clamp(v)
    if v < 0 then return 0 end
    if v > 255 then return 255 end
    return math.floor(v + 0.5)
end

-- number (0xRRGGBB) -> rgb table
function M.num_to_rgb(num)
    num = tonumber(num) or 0
    local r = math.floor(num / 65536) % 256
    local g = math.floor(num / 256) % 256
    local b = num % 256
    return { r = r, g = g, b = b }
end

-- rgb table -> number (0xRRGGBB)
function M.rgb_to_num(rgb)
    local r = clamp(rgb.r)
    local g = clamp(rgb.g)
    local b = clamp(rgb.b)
    return r * 65536 + g * 256 + b
end

-- rgb table -> hex string "#RRGGBB"
function M.rgb_to_hex(rgb)
    return string.format("#%02X%02X%02X", clamp(rgb.r), clamp(rgb.g), clamp(rgb.b))
end

-- hex string "#RRGGBB" or "RRGGBB" -> rgb table
function M.hex_to_rgb(hex)
    local clean = tostring(hex):gsub("^#", ""):gsub("^0x", "")
    assert(#clean == 6, "Hex color must be in RRGGBB format")
    return {
        r = tonumber(clean:sub(1,2), 16),
        g = tonumber(clean:sub(3,4), 16),
        b = tonumber(clean:sub(5,6), 16),
    }
end

-- Flip endianness (accepts number or hex string), returns uppercase hex without prefix
function M.to_little_endian(value)
    local hex
    if type(value) == "number" then
        hex = string.format("%06X", value)
    else
        hex = tostring(value):gsub("^0x", ""):gsub("^#", "")
    end
    if #hex % 2 == 1 then hex = "0" .. hex end
    local bytes = {}
    for i = 1, #hex, 2 do
        table.insert(bytes, 1, hex:sub(i, i+1))
    end
    return table.concat(bytes)
end

-- Convert RGB table {r,g,b} (0..255) to HSL {h,s,l}
function M.rgb_to_hsl(rgb)
    local r = rgb.r / 255
    local g = rgb.g / 255
    local b = rgb.b / 255

    local max = math.max(r, g, b)
    local min = math.min(r, g, b)
    local h, s, l

    l = (max + min) / 2

    if max == min then
        -- achromatic (grey)
        h = 0
        s = 0
    else
        local d = max - min
        s = (l > 0.5) and (d / (2 - max - min)) or (d / (max + min))

        if max == r then
            h = ((g - b) / d + (g < b and 6 or 0))
        elseif max == g then
            h = ((b - r) / d + 2)
        else
            h = ((r - g) / d + 4)
        end
        h = h / 6
    end

    return { h = h, s = s, l = l }
end

-- Convert HSL {h,s,l} to RGB table {r,g,b} (0..255)
function M.hsl_to_rgb(hsl)
    local function hue_to_rgb(p, q, t)
        if t < 0 then t = t + 1 end
        if t > 1 then t = t - 1 end
        if t < 1/6 then return p + (q - p) * 6 * t end
        if t < 1/2 then return q end
        if t < 2/3 then return p + (q - p) * (2/3 - t) * 6 end
        return p
    end

    local r, g, b

    if hsl.s == 0 then
        -- achromatic
        r, g, b = hsl.l, hsl.l, hsl.l
    else
        local q = hsl.l < 0.5 and (hsl.l * (1 + hsl.s)) or (hsl.l + hsl.s - hsl.l * hsl.s)
        local p = 2 * hsl.l - q
        r = hue_to_rgb(p, q, hsl.h + 1/3)
        g = hue_to_rgb(p, q, hsl.h)
        b = hue_to_rgb(p, q, hsl.h - 1/3)
    end

    return {
        r = math.floor(r * 255 + 0.5),
        g = math.floor(g * 255 + 0.5),
        b = math.floor(b * 255 + 0.5)
    }
end

function M.wrap_with(str, with)
    return string.format("%s%s%s", with, str, with)
end

return M
