// Little/big-endian binary read helpers for Hedgehog Engine 2 formats.
#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace sf {

using Bytes = std::vector<uint8_t>;

inline uint16_t u16le(const uint8_t* d, size_t o) { uint16_t v; memcpy(&v, d + o, 2); return v; }
inline uint32_t u32le(const uint8_t* d, size_t o) { uint32_t v; memcpy(&v, d + o, 4); return v; }
inline uint64_t u64le(const uint8_t* d, size_t o) { uint64_t v; memcpy(&v, d + o, 8); return v; }
inline int32_t  s32le(const uint8_t* d, size_t o) { int32_t v; memcpy(&v, d + o, 4); return v; }
inline int16_t  s16le(const uint8_t* d, size_t o) { int16_t v; memcpy(&v, d + o, 2); return v; }
inline float    f32le(const uint8_t* d, size_t o) { float v; memcpy(&v, d + o, 4); return v; }

inline uint16_t bswap16(uint16_t v) { return (uint16_t)((v >> 8) | (v << 8)); }
inline uint32_t bswap32(uint32_t v) {
    return ((v >> 24) & 0xFF) | ((v >> 8) & 0xFF00) | ((v << 8) & 0xFF0000) | ((v << 24) & 0xFF000000);
}
inline uint16_t u16be(const uint8_t* d, size_t o) { return bswap16(u16le(d, o)); }
inline uint32_t u32be(const uint8_t* d, size_t o) { return bswap32(u32le(d, o)); }
inline int32_t  s32be(const uint8_t* d, size_t o) { return (int32_t)bswap32(u32le(d, o)); }
inline int16_t  s16be(const uint8_t* d, size_t o) { return (int16_t)bswap16(u16le(d, o)); }
inline float    f32be(const uint8_t* d, size_t o) { uint32_t v = bswap32(u32le(d, o)); float f; memcpy(&f, &v, 4); return f; }
inline float    f16(uint16_t h) {
    uint32_t sign = (h & 0x8000) << 16;
    uint32_t exp = (h >> 10) & 0x1F, man = h & 0x3FF, out;
    if (exp == 0) { if (man == 0) out = sign; else { exp = 127 - 15 + 1; while (!(man & 0x400)) { man <<= 1; exp--; } man &= 0x3FF; out = sign | (exp << 23) | (man << 13); } }
    else if (exp == 31) out = sign | 0x7F800000 | (man << 13);
    else out = sign | ((exp - 15 + 127) << 23) | (man << 13);
    float f; memcpy(&f, &out, 4); return f;
}

inline std::string cstr(const uint8_t* d, size_t size, size_t o) {
    if (o == 0 || o >= size) return "";
    size_t e = o;
    while (e < size && d[e] != 0) e++;
    return std::string((const char*)d + o, e - o);
}

// Read an entire file into a byte vector. Returns false on failure.
bool read_file(const std::string& path, Bytes& out);
bool write_file(const std::string& path, const uint8_t* data, size_t n);

} // namespace sf
