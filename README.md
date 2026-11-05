<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
<!-- Copyright (C) Opinsys Oy 2026 -->

# puu-install

`puu-install` is a standalone installer for bootc based operating systems.

## Deployment configuration

A deployment configuration must be provided either via `--config-path` or
`PUU_INSTALL_CONFIG`.

This is the default configuration:

```yaml
branding:
  name: Linux
partitions:
  efi: 512M
  rootMin: 64G
  home: 256G
labels:
  root: root
  swap: swap
  home: home
image:
  targetImgRef: ""         # required unless provided by image.manifest
  expectedDigest: ""       # sha256 of raw OCI manifest for local sources
  sourceLabel: PAYLOAD
  sourceMount: /run/puu-install/payload
identity:
  keymap: us
  locale: C.UTF-8
  timezone: UTC
  hostname: localhost
homed:
  groups: []
  identityPath: /var/lib/firstboot/homed-identity.json
  sessionLauncher: ""      # omitted from user record when empty
seeds: []                  # list of {name, target, liveStore, syncHelper, manifestPath}
k3s: null                  # optional {role, serverUrl, nodeName}
kernelArgs: []
serialConsole: {}          # map of machine name -> console
```
Optional K3s token must be passed via `--k3s-token-file`.

## Development

For native development on Linux:

- Install the build dependencies listed in `Dockerfile`.
- Install `abseil-cpp` for foundational C++ utilities:
  - Debian/Ubuntu: `libabsl-dev`.
  - Fedora: `abseil-cpp-devel`.
- Follow [Google C++ Style Guide](https://google.github.io/styleguide/cppguide.html).
- Vendor dependencies other than Abseil and the existing platform libraries with
  [FetchContent](https://cmake.org/cmake/help/latest/module/FetchContent.html),
  pinned to hash-checked upstream release tarballs.

## Licensing

`puu-install` has been licensed with the GNU General Public License 2.0, or a
newer version. Study [`LICENSE`](LICENSE) for further details.
