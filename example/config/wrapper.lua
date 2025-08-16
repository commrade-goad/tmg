package.path = package.path .. ";./lib/?.lua;"
lib = require("lib")

if lib.colors.accent1 == lib.colors.t_n_red  or
   lib.colors.accent1 == lib.colors.t_d_red or
   lib.colors.accent1 == lib.colors.t_b_red then

    rgb = lib.num_to_rgb(lib.colors.t_n_red)
    hsl = lib.rgb_to_hsl(rgb)
    hsl.s = math.min(1, hsl.s + 0.1)
    hsl.l = math.min(1, hsl.l + 0.05)
    lib.colors.t_n_red = lib.rgb_to_num(lib.hsl_to_rgb(hsl))

    rgb_light = lib.num_to_rgb(lib.colors.t_b_red)
    hsl = lib.rgb_to_hsl(rgb_light)
    hsl.s = math.min(1, hsl.s + 0.1)
    hsl.l = math.min(1, hsl.l + 0.08)
    lib.colors.t_b_red = lib.rgb_to_num(lib.hsl_to_rgb(hsl))

    rgb_dark = lib.num_to_rgb(lib.colors.t_d_red)
    hsl = lib.rgb_to_hsl(rgb_dark)
    hsl.l = math.min(1, hsl.l + 0.1)
    hsl.s = math.min(1, hsl.s + 0.02)
    lib.colors.t_d_red = lib.rgb_to_num(lib.hsl_to_rgb(hsl))
end

return lib
