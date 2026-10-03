#!/usr/bin/env python3
"""Compare two rknpu UAPI headers for ABI compatibility, by compiling them.

Why: an external project (oRKLLM/ork-driver) synthesizes its own regcmd
programs and submits them through the rknpu DRM ioctls. Those programs could
only be fed to *this* driver if the two sides agree on the ioctl command
numbers and on the layout of the submit/mem structs. This checks exactly that,
offline, without touching the board.

Method: emit a C file that includes one header and prints, at runtime,
  - sizeof() for each relevant struct
  - offsetof() for every field
  - the numeric ioctl request codes
Build it twice (once per header), diff the outputs. Identical output means the
two headers describe the same ABI.

Usage
    compare-driver-uapi-abi.py <headerA> <headerB>

Notes
    - The header does not need to be self-contained: the generated C file
      supplies the few kernel types (__u32/__u64/...) and the DRM ioctl macros
      if the header does not define them itself.
    - Compiles with the system cc; no kernel tree required.
"""

import os
import re
import subprocess
import sys
import tempfile

# Fields we care about: everything the submit path reads/writes. Extracted from
# the header text so the check follows the header rather than a hard-coded list.
STRUCTS = [
    "rknpu_action", "rknpu_submit", "rknpu_subcore_task",
    "rknpu_mem_create", "rknpu_mem_map", "rknpu_mem_destroy",
    "rknpu_mem_sync",
]

PREAMBLE = r"""
#include <stddef.h>
#include <stdio.h>
#include <stdint.h>
#include <sys/ioctl.h>
#ifndef DRM_IOCTL_BASE
#define DRM_IOCTL_BASE 'd'
#endif
#ifndef DRM_COMMAND_BASE
#define DRM_COMMAND_BASE 0x40
#endif
#ifndef DRM_IOWR
#define DRM_IOWR(nr,type) _IOWR(DRM_IOCTL_BASE,nr,type)
#endif
"""
# Same idea for the command number: the headers define the RKNPU_* selectors
# themselves, so only fall back if one is missing.

# Emitted only if the header under test does not bring in <linux/types.h> or
# define the kernel integral types itself. One header here defines nothing and
# includes nothing, so it needs them; the other includes <linux/types.h>, where
# a duplicate typedef is a redefinition error.
TYPE_FALLBACK = r"""
typedef uint32_t __u32; typedef int32_t __s32;
typedef uint64_t __u64; typedef int64_t  __s64;
typedef uint16_t __u16; typedef int16_t  __s16;
typedef uint8_t  __u8;  typedef int8_t   __s8;
"""


def fields_of(header_text, struct):
    """Best-effort field extraction so the generated offsets follow the header."""
    m = re.search(r"struct\s+%s\s*\{(.*?)\n\};" % re.escape(struct),
                  header_text, re.S)
    if not m:
        return []
    out = []
    depth = 0
    for line in m.group(1).split("\n"):
        line = line.split("/*")[0].split("//")[0].strip()
        if not line or line.startswith("#"):
            continue
        # Skip anything nested inside a union/struct: offsetof on an anonymous
        # or nested member's own fields would not compile, and the outer member
        # already covers it.
        opens, closes = line.count("{"), line.count("}")
        if depth > 0 or opens:
            depth += opens - closes
            if depth <= 0:
                depth = 0
            continue
        if closes:
            continue
        # Keep only "type name;" / "type name[..];" declarations. Take the last
        # identifier on the line as the member name; bail on anything that is
        # not a plain declaration (function pointers, macros, etc.).
        if not line.endswith(";"):
            continue
        decl = line[:-1]
        # drop a trailing array suffix
        decl = re.sub(r"\[[^\]]*\]\s*$", "", decl)
        toks = re.findall(r"[A-Za-z_][A-Za-z0-9_]*", decl)
        if len(toks) < 2 or "(" in decl:
            continue
        out.append(toks[-1])
    return out


def build_and_run(header, workdir, tag):
    text = open(header).read()
    # Decide whether this header needs the kernel-type fallback. It does not if
    # it includes <linux/types.h> or typedefs __u32/__u64 itself; the fallback
    # must come *after* the include otherwise.
    needs_fallback = ("linux/types.h" not in text
                      and "asm/types.h" not in text
                      and not re.search(r"typedef[^;]*\b__u32\b", text))
    if needs_fallback:
        c = [PREAMBLE, TYPE_FALLBACK,
             '#include "%s"' % os.path.abspath(header), "\nint main(void){"]
    else:
        c = [PREAMBLE, '#include "%s"' % os.path.abspath(header),
             "\nint main(void){"]
    for s in STRUCTS:
        c.append('  printf("sizeof %s = %%zu\\n", sizeof(struct %s));' % (s, s))
        for f in fields_of(text, s):
            c.append('  printf("offset %s.%s = %%zu\\n", offsetof(struct %s, %s));'
                     % (s, f, s, f))
    for name in ("ACTION", "SUBMIT", "MEM_CREATE", "MEM_MAP", "MEM_DESTROY",
                 "MEM_SYNC"):
        c.append('  printf("ioctl %s = %%lx\\n", '
                 '(unsigned long)DRM_IOCTL_RKNPU_%s);' % (name, name))
    c.append("  return 0;\n}")
    src = os.path.join(workdir, tag + ".c")
    exe = os.path.join(workdir, tag)
    open(src, "w").write("\n".join(c))
    r = subprocess.run(["cc", "-O0", "-o", exe, src],
                       capture_output=True, text=True)
    if r.returncode != 0:
        return None, (r.stderr or "").strip().split("\n")[0:6]
    r = subprocess.run([exe], capture_output=True, text=True)
    lines = [l for l in r.stdout.strip().split("\n") if l]
    return lines, None


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 2
    a, b = argv[1], argv[2]
    for p in (a, b):
        if not os.path.exists(p):
            print("missing header: %s" % p)
            return 2
    with tempfile.TemporaryDirectory() as d:
        la, ea = build_and_run(a, d, "A")
        lb, eb = build_and_run(b, d, "B")
    if la is None or lb is None:
        print("compile failed:")
        for p, e in ((a, ea), (b, eb)):
            if e:
                print("  %s:" % p)
                for line in e:
                    print("    %s" % line)
        return 3
    print("A: %s" % a)
    print("B: %s" % b)
    print()
    da, db = {}, {}
    for line in la:
        k, v = line.split(" = "); da[k] = v
    for line in lb:
        k, v = line.split(" = "); db[k] = v
    keys = sorted(set(da) | set(db))
    same = diff = 0
    for k in keys:
        if da.get(k) == db.get(k):
            same += 1
        else:
            diff += 1
            print("  DIFF %-34s A=%-12s B=%s" % (k, da.get(k), db.get(k)))
    print()
    print("compared %d items: %d identical, %d different" % (len(keys), same, diff))
    if diff == 0:
        print("verdict: ABI-IDENTICAL - programs built against A are valid for B")
        return 0
    print("verdict: ABI DIFFERS - a program built against one is NOT portable to "
          "the other")
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
