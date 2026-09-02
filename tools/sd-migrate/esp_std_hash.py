# libstdc++ size_t _Hash_bytes for 32-bit (ESP32-C3)
# Matches gcc libstdc++ bits/hash_bytes.h / _Hash_bytes unaligned 32-bit path
# seed used by std::hash for strings is 0xc70f6907

def hash_bytes_32(data: bytes, seed: int = 0xC70F6907) -> int:
    m = 0x5BD1E995
    r = 24
    length = len(data)
    h = (seed ^ length) & 0xFFFFFFFF
    i = 0
    while length >= 4:
        k = int.from_bytes(data[i:i+4], "little")
        k = (k * m) & 0xFFFFFFFF
        k ^= (k >> r)
        k = (k * m) & 0xFFFFFFFF
        h = (h * m) & 0xFFFFFFFF
        h ^= k
        i += 4
        length -= 4
    # remaining
    if length == 3:
        h ^= data[i+2] << 16
    if length >= 2:
        h ^= data[i+1] << 8
    if length >= 1:
        h ^= data[i]
        h = (h * m) & 0xFFFFFFFF
    h ^= (h >> 13)
    h = (h * m) & 0xFFFFFFFF
    h ^= (h >> 15)
    return h & 0xFFFFFFFF

# also try 64-bit path in case (unlikely on C3 size_t is 32)
def hash_bytes_64(data: bytes, seed: int = 0xC70F6907) -> int:
    # simplified - not needed if 32-bit
    return hash_bytes_32(data, seed)

paths = [
    "/Fantasy-LitRPG/Dungeon Crawler Carl/06 - The Eye of the Bedlam Bride.epub",
    "/Fantasy-LitRPG/Dungeon Crawler Carl/01 - Dungeon Crawler Carl.epub",
]
for p in paths:
    b = p.encode("utf-8")
    h = hash_bytes_32(b)
    print(p)
    print("  epub_%u" % h, "epub_%d" % h)
