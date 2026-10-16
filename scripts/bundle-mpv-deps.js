#!/usr/bin/env node
/**
 * Copy the transitive dynamic-library closure of libmpv next to the staged
 * copy, so a machine without the system libmpv runtime can still load it.
 *
 * Denylist, not allowlist, on purpose: the engine dlopens libmpv with
 * RTLD_NOW, which resolves EVERY dependency up front — video stacks included,
 * even though the engine runs `video=no`. One missing soname fails the whole
 * dlopen and the engine drops to its no-mpv fallback, where playback still
 * works (Chromium media element) but the visualizer and native device
 * switching are dead — the "visualizer and output-device switching do not
 * work in mpv mode" report. An
 * allowlist of media libraries cannot express that closure; a denylist of
 * the glibc core can. Shipping a private libc would be actively dangerous,
 * and libstdc++/libgcc_s exist on any distro that can start Electron, so
 * those are the only exclusions.
 */
import { existsSync, copyFileSync, lstatSync, readlinkSync } from 'node:fs'
import { join, dirname, resolve } from 'node:path'
import { fileURLToPath } from 'node:url'
import { spawnSync } from 'node:child_process'
import process from 'node:process'
import console from 'node:console'

const EXCLUDED_SONAMES = [
  'linux-vdso',
  'ld-linux',
  'libc.so',
  'libm.so',
  'libpthread',
  'libdl.so',
  'librt.so',
  'libresolv',
  'libgcc_s',
  'libstdc++',
]

function resolveSymlinks(path) {
  let current = path
  while (lstatSync(current).isSymbolicLink()) {
    current = resolve(dirname(current), readlinkSync(current))
  }
  return current
}

/**
 * Stage every transitive dependency of `libmpvPath` into `targetDir`.
 * Idempotent: sonames already present in the target are skipped, so a
 * re-run after a system upgrade only fills gaps.
 *
 * Runs iteratively over libmpv and any newly staged libraries so the
 * entire transitive closure is captured.
 *
 * @returns {number} how many libraries were newly copied
 */
export function bundleMpvDeps(libmpvPath, targetDir) {
  if (process.platform !== 'linux' || !existsSync(libmpvPath)) return 0

  let totalStaged = 0
  let pass = 0
  const maxPasses = 5
  let filesToScan = [libmpvPath]

  while (filesToScan.length > 0 && pass < maxPasses) {
    pass++
    const newlyCopiedFiles = []
    const seenInPass = new Set()

    for (const file of filesToScan) {
      if (!existsSync(file)) continue
      const result = spawnSync('ldd', [file], {
        encoding: 'utf-8',
        env: { ...process.env, LD_LIBRARY_PATH: targetDir },
      })
      if (result.status !== 0 || !result.stdout) continue

      for (const line of result.stdout.split('\n')) {
        const match = /^(.+?)\s+=>\s+(.+?)(?:\s+\(0x[0-9a-f]+\))?$/.exec(line.trim())
        if (!match) continue
        const soname = match[1].trim()
        const location = match[2].trim()
        const stagedCopy = join(targetDir, soname)

        if (location === 'not found') {
          if (!existsSync(stagedCopy) && pass === 1) {
            console.warn(
              `[bundle-mpv-deps] WARNING: ${soname} is missing on this build machine — ` +
                'the packaged engine will fail to load libmpv on clean targets',
            )
          }
          continue
        }

        const path = location.split(' ')[0]
        if (!path || seenInPass.has(soname)) continue
        seenInPass.add(soname)
        if (EXCLUDED_SONAMES.some((prefix) => soname.startsWith(prefix))) continue
        if (!path.includes('/') || existsSync(stagedCopy)) continue
        if (!existsSync(path)) continue

        try {
          copyFileSync(resolveSymlinks(path), stagedCopy)
          newlyCopiedFiles.push(stagedCopy)
          totalStaged++
        } catch (err) {
          console.warn(`[bundle-mpv-deps] failed to copy ${path}: ${err}`)
        }
      }
    }

    filesToScan = newlyCopiedFiles
  }

  console.log(`[bundle-mpv-deps] staged ${totalStaged} dependencies -> ${targetDir}`)
  return totalStaged
}

if (process.argv[1] && fileURLToPath(import.meta.url) === resolve(process.argv[1])) {
  let [libmpvPath, targetDir] = process.argv.slice(2)
  if (!libmpvPath) {
    const repoRoot = resolve(dirname(fileURLToPath(import.meta.url)), '..')
    const candidate = join(repoRoot, 'build', 'libmpv', 'linux', 'libmpv.so.2')
    if (existsSync(candidate)) {
      libmpvPath = candidate
      targetDir = dirname(candidate)
    } else {
      console.error('Usage: node bundle-mpv-deps.js <libmpvPath> [targetDir]')
      process.exit(1)
    }
  } else if (!targetDir) {
    targetDir = dirname(resolve(libmpvPath))
  }
  bundleMpvDeps(resolve(libmpvPath), resolve(targetDir))
}
