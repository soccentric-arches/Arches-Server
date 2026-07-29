---
description: Configure and build (native). Never raw cmake — the Makefile carries the flags.
allowed-tools: Bash(make build), Bash(make configure)
---

!`make build`

Report warnings as findings (the build is expected clean under -Werror). If
configure fails on a missing ../Arches-HAL, say so plainly — this repo links HAL
as a build-time sibling via add_subdirectory and cannot build without it.
