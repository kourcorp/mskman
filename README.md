# MSKman

A maze chase arcade game for the Linux desktop, written in C with SDL2.
Guide MSK through the maze, eat every dot, and dodge four ghosts that each
hunt in their own way. Energizers turn the ghosts blue so you can eat them,
and bonus fruit shows up twice per level. The first two levels use
hand-made mazes; after that every level is played on a freshly generated
maze (symmetric, no dead ends, fully connected).

Up to four players can play, taking turns like the arcade: a turn ends
when the player loses a life, and each player keeps their own score,
lives, level and maze.

All mazes, sprites and sounds are original: the graphics are drawn and the
audio is synthesized at runtime, so the game ships no asset files.

## Controls

| Key | Action |
| --- | --- |
| Arrow keys, WASD, HJKL | Move |
| Enter / Space | Start a game |
| Left / Right or 1-4 (title screen) | Choose the number of players |
| O (title screen) | Options |
| P | Pause |
| M | Mute / unmute |
| F11 or Alt+Enter | Toggle fullscreen |
| Esc | Back to the title screen (quits from the title) |

Game controllers work too (D-pad or left stick, Start to pause, Y for
options, B to go back).
The window can be resized freely; the picture scales to fit.

## Install a package

Download the `.deb` or `.rpm` from the
[Releases](https://github.com/kourcorp/mskman/releases) page (or from the
artifacts of the latest *Build packages* run), then:

```sh
sudo apt install ./mskman_*.deb        # Debian, Ubuntu, Mint
sudo dnf install ./mskman-*.x86_64.rpm # Fedora, RHEL-likes
```

MSKman then appears in your desktop's application menu under Games.

## Build from source

Needs a C compiler, `make`, `pkg-config` and the SDL2 development files
(`libsdl2-dev` on Debian/Ubuntu, `SDL2-devel` on Fedora).

```sh
make
./mskman
make check                          # headless smoke test
sudo make install PREFIX=/usr/local # optional
```

## Building packages locally

```sh
dpkg-buildpackage -us -uc -b   # .deb lands in the parent directory

make dist && mkdir -p ~/rpmbuild/SOURCES && cp mskman-*.tar.gz ~/rpmbuild/SOURCES/
rpmbuild -ba packaging/rpm/mskman.spec
```

CI builds both packages on every push. Pushing a tag such as `v0.1.0`
publishes them as a GitHub release.

## Control tuning

Press **O** on the title screen to open the options:

| Setting | What it does | Default |
| --- | --- | --- |
| Corner assist | Accepts a turn up to this many pixels before or after a junction, then slides MSK onto the new lane (0 = off, 4 = max) | 3 px |
| MSK speed | Player speed, 50-150% of normal | 100% |
| Ghost speed | Ghost speed, 50-150% of normal | 100% |

Settings are saved to `~/.config/mskman/settings.conf` (or
`$XDG_CONFIG_HOME/mskman/settings.conf`), which can also be edited by hand.

## Command line

```
mskman [--fullscreen] [--scale N] [--mute] [--players N] [--version] [--help]
```

The high score is kept in `~/.local/share/mskman/highscore`.

## License

MIT, see [LICENSE](LICENSE).
