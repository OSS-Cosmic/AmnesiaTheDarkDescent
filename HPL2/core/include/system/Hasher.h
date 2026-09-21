
/* Copyright (c) 2021 Hans-Kristian Arntzen
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
#ifndef R_HASHER_H
#define R_HASHER_H

#include <cstddef>
#include <stdint.h>

typedef uint64_t hash_t;
static hash_t const HASH_INITIAL_VALUE = 0xcbf29ce484222325ull;

static inline hash_t hash_u32( hash_t hash, uint32_t value )
{

	return ( hash * 0x100000001b3ull ) ^ value;
}

static inline hash_t hash_s32( hash_t hash, int32_t value )
{
	return hash_u32( hash, value );
}

static inline hash_t hash_u64( hash_t hash, uint64_t value )
{
	hash = hash_u32( hash, value & 0xffffffffu );
	hash = hash_u32( hash, value >> 32 );
	return hash;
}

static inline hash_t hash_f32( hash_t hash, float value )
{
	union {
		float f32;
		uint32_t u32;
	} u;
	u.f32 = value;
	return hash_u32( hash, u.u32 );
}

// Returns a unique, randomly generated 64-bit hash. Each call yields a
// different value, seeded once per thread from a non-deterministic source.
hash_t hash_random();

static inline hash_t hash_data( hash_t hash, const void *data, size_t size )
{
	for( size_t i = 0; i < size; i++ )
		hash = ( hash * 0x100000001b3ull ) ^ ( (uint8_t *)data )[i];
	return hash;
}

// inline void string( const std::string &str )
//{
//	u32( 0xff );
//	for( auto &c : str )
//		u32( uint8_t( c ) );
// }

// http://www.azillionmonkeys.com/qed/hash.html
static inline hash_t hash_data_hsieh( hash_t hash, const void *buf, size_t len )
{

#if (defined(__GNUC__) && defined(__i386__)) || defined(__WATCOMC__) || defined(_MSC_VER) || defined (__BORLANDC__) || defined (__TURBOC__)
#define get16bits(d) (*((const uint16_t *) (d)))
#else
#define get16bits(d) ((((uint32_t)(((const uint8_t *)(d))[1])) << 8)\
                       +(uint32_t)(((const uint8_t *)(d))[0]) )
#endif
	uint32_t tmp = 0;
	const unsigned char *s = (unsigned char*)buf;

	if( len <= 0 || buf == NULL ) {
		return hash;
  }

  const size_t rem = len & 3;
  len >>= 2;

  /* Main loop */
  for (; len > 0; len--) {
    hash += get16bits(s);
    tmp = static_cast<uint32_t>((get16bits(s + 2) << 11) ^ hash);
    hash = (hash << 16) ^ tmp;
    s += 2 * sizeof(uint16_t);
    hash += hash >> 11;
  }

  /* Handle end cases */
  switch (rem) {
    case 3:
      hash += get16bits(s);
      hash ^= hash << 16;
      hash ^= s[sizeof(uint16_t)] << 18;
      hash += hash >> 11;
      break;
    case 2:
      hash += get16bits(s);
      hash ^= hash << 11;
      hash += hash >> 17;
      break;
    case 1:
      hash += *s;
      hash ^= hash << 10;
      hash += hash >> 1;
  }

  /* Force "avalanching" of final 127 bits */
  hash ^= hash << 3;
  hash += hash >> 5;
  hash ^= hash << 4;
  hash += hash >> 17;
  hash ^= hash << 25;
  hash += hash >> 6;

  #undef get16bits

	return hash;
}
#endif

