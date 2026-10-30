![Logo](docs/logo-30.png) i3: A tiling window manager
=====================================================

[![Build Status](https://github.com/i3/i3/actions/workflows/main.yml/badge.svg)](https://github.com/i3/i3/actions/workflows/main.yml)
[![Issue Stats](https://img.shields.io/github/issues/i3/i3.svg)](https://github.com/i3/i3/issues)
[![Pull Request Stats](https://img.shields.io/github/issues-pr/i3/i3.svg)](https://github.com/i3/i3/pulls)

[![Packages](https://repology.org/badge/latest-versions/i3.svg)](https://repology.org/metapackage/i3/versions)
[![Packages](https://repology.org/badge/tiny-repos/i3.svg)](https://repology.org/metapackage/i3/versions)

i3 is a tiling window manager for X11.

For more information about i3, please see [the project's website](https://i3wm.org/) and [online documentation](https://i3wm.org/docs/).

For information about contributing to i3, please see [CONTRIBUTING.md](.github/CONTRIBUTING.md).

NOTES

My personal fork of [i3](https://github.com/i3/i3), a tiling window manager for X11.

This fork is kept for **my own use**. It follows upstream's `next` branch.  
Everything not listed below is upstream code; the notes here are mine and are not part of upstream's documentation.

## Personal use

This repo is for **my own setup**.  
It is not a general-purpose project, and I don’t guarantee it will work on your machine.  
Feel free to read/copy it, but expect to edit the source for your own system.

## What’s different

- Follows upstream `next` branch.
- Personal notes and small tweaks may be added over time.
- See the [commit history](https://github.com/kafudz/i3/commits/next) for exact changes.

## Build

Standard i3 build process:

```bash
git clone https://github.com/kafudz/i3.git
cd i3
mkdir build && cd build
meson setup ..
ninja
```
· This is a personal fork — no support, no guarantees.
· For general i3 usage, refer to the official website and docs.
