# Trying plugin windows with real audio

A looping test sound (`test_loop.wav`: drums over a quiet noise bed, -12 dBFS, made by `make_test_loop.py`) on its own bus, **Plugins**, to watch plugin GUIs with signal running.

1. Open `demo/` in the Godot editor, and open `gui_test/gui_test.tscn`.
2. Select the **TestLoop** node and tick **Playing** in the inspector: the loop plays in the editor, through the Plugins bus. (Turn your volume down first.)
3. Open the **Audio** panel at the bottom. On the **Plugins** bus, **Add Effect** and pick an installed plugin with a meter or a spectrum display, such as a compressor or an analyser.
4. Click the effect on the bus: the plugin's window opens, with its display moving.

Things to check:

| # | do | expect |
| --- | --- | --- |
| 1 | close the window with its X | it closes; clicking the effect again reopens it |
| 2 | look at the meter or spectrum | moving with the loop: the GUI is on the live instance |
| 3 | click the effect while **Playing** is off, then tick it | the GUI moves to the live instance |
| 4 | add a second effect, or reorder, while the window is open | Godot rebuilds the bus; the window stays, the GUI comes back in it |
| 5 | click another node, then the effect again | the window stays open throughout (and is raised) |

The same works with any CLAP or VST3 plugin in the Add Effect menu; one without a moving display shows checks 1, 4 and 5.
