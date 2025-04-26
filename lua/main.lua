rl.SetConfigFlags(rl.FLAG_VSYNC_HINT)

rl.InitWindow(800, 450, "basic window")

while not rl.WindowShouldClose() do
	rl.BeginDrawing()

	rl.ClearBackground(rl.RAYWHITE)
	rl.DrawText("Congrats! You created your first window!", 190, 200, 20, rl.LIGHTGRAY)

	rl.EndDrawing()
end

rl.CloseWindow()
