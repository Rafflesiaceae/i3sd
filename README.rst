i3sd
====

``i3sd`` is an event-driven Linux status generator for i3bar. Its native core owns the epoll reactor, bounded protocol buffers, timers, signals, reloads and collectors; presentation policy remains in Lua.

Build
-----

.. code:: console

   meson setup build
   meson compile -C build
   meson test -C build

The required development dependencies are LuaJIT, yyjson and xxHash.

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

   clock { format = "%H:%M" }
   memory { warn_below = 20, critical_below = 10 }

See ``PLAN.rst`` for the complete architecture, API contracts and delivery roadmap.
