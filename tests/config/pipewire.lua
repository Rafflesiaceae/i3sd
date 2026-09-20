local pipewire_volume = require("i3sd.modules.pipewire_volume")

pipewire_volume {
    order = 1,
    key = "default-sink",
}
