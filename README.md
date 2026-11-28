# tikutests

Firmware tests for TikuOS, built into the firmware image by the main
repository's Makefile. This repository is cloned into `tests/` in the TikuOS
tree and is listed in its `.gitignore`.

```
git clone <url> tests        # from the TikuOS root
```

## Building a test

Tests are off by default. Turn them on with `TIKU_TESTS_ENABLE=1` and pick one
test:

```
make MCU=stm32n6 BOARD=stm32n6570_dk TIKU_TESTS_ENABLE=1 TEST_dk_dcmipp_dual=1 main.signed.bin
```

The selection comes from, lowest priority first:

1. `tests/tests.config`: copy `tests.config.example` and set `TEST_<name> = 1`.
   The file is ignored by git.
2. `TIKU_TESTS="name1 name2"` on the make line.
3. `TEST_<name>=1` on the make line.

Other targets:

- `make test-list` prints every test with its group, board and requirements.
- `make test-config` prints the resolved selection, sources and flags.

A misspelt test name is an error. With `TIKU_TESTS_ENABLE=0` nothing from this
directory is compiled, whatever `tests.config` says.

The old per-test flags (`TIKU_NPU_MEM_TEST_ENABLE=1` and the like) no longer
select tests; the Makefile stops with a message if one is set.

## One test per image

Every firmware test autostarts its own process, so an image holds one test plus
the tests it needs. The enabled tests must form one group, where two tests are
connected when one lists the other in `requires` or `with`:

| Group | Tests |
|---|---|
| DK images | each of the `dk_*` tests alone |
| memory | `npu_mem`, `npu_mem_negative` |
| embedded | `npu_embedded`, `npu_run`, `npu_resource`, `npu_vfs` |
| alone | `region`, `npu_preflight`, `npu_pool`, `npu_lifecycle` |

Inside the embedded group, `npu_run` needs `npu_embedded`. `npu_resource` and
`npu_vfs` link to the group only through `npu_embedded`: with embedded on,
the resource checks run inside the embedded test and vfs takes over its
autostart.

Enabling unrelated tests stops the build, naming the two sides. There is no
override.

The same rules are checked a second time by the C preprocessor in
`tiku_test_config.h`, which every firmware test includes, so a build that
bypasses `tests.mk` (for example `EXTRA_CFLAGS=-D...`) is caught too.

## Tests, apps and other images

An image holds one owner of the autostart processes. With `TIKU_TESTS_ENABLE=1`:

- `apps/apps.mk` is not included, so no app is built.
- These stop the build with a message naming the flag: `HAS_TESTS=1` (the
  TikuBench tests), `APP=<name>`, `DEMO=<name>`, and
  `TIKU_STM32N6_DK_EDGE_DETECT_ENABLE=1`.
- Enabling tests but selecting none is an error (`make test-list` and
  `make test-config` still work).

The checks read values given on the make line or in the environment. A makefile
included later that sets `HAS_TESTS` is not seen.

## Parameters

Set in `tests.config` or on the make line. A parameter reaches the compiler as
`-D<name>=<value>` only when it is set.

| Parameter | Test | Values |
|---|---|---|
| `PIPE2_TEST_VARIANT` | `dk_dcmipp_pipe2_640` | 0, 1, 2 (default 0) |
| `DUAL_P1_FORMAT` | `dk_dcmipp_dual` | 565, 888 (default 565) |
| `DUAL_CYCLES`, `DUAL_MAD_LIMIT` | `dk_dcmipp_dual` | test defaults |
| `TFT_COLOR_ROTATION_SECONDS` | `dk_tft_colorcycle` | test default |

## Adding a test

1. Add the source file here. Put `#include "tiku_test_config.h"` before the
   test's `#if <flag>` guard, and keep a board `#error` if the test is
   board-specific.
2. In `tests.mk`, add the name to `TESTS_ALL` and give it a `T.<name>.*` block:
   `srcs`, `macro` (the `-D` flag, which is also the guard in the source),
   and as needed `board`, `group`, `requires`, `with`, `needs`, `vars`, `note`.
3. Add `TEST_<name> = 0` to `tests.config.example`.
4. In `tiku_test_config.h`, add the macro to `TIKU_TEST_GROUPS_`, and a
   `requires` check if it has one.

`tests.mk` and `tiku_test_config.h` encode the same rules; keep them in step.

## Host tests

`*_host_test.c` and `tiku_stm32n6570_imx335_test.c` are built for the host,
not into the firmware. `make lint` in the TikuOS tree runs them and skips them
when this directory is absent.

## Files

| File | Purpose |
|---|---|
| `tests.mk` | Registry, selection, validation, `test-list`, `test-config` |
| `tests.config.example` | Template for `tests.config` |
| `tiku_test_config.h` | Compile-time checks on the enabled set |
| `tiku_*_acceptance.c`, `tiku_stm32n6_dk_*.c` | Firmware tests |
| `npu/fixtures/` | NPU test fixtures |
