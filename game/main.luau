local rl = raylib

-- top level runs once at boot, the window is already open
local sprite = rl.LoadTexture("assets/sprite.png")
local pos = vector.create(60, 60, 0)
local vel = vector.create(220, 170, 0)

-- runs every frame, return true to quit
function update()
    local dt = rl.GetFrameTime()
    pos = pos + vel * dt

    -- bounce off the window edges, vector components are read only so rebuild
    if (pos.x < 0 and vel.x < 0) or (pos.x + sprite.width > rl.GetScreenWidth() and vel.x > 0) then
        vel = vector.create(-vel.x, vel.y, 0)
    end
    if (pos.y < 0 and vel.y < 0) or (pos.y + sprite.height > rl.GetScreenHeight() and vel.y > 0) then
        vel = vector.create(vel.x, -vel.y, 0)
    end

    rl.BeginDrawing()
    rl.ClearBackground(rl.RAYWHITE)
    rl.DrawTextureV(sprite, pos, rl.WHITE)
    rl.DrawText("with ./build.sh dev, edit game/main.luau and save, the box keeps going", 20, 20, 20, rl.DARKGRAY)
    rl.EndDrawing()
end

-- reload hooks, delete them to get a fresh boot on every reload
function before_reload()
    rl.UnloadTexture(sprite) -- reloads never free GPU resources
    return { pos = pos, vel = vel }
end

function after_reload(state)
    pos, vel = state.pos, state.vel
end
