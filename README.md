# **Libretro notice** #

Send bugs or issues related to the core's functionality here on this repo.
Report emulation issues on the upstream MAME repo.

# Theseus-CDi

## Why? MAME already exists.

MAME's way of loading commercial games on frontends like Libretro was... complicated. You had to get
hash files, dummy zip archives, and CHDs, just like any arcade ROM. Theseus-CDi, much like its predecessor,
SAME_CDi, simplifies the loading of games to make it feel like a regular console emulator.

## What happened to SAME_CDi?

In a way, SAME_CDi was pretty much hard to maintain, as it was based on an archaic MAME framework.
I did a painstaking effort of backporting code from MAME 0.287 to 0.22x(!), all by hand with no AI.
Definitely an annoying trial-and-error process.
Theseus-CDi aims for ease of maintainability, unlike SAME_CDi, which had stayed on its ancient
framework since 2022.

### What games are supported?

Virtually every game is supported, even the ones relying on the mythical DVC peripheral, which went
unemulated for 30 years on an open-source emulator.
Practically, though, I've only tested Tetris, Atlantis: The Last Resort, and Hotel Mario.

### I want to compile this. How do I do that?

It's fairly simple. If you're on an UNIX-like system (like Linux), simply run the following:

```
make -f Makefile.libretro SUBTARGET=cdi
```

On a Windows system, you can use a compiler environment like MSYS2.

### AI disclosure

Yes, you probably saw this coming from an up-and-coming coder in this day and age. It was
simply inevitable.

GPT-6 Astra helped me with backporting the CD-i drivers. They didn't work as-is: they used
a modern version of screen.h API calls that had fewer arguments. I couldn't figure out how
those API calls even worked, so I had to ask the AI for help in that regard. MAME is one
behemoth of an emulator, and I couldn't have learned the ins and outs of the emulator in a
week.

If you choose to not use this emulator, good. If you're going to complain about me using AI,
why don't you make an alternative yourself? Maybe you have better ideas than me, and maybe
you do a better job than I did. Prove yourself, don't just whine about it.

## License

(Figured I'd leave this stuff here.)

The MAME project as a whole is made available under the terms of the
[GNU General Public License, version 2](http://opensource.org/licenses/GPL-2.0)
or later (GPL-2.0+), since it contains code made available under multiple
GPL-compatible licenses.  A great majority of the source files (over 90%
including core files) are made available under the terms of the
[3-clause BSD License](http://opensource.org/licenses/BSD-3-Clause), and we
would encourage new contributors to make their contributions available under the
terms of this license.

Please note that MAME is a registered trademark of Gregory Ember, and permission
is required to use the "MAME" name, logo, or wordmark.

<a href="http://opensource.org/licenses/GPL-2.0" target="_blank">
<img align="right" width="100" src="https://opensource.org/wp-content/uploads/2009/06/OSIApproved.svg">
</a>

    Copyright (c) 1997-2026  MAMEdev and contributors

    This program is free software; you can redistribute it and/or modify it
    under the terms of the GNU General Public License version 2, as provided in
    docs/legal/GPL-2.0.

    This program is distributed in the hope that it will be useful, but WITHOUT
    ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
    FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
    more details.

Please see [COPYING](COPYING) for more details.
