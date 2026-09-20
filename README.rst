i3sd
====

``i3sd`` is an event-driven Linux status generator for i3bar. Its native core owns the epoll reactor, bounded protocol buffers, timers, signals, reloads and collectors; presentation policy remains in Lua.

Build
-----

.. code:: console

   meson setup build
   meson compile -C build
   meson test -C build

The required development dependencies are LuaJIT, yyjson, xxHash and libsystemd.
PipeWire development headers are optional and enable the event-driven default
sink volume module.

Configuration
-------------

The default configuration is ``${XDG_CONFIG_HOME:-$HOME/.config}/i3sd.lua``. A minimal low-level configuration is:

.. code:: lua

   block {
       name = "clock",
       interval = 1,
       update = function(ctx)
           ctx:set { full_text = os.date("%H:%M:%S") }
       end,
   }

Validate it without activating runtime sources or writing the i3bar protocol:

.. code:: console

   i3sd --check -c ~/.config/i3sd.lua

The high-level Lua modules can be loaded explicitly:

.. code:: lua

   local clock = require("i3sd.modules.clock")
   local battery = require("i3sd.modules.battery")
   local memory = require("i3sd.modules.memory")
   local pipewire_volume = require("i3sd.modules.pipewire_volume")
   local power_profiles = require("i3sd.modules.power_profiles")
   local systemd = require("i3sd.modules.systemd")

   clock { format = "%H:%M" }
   battery { show = "auto" }
   memory { warn_below = 20, critical_below = 10 }
   pipewire_volume {}
   power_profiles {}
   systemd { scope = "both" }

Copyable configurations are available under ``examples/``. See ``PLAN.rst`` for the complete architecture, API contracts and delivery roadmap.

``pipewire_volume`` follows the current default audio sink and updates without
polling. It displays one of ``▏ ▎ ▍ ▌ ▋ ▊ ▉ █`` for the volume, ``🔇`` while
muted, and remains hidden while PipeWire or a default sink is unavailable.

Lua modules can start one bounded asynchronous child process from a block
callback. ``ctx:spawn`` returns ``nil`` during staging, when another child is
active, or when process setup fails:

.. code:: lua

   local process = ctx:spawn({
       argv = { "example-command", "--mode", "menu" },
       stdin = "first\nsecond\n",
       stdout_limit = 1024,
   }, function(process_ctx, result)
       if result.success then
           process_ctx:set { full_text = result.stdout }
       end
   end)

The returned handle has an idempotent ``cancel()`` method. The
``power_profiles`` module uses this primitive to implement its left-click rofi
selector entirely in Lua. Install ``rofi`` when using that module.

The ``battery`` module refreshes immediately on Linux power-supply change
events and keeps a slower periodic sample as a resynchronization fallback.

D-Bus module API
----------------

Lua modules can lazily acquire the shared system or user bus and use it without
blocking the status loop:

.. code:: lua

   local bus = ctx:dbus("user")
   local match = bus:match({
       sender = "org.example.Service",
       path = "/org/example/Object",
       interface = "org.example.Interface",
       member = "Changed",
   }, function(message)
       local name, value = message:read("su")
       ctx:set { full_text = name .. ": " .. value }
   end, function(error)
       assert(error == nil, error and error.message)
   end)

   local call = bus:call({
       destination = "org.example.Service",
       path = "/org/example/Object",
       interface = "org.example.Interface",
       member = "GetState",
       signature = "",
       args = {},
       timeout = 5,
   }, function(reply, error)
       if error == nil then
           local state = reply:read("s")
           ctx:set { full_text = state }
       end
   end)

``bus:on_connect(callback)`` reports every connection epoch so modules can redo
connection-scoped initialization after a reconnect. All three operations return
cancellable handles owned by the block generation; retain a handle while its
operation should stay active. Calls are sent only after the staged configuration
becomes live, are never synchronously waited for, and are not replayed after a
disconnect.

Signatures are explicit. Basic values, arrays, structs, ordered dictionary pair
sequences, and variants are supported. ``ay`` is represented as a binary Lua
string, and ``x``/``t`` decode to exact LuaJIT ``int64_t``/``uint64_t`` cdata.
Use ``i3sd.dbus.dict(entries)`` to validate a dictionary pair sequence and
``i3sd.dbus.variant(signature, value)`` to create a variant. Message objects are
valid only for the duration of their callback, while values returned by
``message:read(signature)`` are ordinary owned Lua values.
