# Flight Game — how to run it

A little flight game built on the [raytiles](https://github.com/ziv/raytiles) engine: fly a
Star Wars ship over the real Mount Everest, race through checkpoint courses against the clock,
and shoot targets. Built and tested on macOS.

## One-time setup (about 10 minutes)

You need a Mac and a few free developer tools. Open **Terminal** and run:

1. Apple's command-line tools (compiler + git):
   ```
   xcode-select --install
   ```
2. Homebrew (skip if you already have it): see https://brew.sh
3. The build tools the game needs:
   ```
   brew install cmake openssl@3
   ```

## Get the game and run it

```
git clone <REPO_URL>
cd raytiles
./fly.command
```

- The **first** run compiles the game (it downloads the graphics library automatically) —
  this takes a few minutes and needs an internet connection.
- After that, just run `./fly.command` again any time — it starts in a few seconds.
- The game streams real satellite map tiles, so you need to be **online** while playing.

If double-clicking `fly.command` in Finder is blocked by macOS, right-click it → **Open** →
**Open** (only needed once), or run `./fly.command` from Terminal.

## Controls

- **Arrows** — pitch / roll, **Q / E** — yaw
- **D / A** — speed up / slow down
- **S** — fire lasers
- **M** — change mode: Cruise · Competition (timed 7-gate race) · Infinite · Target Course
- **G** — start / restart the current mode
- **C** — change ship, **P** — fullscreen, **R** — reset after a crash
- **Esc** — quit

## Notes
- Works on any modern Mac (it builds a binary for whatever chip you have).
- Ship models are CC-BY (credits in `res/*/license.txt`); sounds are from freesound.org.
