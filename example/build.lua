-- replaces apply.sh: one process, one Lua loop, instead of six tmg spawns.
local home = os.getenv("HOME")

local targets = {
    { "config/hypr-color.tmg",      home .. "/.config/hypr/colorscheme.conf" },
    { "config/rofi-color.tmg",      home .. "/.config/rofi/sel.rasi" },
    { "config/dunst.tmg",           home .. "/.config/dunst/dunstrc" },
    { "config/eww-color.tmg",       home .. "/.config/eww/eww.scss" },
    { "config/nvim-color.tmg",      home .. "/.config/nvim/lua/colors/custom.lua" },
    { "config/alacritty-color.tmg", home .. "/.config/alacritty/sel.toml" },
}

for _, t in ipairs(targets) do
    local ok = tmg.render(t[1], t[2])
    if not ok then
        io.stderr:write("failed to render " .. t[1] .. "\n")
        os.exit(1)
    end
end

os.execute("dunstctl reload")
