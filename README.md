# MSKman

A maze chase arcade game for the Linux desktop, written in C with SDL2.
Guide MSK through the maze, eat every dot, and dodge four ghosts that each
hunt in their own way. Energizers turn the ghosts blue so you can eat them,
and bonus fruit shows up twice per level.

All mazes, sprites and sounds are original: the graphics are drawn and the
audio is synthesized at runtime, so the game ships no asset files.

## Controls

| Key | Action |
| --- | --- |
| Arrow keys, WASD, HJKL | Move |
| Enter / Space | Start a game |
| P | Pause |
| M | Mute / unmute |
| F11 or Alt+Enter | Toggle fullscreen |
| Esc | Back to the title screen (quits from the title) |

Game controllers work too (D-pad or left stick, Start to pause).
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

## Options

```
mskman [--fullscreen] [--scale N] [--mute] [--version] [--help]
```

The high score is kept in `~/.local/share/mskman/highscore`.

## License

MIT, see [LICENSE](LICENSE).
