#!/usr/bin/env python3
import base64
import re
import sys

P256_PREFIX = bytes.fromhex("3059301306072a8648ce3d020106082a8648ce3d030107034200")


def public_keys(pem_text):
    pattern = r"-----BEGIN PUBLIC KEY-----\s*([A-Za-z0-9+/=\s]+)-----END PUBLIC KEY-----"
    keys = []
    for match in re.finditer(pattern, pem_text):
        der = base64.b64decode("".join(match[1].split()), validate=True)
        if len(der) != len(P256_PREFIX) + 65 or not der.startswith(P256_PREFIX) or der[len(P256_PREFIX)] != 4:
            raise ValueError("Not an uncompressed P-256 public key")
        keys.append((match[0] + "\n", der[len(P256_PREFIX):]))
    remainder = re.sub(pattern, "", pem_text)
    if any(line.strip() and not line.startswith("#") for line in remainder.splitlines()):
        raise ValueError("Expected only public PEM keys or comments")
    return keys


def key_header(pem_text):
    keys = public_keys(pem_text)
    rows = ["    std::array<uint8_t, 65>{" + ", ".join(f"0x{b:02x}" for b in point) + "}" for _, point in keys]
    return (
        "#pragma once\n#include <array>\n#include <cstdint>\n"
        f"inline constexpr std::array<std::array<uint8_t, 65>, {len(keys)}> UPDATE_PUBLIC_KEYS{{{{\n"
        + ",\n".join(rows) + "\n}};\n"
    )


if __name__ == "__main__":
    if len(sys.argv) != 3:
        raise SystemExit("usage: update-key-header.py public-keys.pem src/UpdateKey.hpp")
    with open(sys.argv[1], encoding="ascii") as source:
        header = key_header(source.read())
    with open(sys.argv[2], "w", encoding="utf-8", newline="\n") as target:
        target.write(header)
