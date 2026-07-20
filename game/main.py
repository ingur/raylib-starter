import raylib as rl
from vmath import vec2

# top level runs once at boot, the window is already open
sprite = rl.LoadTexture("assets/sprite.png")
pos = vec2(60, 60)
vel = vec2(220, 170)


# runs every frame, return True to quit
def update():
    global pos, vel
    dt = rl.GetFrameTime()
    pos = pos + vel * dt

    # bounce off the window edges
    if (pos.x < 0 and vel.x < 0) or (pos.x + sprite.width > rl.GetScreenWidth() and vel.x > 0):
        vel = vel.with_x(-vel.x)
    if (pos.y < 0 and vel.y < 0) or (pos.y + sprite.height > rl.GetScreenHeight() and vel.y > 0):
        vel = vel.with_y(-vel.y)

    rl.BeginDrawing()
    rl.ClearBackground(rl.RAYWHITE)
    rl.DrawTextureV(sprite, pos, rl.WHITE)
    rl.DrawText("edit game/main.py and save, the game reloads", 20, 20, 20, rl.DARKGRAY)
    rl.EndDrawing()
