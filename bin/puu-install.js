#!/usr/bin/env node
// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) Opinsys Oy 2026

const { execFileSync } = require('node:child_process');
const fs = require('node:fs');
const path = require('node:path');

function resolveBinary() {
  const platform = `${process.platform}-${process.arch}`;
  const packages = {
    'linux-arm64': '@puu/install-linux-arm64',
    'linux-x64': '@puu/install-linux-x64',
  };
  const packageName = packages[platform];
  if (!packageName) {
    throw new Error(
      `unsupported platform: ${platform}; supported platforms: ${Object.keys(packages).join(', ')}`
    );
  }

  let packageDir;
  try {
    packageDir = path.dirname(
      require.resolve(`${packageName}/package.json`, { paths: [__dirname] })
    );
  } catch {
    const devPath = path.join(__dirname, '..', 'npm', platform);
    if (fs.existsSync(path.join(devPath, 'package.json'))) {
      packageDir = devPath;
    } else {
      throw new Error(
        `package ${packageName} is not installed; ` +
          'install the optional platform-specific dependency'
      );
    }
  }

  const imagePath = path.join(packageDir, 'bin', 'puu-install.AppImage');
  if (!fs.existsSync(imagePath)) {
    throw new Error(`AppImage not found: ${imagePath}`);
  }

  return imagePath;
}

try {
  execFileSync(resolveBinary(), process.argv.slice(2), {
    stdio: 'inherit',
    env: { ...process.env, APPIMAGE_EXTRACT_AND_RUN: '1' },
  });
} catch (err) {
  if (typeof err.status === 'number') {
    process.exit(err.status);
  }
  if (err.signal) {
    process.kill(process.pid, err.signal);
  }
  console.error(err.message);
  process.exit(1);
}
