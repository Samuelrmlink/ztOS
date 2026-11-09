// SPDX-License-Identifier: GPL-2.0-or-later
#include "wire.h"

#include <stdio.h>
#include <string.h>

#include "ztbt_proto.h"

namespace ztbt {

void Value::addr(uint8_t out[6]) const {
  memset(out, 0, 6);
  if (t == 'a' && s.size() == 6) memcpy(out, s.data(), 6);
}

std::array<uint8_t, 16> Value::uuid() const {
  std::array<uint8_t, 16> out{};
  if (t == 'U' && s.size() == 16) memcpy(out.data(), s.data(), 16);
  return out;
}

std::string AddrText(const uint8_t a[6]) {
  char buf[18];
  snprintf(buf, sizeof buf, "%02X:%02X:%02X:%02X:%02X:%02X", a[0], a[1], a[2], a[3], a[4], a[5]);
  return buf;
}

namespace {

void put(std::string* o, const void* p, size_t n) { o->append((const char*)p, n); }
void put16(std::string* o, uint16_t v) { uint8_t b[2] = {(uint8_t)v, (uint8_t)(v >> 8)}; put(o, b, 2); }
void put32(std::string* o, uint32_t v) {
  uint8_t b[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
  put(o, b, 4);
}

bool enc(const Value& v, std::string* o) {
  o->push_back(v.t);
  switch (v.t) {
    case 'y': o->push_back((char)v.u); return true;
    case 'B': o->push_back(v.u ? 1 : 0); return true;
    case 'q': put16(o, (uint16_t)v.u); return true;
    case 'u': put32(o, (uint32_t)v.u); return true;
    case 'i': put32(o, (uint32_t)(int32_t)v.i); return true;
    case 'a': if (v.s.size() != 6) return false; put(o, v.s.data(), 6); return true;
    case 'U': if (v.s.size() != 16) return false; put(o, v.s.data(), 16); return true;
    case 's': if (v.s.size() > 0xffff) return false; put16(o, (uint16_t)v.s.size()); put(o, v.s.data(), v.s.size()); return true;
    case 'b': put32(o, (uint32_t)v.s.size()); put(o, v.s.data(), v.s.size()); return true;
    case 'L':
      if (v.items.size() > 0xffff) return false;
      put16(o, (uint16_t)v.items.size());
      for (auto& x : v.items) if (!enc(x, o)) return false;
      return true;
    case 'T':
      if (v.items.size() > 0xff) return false;
      o->push_back((char)v.items.size());
      for (auto& x : v.items) if (!enc(x, o)) return false;
      return true;
  }
  return false;
}

struct Reader {
  const uint8_t* p;
  size_t n, pos = 0;
  int depth = 0;
  bool take(size_t k, const uint8_t** out) {
    if (k > n - pos) return false;
    *out = p + pos;
    pos += k;
    return true;
  }
  bool value(Value* v) {
    const uint8_t* b;
    if (!take(1, &b)) return false;
    v->t = (char)b[0];
    switch (v->t) {
      case 'y': if (!take(1, &b)) return false; v->u = b[0]; return true;
      case 'B': if (!take(1, &b) || b[0] > 1) return false; v->u = b[0]; return true;
      case 'q': if (!take(2, &b)) return false; v->u = b[0] | b[1] << 8; return true;
      case 'u': if (!take(4, &b)) return false; v->u = (uint32_t)(b[0] | b[1] << 8 | b[2] << 16 | (uint32_t)b[3] << 24); return true;
      case 'i': if (!take(4, &b)) return false; v->i = (int32_t)(b[0] | b[1] << 8 | b[2] << 16 | (uint32_t)b[3] << 24); return true;
      case 'a': if (!take(6, &b)) return false; v->s.assign((const char*)b, 6); return true;
      case 'U': if (!take(16, &b)) return false; v->s.assign((const char*)b, 16); return true;
      case 's': {
        if (!take(2, &b)) return false;
        size_t k = b[0] | b[1] << 8;
        if (!take(k, &b)) return false;
        v->s.assign((const char*)b, k);
        return true;
      }
      case 'b': {
        if (!take(4, &b)) return false;
        size_t k = b[0] | b[1] << 8 | b[2] << 16 | (size_t)b[3] << 24;
        if (!take(k, &b)) return false;
        v->s.assign((const char*)b, k);
        return true;
      }
      case 'L': case 'T': {
        size_t k;
        if (v->t == 'L') { if (!take(2, &b)) return false; k = b[0] | b[1] << 8; }
        else { if (!take(1, &b)) return false; k = b[0]; }
        if (++depth > 8) return false;
        for (size_t i = 0; i < k; i++) {
          v->items.emplace_back();
          if (!value(&v->items.back())) return false;
        }
        depth--;
        return true;
      }
    }
    return false;
  }
};

// signature matching: one element at sig[*i]
bool match_one(const char* sig, size_t* i, const Value& v);

bool match_seq(const char* sig, size_t* i, char stop, const std::vector<Value>& vs) {
  size_t k = 0;
  while (sig[*i] && sig[*i] != stop) {
    if (k >= vs.size()) return false;
    if (!match_one(sig, i, vs[k++])) return false;
  }
  if (stop && sig[*i] != stop) return false;
  if (stop) (*i)++;
  return k == vs.size();
}

bool match_one(const char* sig, size_t* i, const Value& v) {
  char c = sig[*i];
  if (c == 'L' || c == 'T') {
    if (v.t != c) return false;
    if (sig[*i + 1] == '(') {
      size_t start = *i + 2;
      if (c == 'T') { *i = start; return match_seq(sig, i, ')', v.items); }
      size_t end = start;
      for (auto& item : v.items) {       // each item a tuple of the bracketed signature
        if (item.t != 'T') return false;
        size_t j = start;
        if (!match_seq(sig, &j, ')', item.items)) return false;
        end = j;
      }
      if (v.items.empty()) {             // skip the brackets
        int depth = 0;
        size_t j = *i + 1;
        do { if (sig[j] == '(') depth++; else if (sig[j] == ')') depth--; j++; } while (depth && sig[j]);
        end = j;
      }
      *i = end;
      return true;
    }
    char elem = sig[*i + 1];
    for (auto& item : v.items) if (item.t != elem) return false;
    *i += 2;
    return true;
  }
  (*i)++;
  return v.t == c;
}

}  // namespace

bool Encode(uint16_t op, uint32_t id, const Values& args, std::string* out) {
  out->clear();
  put16(out, op);
  put16(out, 0);
  put32(out, id);
  for (auto& v : args) if (!enc(v, out)) return false;
  return out->size() <= kMaxPacket;
}

bool Decode(const uint8_t* p, size_t n, uint16_t* op, uint32_t* id, Values* args) {
  if (n < 8) return false;
  *op = p[0] | p[1] << 8;
  *id = (uint32_t)(p[4] | p[5] << 8 | p[6] << 16 | (uint32_t)p[7] << 24);
  Reader r{p, n, 8};
  args->clear();
  while (r.pos < n) {
    args->emplace_back();
    if (!r.value(&args->back())) return false;
  }
  return true;
}

bool Matches(const char* sig, const Values& args) {
  size_t i = 0;
  return match_seq(sig, &i, 0, args);
}

}  // namespace ztbt
