// SPDX-License-Identifier: GPL-2.0-or-later
// ztos-btbridge's wire format, the shim's side (see ../ztbt/proto.py).
// Plain C++17; no Android or Bluetooth headers, so it builds and is tested
// on the host too.
#pragma once

#include <stdint.h>

#include <array>
#include <string>
#include <vector>

namespace ztbt {

struct Value {
  char t = 0;          // y q u i B a U s b L T
  uint64_t u = 0;      // y q u B
  int64_t i = 0;       // i
  std::string s;       // a (6 bytes) U (16 bytes, big endian) s b
  std::vector<Value> items;  // L T

  static Value Y(uint8_t v) { Value x; x.t = 'y'; x.u = v; return x; }
  static Value Q(uint16_t v) { Value x; x.t = 'q'; x.u = v; return x; }
  static Value U32(uint32_t v) { Value x; x.t = 'u'; x.u = v; return x; }
  static Value I32(int32_t v) { Value x; x.t = 'i'; x.i = v; return x; }
  static Value Bool(bool v) { Value x; x.t = 'B'; x.u = v; return x; }
  static Value Addr(const uint8_t a[6]) { Value x; x.t = 'a'; x.s.assign((const char*)a, 6); return x; }
  static Value Uuid(const std::array<uint8_t, 16>& be) { Value x; x.t = 'U'; x.s.assign((const char*)be.data(), 16); return x; }
  static Value Str(const std::string& v) { Value x; x.t = 's'; x.s = v; return x; }
  static Value Bytes(const uint8_t* p, size_t n) { Value x; x.t = 'b'; x.s.assign((const char*)p, n); return x; }
  static Value Bytes(const std::string& v) { Value x; x.t = 'b'; x.s = v; return x; }
  static Value List(std::vector<Value> v) { Value x; x.t = 'L'; x.items = std::move(v); return x; }
  static Value Tuple(std::vector<Value> v) { Value x; x.t = 'T'; x.items = std::move(v); return x; }

  uint32_t num() const { return (uint32_t)u; }
  int32_t sint() const { return (int32_t)i; }
  bool flag() const { return u != 0; }
  void addr(uint8_t out[6]) const;
  std::array<uint8_t, 16> uuid() const;
};

using Values = std::vector<Value>;

// Encode one message; false if it doesn't fit in kMaxPacket.
bool Encode(uint16_t op, uint32_t id, const Values& args, std::string* out, uint16_t flags = 0);
// Decode one packet; false on any malformation.
bool Decode(const uint8_t* p, size_t n, uint16_t* op, uint32_t* id, Values* args);
// Do args fit a signature such as "uqyqb" or "L(yqqqUq)"?
bool Matches(const char* sig, const Values& args);

std::string AddrText(const uint8_t a[6]);

}  // namespace ztbt
