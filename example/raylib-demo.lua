-- Basic raylib window from tmg (uses lib/raylib.lua + the raylib-luajit-generated submodule).
--
--   git submodule update --init
--   TMG_LIB_PATH=/path/to/tmg/lib:/dir/with/libraylib tmg raylib-demo.lua
--
-- /dir/with/libraylib must contain libraylib.so (and libphysac.so for physac).
local rl = require("raylib")

rl.SetConfigFlags(rl.FLAG_VSYNC_HINT)
rl.InitWindow(800, 450, "tmg + raylib")
rl.SetTargetFPS(60)

while not rl.WindowShouldClose() do
  rl.BeginDrawing()
  rl.ClearBackground(rl.RAYWHITE)
  rl.DrawText("Hello from tmg!", 190, 200, 20, rl.DARKGRAY)
  rl.EndDrawing()
end

rl.CloseWindow()
