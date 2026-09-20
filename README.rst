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
   local memory = require("i3sd.modules.memory")
   local power_profiles = require("i3sd.modules.power_profiles")
   local systemd = require("i3sd.modules.systemd")

   clock { format = "%H:%M" }
   memory { warn_below = 20, critical_below = 10 }
   power_profiles {}
   systemd { scope = "both" }

Copyable configurations are available under ``examples/``. See ``PLAN.rst`` for the complete architecture, API contracts and delivery roadmap.

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
