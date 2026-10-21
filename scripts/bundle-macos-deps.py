#!/usr/bin/env python3
"""
Bundle macOS dynamic library dependencies for BBeBee audio-engine.

Traces all transitive non-system dylibs from audio-engine and libmpv.2.dylib,
copies them into the destination directory, rewrites load commands to @rpath/<basename>,
configures @loader_path and @executable_path in LC_RPATH, removes foreign/Homebrew rpaths,
and ad-hoc signs all binaries so they pass Apple Silicon AMFI security checks.
"""

import os
import sys
import shutil
import subprocess

def get_deps(path, is_exe=False):
    out = subprocess.check_output(['otool', '-L', path]).decode('utf-8')
    lines = out.splitlines()[1:]
    if not is_exe:
        lines = lines[1:]  # Skip LC_ID_DYLIB
    deps = []
    for line in lines:
        line = line.strip()
        if not line:
            continue
        dep = line.split()[0]
        if not dep.startswith('/usr/lib') and not dep.startswith('/System'):
            deps.append(dep)
    return deps

def get_rpaths(path):
    import re
    out = subprocess.check_output(['otool', '-l', path]).decode('utf-8')
    rpaths = []
    lines = out.splitlines()
    i = 0
    while i < len(lines):
        line = lines[i].strip()
        if line == 'cmd LC_RPATH':
            i += 2
            m = re.search(r'path (.+) \(offset', lines[i])
            if m:
                rpaths.append(m.group(1).strip())
        i += 1
    return rpaths

def bundle_macos(target_dir):
    exe_path = os.path.join(target_dir, 'audio-engine')
    mpv_path = os.path.join(target_dir, 'libmpv.2.dylib')

    if not os.path.exists(exe_path) or not os.path.exists(mpv_path):
        print(f"Error: {exe_path} or {mpv_path} does not exist!")
        sys.exit(1)

    print(f"[bundle-macos] Inspecting dependencies in {target_dir}...")
    visited = {}
    to_visit = [(exe_path, True), (mpv_path, False)]

    while to_visit:
        curr, is_exe = to_visit.pop(0)
        real_curr = os.path.realpath(curr)
        if real_curr in visited:
            continue
        visited[real_curr] = curr

        deps = get_deps(real_curr, is_exe)
        for d in deps:
            resolved = None
            if d.startswith('@rpath/'):
                sub = d[7:]
                if sub in ('libmpv.2.dylib', 'libmpv.dylib'):
                    continue
                cand = os.path.join(target_dir, sub)
                if os.path.exists(cand):
                    resolved = cand
                else:
                    res = subprocess.getoutput(f'find /opt/homebrew/Cellar -name {sub} 2>/dev/null').splitlines()
                    if res and os.path.exists(res[0]):
                        resolved = res[0]
            elif os.path.exists(d):
                resolved = d

            if resolved and os.path.realpath(resolved) not in visited:
                to_visit.append((resolved, False))

    print(f"[bundle-macos] Total items in closure: {len(visited)}")

    copied_map = {}
    for real, orig in visited.items():
        base = os.path.basename(orig)
        if base in ('audio-engine', 'libmpv.2.dylib'):
            continue
        dest = os.path.join(target_dir, base)
        if not os.path.exists(dest):
            shutil.copy2(real, dest)
            os.chmod(dest, 0o755)
            print(f"  + Copied {base}")
        copied_map[base] = dest

    symlink_path = os.path.join(target_dir, 'libmpv.dylib')
    if not os.path.exists(symlink_path):
        os.symlink('libmpv.2.dylib', symlink_path)

    all_binaries = [exe_path, mpv_path] + list(copied_map.values())

    ref_rewrites = {}
    for b in all_binaries:
        os.chmod(b, 0o755)
        is_exe = (os.path.basename(b) == 'audio-engine')
        deps = get_deps(b, is_exe)
        for d in deps:
            base = os.path.basename(d)
            if base in copied_map or base in ('libmpv.2.dylib', 'libmpv.dylib'):
                ref_rewrites[d] = f'@rpath/{base}'

    print(f"[bundle-macos] Rewriting library IDs and load paths...")
    for b in all_binaries:
        base = os.path.basename(b)
        is_exe = (base == 'audio-engine')

        if not is_exe:
            subprocess.run(['install_name_tool', '-id', f'@rpath/{base}', b], check=False, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

        rps = get_rpaths(b)
        for r in rps:
            if r not in ('@loader_path', '@executable_path'):
                subprocess.run(['install_name_tool', '-delete_rpath', r, b], check=False, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

        subprocess.run(['install_name_tool', '-add_rpath', '@loader_path', b], check=False, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        subprocess.run(['install_name_tool', '-add_rpath', '@executable_path', b], check=False, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

        deps = get_deps(b, is_exe)
        for d in deps:
            if d in ref_rewrites:
                subprocess.run(['install_name_tool', '-change', d, ref_rewrites[d], b], check=False, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            else:
                d_base = os.path.basename(d)
                if d_base in copied_map or d_base in ('libmpv.2.dylib', 'libmpv.dylib'):
                    subprocess.run(['install_name_tool', '-change', d, f'@rpath/{d_base}', b], check=False, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    print(f"[bundle-macos] Ad-hoc codesigning all binaries...")
    for b in all_binaries:
        res = subprocess.run(['codesign', '--force', '--sign', '-', b], capture_output=True, text=True)
        if res.returncode != 0:
            print(f"Warning: codesign error on {b}: {res.stderr.strip()}")

    print(f"[bundle-macos] Complete! macOS audio-engine in {target_dir} is fully self-contained.")

if __name__ == '__main__':
    target = sys.argv[1] if len(sys.argv) > 1 else 'resources/macos'
    bundle_macos(os.path.abspath(target))
