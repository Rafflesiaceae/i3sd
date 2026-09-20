local pipewire_volume = require("i3sd.modules.pipewire_volume")

-- The block updates immediately when the default sink, volume, or mute state
-- changes; no polling interval is involved.
pipewire_volume {
    order = 10,
}
