-- Startup settings, read before the window is created. Keep it to plain values.
-- Title, size and fps also apply on reload, the rest needs a restart.
-- This module returns the table below, so `require("window")` hands the same
-- table back to your game.

return {
    -- text in the title bar
    title = "game",

    -- window size in pixels
    size = { 960, 540 },

    -- frame rate cap, 0 for uncapped
    fps = 60,

    -- sync to the monitor refresh rate
    vsync = true,

    -- let the user resize the window
    resizable = false,

    -- 4x antialiasing for shapes
    msaa = false,

    -- start in fullscreen
    fullscreen = false,
}
