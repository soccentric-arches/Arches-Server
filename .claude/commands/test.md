---
description: Run the label-filtered CTest suite. NEVER bare ctest here.
allowed-tools: Bash(make test)
---

!`make test`

`make test` applies the repo's ctest label filter. A bare `ctest` FAILS in this
repo: HAL's enable_testing() registers ~36 HAL test executables that are not
built downstream (EXCLUDE_FROM_ALL), so an unfiltered run errors on missing
binaries and looks like a code failure when it isn't. Report failures with the
test name and the relevant output lines, not a summary.
