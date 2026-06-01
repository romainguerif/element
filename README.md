[![REUSE status](https://api.reuse.software/badge/github.com/kushview/element)](https://api.reuse.software/info/github.com/kushview/element) [![Build and Test](https://github.com/kushview/element/actions/workflows/build.yml/badge.svg)](https://github.com/kushview/element/actions/workflows/build.yml) [![Generate Lua Documentation](https://github.com/kushview/element/actions/workflows/ldoc.yml/badge.svg)](https://github.com/kushview/element/actions/workflows/ldoc.yml)

# Element
![Element Screenshot](data/screenshot.png)

### ADVANCED AUDIO PLUGIN HOST
This is the community version of Element, a modular AU/LV2/VST/VST3/CLAP audio plugin host. Create powerful effects, racks and instruments by connecting nodes to one another. Integrates with your existing hardware via standard protocols such as MIDI. See [kushview.net](https://kushview.net/element/) for more information and [pre-built binaries](https://kushview.net/element/download/). 

_See also:_ [Element User Manual](https://element.readthedocs.io)

---

## About this fork

This is a personal fork of Element where I'm extending the host with a set of
performance- and automation-oriented features. All of the work below was
designed and implemented **with [Claude Code](https://www.anthropic.com/claude-code)**,
Anthropic's agentic coding tool — I drove the design decisions and Claude Code
carried out the exploration, implementation, and build/verify loop.

### What I added

**Parameter Mapper automation lane.** A Serato-style automation lane in the
bottom panel that draws and edits an automation curve for each of the Parameter
Mapper's 64 slots. Free time placement (no forced grid), per-segment curvature
(alt-drag to bend), time/bars ruler, zoom and pan. Curves are evaluated on the
audio thread in the node's `render()` and pushed to the mapped parameters, so a
single mapper can sequence dozens of plugin/mixer parameters over a timeline.
Everything is decoupled from the UI by a short try-lock so audio stays
click-free.

**Snapshot morphing — automated, with magnetic snap.** The Parameter Mapper can
store full-state snapshots; this fork lets you *automate* them. A reserved
"Snapshot Morph" lane holds its own curve whose value cross-fades **all 64
slots** across the recorded snapshots over time (0 = first snapshot, 1 = last,
fractions blend the two nearest). It reuses the same automation machinery as the
slots — the morph is just one extra lane — and per-slot automation still
overrides individual slots. On that lane only, horizontal reference lines mark
each snapshot level (S1…Sn) and an optional **magnetic snap** locks points onto
a level so you can build clean step-plateaus, while placing points between levels
stays smooth. A "Snap" toggle turns the magnetism off for fully free editing.

**Audio mixer is fully mappable.** Every per-channel control (gain, pan, EQ,
filter, drive, transient, sends, mute/solo/cue) plus the master and return
strips are exposed as bridge `AudioProcessorParameter`s, so the Parameter Mapper
(and any host automation) can learn and drive them. The mixer editor and the
mapper stay visually in sync in both directions: moving a knob in either place
updates the other, with drag guards to avoid feedback loops, and gain/pan are
ramped per block for smooth, glitch-free automation.

**Tab node search (Spotlight-style).** Press <kbd>Tab</kbd> to open a centred
search overlay — type a node or plugin name and press <kbd>Return</kbd> to drop
the match into the active graph. Fuzzy ranking (prefix > word-boundary >
contains), arrow-key navigation, click-outside to dismiss. It searches the same
catalog the right-click "Plugins" menu uses, so anything you can add is findable.

**Collapsible navigation zone.** The left session/navigation panel collapses to a
thin chevron handle to reclaim screen space — toggle with <kbd>Cmd</kbd>+<kbd>B</kbd>,
from the View menu, or by clicking the handle. The state is persisted.

> These additions live in the `feat/robust-pdc` branch and target macOS / Apple
> Silicon builds.

---

### Compatibility
Element currently loads most major plugin formats.

| OS       | Version       | Formats         |
| -------- |:-------------:| ---------------:|
| Linux*   |       -       | LADSPA/LV2/VST3/CLAP |
| Mac OSX  | 10.8 and up   | AU/VST/VST3/LV2/CLAP |
| Windows  | XP and up     | VST/VST3/LV2/CLAP    |

_*Ubuntu is the most tested, but should run on any major distribution_

### Features
* Runs standalone or as a plugin in your DAW**
* Route Audio and MIDI from anywhere to anywhere
* Play virtual instruments and effects live
* Create re-usable instruments and effect graphs
* External Sync with MIDI Clock
* Sub Graphing – Nest Graphs within each other
* Custom Keyboard Shortcuts
* Placeholder Nodes
* Built In Virtual Keyboard
* Multiple Undo/Redo
* Scripting - Custom DSP and DSP UI's
* Embed plugin UIs directly in Graphs
* And more...

### Building 
See [building.md](./docs/building.md) for instructions and dependency details.

### Contributing
If you'd like to contribute code please review the [code style](./docs/cppstyle.md) and [contributor notes](CONTRIBUTING.md) before submitting pull requests.  You may also want to join the [#element](https://discord.gg/fAsQ5fMuHy) channel on the Kushview [Discord](https://discord.gg/fAsQ5fMuHy) server.

### Issue Reporting
Please report bugs and feature requests on Gitlab. [Element issue tracker](https://gitlab.com/kushview/element/-/issues).
