import raylib as rl
from vmath import vec2
import json

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
    rl.DrawText("edit game/main.py and save, the game reloads, the box keeps going", 20, 20, 20, rl.DARKGRAY)
    rl.EndDrawing()


# dev only: hot reload hooks, delete them to get a fresh boot on every save
def before_reload():
    rl.UnloadTexture(sprite)  # reloads never free GPU resources
    return json.dumps([pos.x, pos.y, vel.x, vel.y])


def after_reload(state):
    global pos, vel
    x, y, vx, vy = json.loads(state)
    pos = vec2(x, y)
    vel = vec2(vx, vy)
