# How to help

[Русский](CONTRIBUTING.md) · **English**

Thank you for looking in. The mod is young, and what it needs most right now is reports from real play.

## Report a bug

Open an [Issue](../../issues) and attach:

- `metronet.log` from the game folder — **from every PC** that was in the session. The log is made anew at
  every start of the game, so take it before you start the game again;
- what happened and on which level; who led the game and who joined; through Steam or by IP address;
- the mod's version (the first line of the log).

Look the log through before you publish it: it holds your Steam name and the game's folder. You may replace them.

## Propose a change

1. Build the mod (see "Building from source" in [README.en.md](README.en.md)).
2. Keep a change small and about one thing. Code style follows the neighbouring files; comments say "why".
3. Addresses inside `metro.exe` are right for the GOG build of September 2014 only (`src/mod/game.h`). A new
   address comes with a comment: what the function is and how it was found.
4. Test the change. The automatic lab (`tools/lab`) runs several copies of the game with no window on the
   screen, drives them with a scripted gamepad and compares their logs; it needs your own copy of the game next
   to the repository folder and your own saves. Without a lab, say in the request what you checked by hand.
5. Open a Pull Request that says what changes, why, and how it was tested.

## What must not be in the repository

- Files of the game and its data: the exe, archives, textures, sounds, saves.
- The Steamworks SDK and its libraries.
- Personal data: names, e-mail addresses, Steam names, paths with a user name, logs that hold them.
