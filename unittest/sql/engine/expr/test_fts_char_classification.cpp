/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#include "storage/fts/ik/ob_ik_char_util.h"
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <vector>

#ifdef NDEBUG
#error "Build this correctness test with -UNDEBUG so its checks remain active."
#endif

using namespace oceanbase::common;
using namespace oceanbase::storage;
using Type = ObFTCharUtil::CharType;

// Reference the original repeated-decode algorithm, including its error path
// and UTF-16-only surrogate handling. Do not replace this with the optimized
// routine: the two implementations must be checked independently.
template<ObCharsetType CS>
int reference(const char *input, uint8_t length, Type &type)
{
  type = Type::USELESS;
  if (input == nullptr) return OB_INVALID_ARGUMENT;
  const auto *first = reinterpret_cast<const unsigned char *>(input);
  const auto *last = first + length;
  ob_wc_t unicode = 0;
  auto decode = [&] { return ob_charset_decode_unicode<CS>(first, last, unicode) >= 0; };
  if (!decode()) return OB_UNEXPECT_INTERNAL_ERROR;
  if (ObUnicodeBlockUtils::is_alpha(unicode)) type = Type::ENGLISH_LETTER;
  else {
    if (!decode()) return OB_UNEXPECT_INTERNAL_ERROR;
    if (ObUnicodeBlockUtils::is_arabic(unicode)) type = Type::ARABIC_LETTER;
    else {
      if (!decode()) return OB_UNEXPECT_INTERNAL_ERROR;
      if (ObUnicodeBlockUtils::is_chinese(unicode)) type = Type::CHINESE;
      else {
        if (!decode()) return OB_UNEXPECT_INTERNAL_ERROR;
        if (ObUnicodeBlockUtils::is_other_cjk(unicode)) type = Type::OTHER_CJK;
        else if constexpr (CS == CHARSET_UTF16 || CS == CHARSET_UTF16LE) {
          if (!decode()) return OB_UNEXPECT_INTERNAL_ERROR;
          if (ObUnicodeBlockUtils::check_high_surrogate(unicode)) type = Type::SURROGATE_HIGH;
          else {
            if (!decode()) return OB_UNEXPECT_INTERNAL_ERROR;
            if (ObUnicodeBlockUtils::check_low_surrogate(unicode)) type = Type::SURROGATE_LOW;
          }
        }
      }
    }
  }
  return OB_SUCCESS;
}

template<ObCharsetType CS>
void check(ObCollationType collation, const char *input, uint8_t length)
{
  Type old_type = Type::USELESS, new_type = Type::USELESS;
  const int old_ret = ObCharset::charset_type_by_coll(collation) == CS
      ? reference<CS>(input, length, old_type) : OB_NOT_SUPPORTED;
  const int new_ret = ObFTCharUtil::classify_first_char(collation, input, length, new_type);
  if (old_ret != new_ret || old_type != new_type) {
    std::fprintf(stderr,"mismatch charset=%d collation=%d length=%u old_ret=%d new_ret=%d oldtype=%d newtype=%d first=%u second=%u\n",int(CS),int(collation),length,old_ret,new_ret,int(old_type),int(new_type),unsigned(static_cast<unsigned char>(input[0])),length>1?unsigned(static_cast<unsigned char>(input[1])):0);
  }
  assert(old_ret == new_ret);
  assert(old_type == new_type);
}

struct EncodedChar { char bytes[4]; uint8_t length; };
__attribute__((noinline)) int original(const EncodedChar &c, Type &type)
{
  return reference<CHARSET_UTF8MB4>(c.bytes, c.length, type);
}
__attribute__((noinline)) int optimized(const EncodedChar &c, Type &type)
{
  return ObFTCharUtil::classify_first_char(CS_TYPE_UTF8MB4_BIN, c.bytes, c.length, type);
}

// Optional diagnostic only. Competition performance must use the unmodified
// full SQL benchmark, not this in-process measurement.
void benchmark()
{
  std::vector<EncodedChar> corpus;
  unsigned seed = 4129;
  for (int i = 0; i < 1024; ++i) {
    seed = seed * 1664525u + 1013904223u;
    const ob_wc_t cp = i % 4 == 0 ? 0x4e00 + seed % 20000
        : i % 4 == 1 ? 'a' + seed % 26
        : i % 4 == 2 ? '0' + seed % 10 : 0x1f600 + seed % 64;
    EncodedChar c{};
    c.length = ob_charset_encode_unicode<CHARSET_UTF8MB4>(cp,
        reinterpret_cast<unsigned char *>(c.bytes),
        reinterpret_cast<unsigned char *>(c.bytes + sizeof(c.bytes)));
    corpus.push_back(c);
  }
  auto measure = [&](auto classify) {
    uint64_t checksum = 0;
    const auto start = std::chrono::steady_clock::now();
    for (int round = 0; round < 5000; ++round) {
      for (const auto &c : corpus) {
        Type type;
        const int ret = classify(c, type);
        assert(ret == OB_SUCCESS);
        checksum += static_cast<uint8_t>(type);
      }
    }
    const double ms = std::chrono::duration<double,std::milli>(
        std::chrono::steady_clock::now() - start).count();
    std::printf("milliseconds=%.3f checksum=%lu\n", ms, checksum);
    return checksum;
  };
  for (int sample = 0; sample < 3; ++sample) {
    std::printf("sample=%d original ", sample + 1);
    const auto old_sum = measure(original);
    std::printf("sample=%d optimized ", sample + 1);
    const auto new_sum = measure(optimized);
    assert(old_sum == new_sum);
  }
}

int main(int argc, char **argv)
{
  assert(ObCharset::init_charset() == OB_SUCCESS);
  for (ob_wc_t cp = 0; cp <= 0x10ffff; ++cp) {
    if (cp >= 0xd800 && cp <= 0xdfff) continue;
    unsigned char encoded[4];
    const int length = ob_charset_encode_unicode<CHARSET_UTF8MB4>(cp, encoded, encoded + sizeof(encoded));
    assert(length > 0);
    check<CHARSET_UTF8MB4>(CS_TYPE_UTF8MB4_BIN, reinterpret_cast<const char *>(encoded), length);
  }
  // Some seekdb builds do not register UTF-16 collations. Verify their
  // original unsupported result without logging 131072 duplicate errors.
  const unsigned utf16_limit = ObCharset::charset_type_by_coll(CS_TYPE_UTF16_BIN)
      == CHARSET_UTF16 ? 0xffff : 0;
  for (unsigned cp = 0; cp <= utf16_limit; ++cp) {
    const char be[] = {static_cast<char>(cp >> 8), static_cast<char>(cp)};
    const char le[] = {be[1], be[0]};
    check<CHARSET_UTF16>(CS_TYPE_UTF16_BIN, be, sizeof(be));
    check<CHARSET_UTF16LE>(CS_TYPE_UTF16LE_BIN, le, sizeof(le));
  }
  const char malformed[][4] = {{char(0xff),0,0,0},{char(0xc0),char(0x80),0,0},
    {char(0xe4),char(0xb8),0,0},{char(0xf4),char(0x90),char(0x80),char(0x80)}};
  for (const auto &bytes : malformed) {
    for (uint8_t length = 0; length <= 4; ++length)
      check<CHARSET_UTF8MB4>(CS_TYPE_UTF8MB4_BIN, bytes, length);
  }
  std::puts("Classification equivalence passed: all Unicode scalar values, UTF-16 dispatch, malformed UTF-8.");
  if (argc == 2 && std::strcmp(argv[1], "--benchmark") == 0) benchmark();
  return 0;
}
