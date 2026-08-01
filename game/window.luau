-- read before the window is created, so keep it to plain values
-- title, size and fps also apply on reload, the rest needs a restart
-- require("window") hands this same table back to your game

return {
    title = "game",
    size = { 960, 540 },
    fps = 60,  -- 0 for uncapped
    vsync = true,
    resizable = false,
    msaa = false,
    fullscreen = false,
}
