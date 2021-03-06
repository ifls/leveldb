// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "util/coding.h"

namespace leveldb {
//  追加 uint32
void PutFixed32(std::string *dst, uint32_t value) {
	char buf[sizeof(value)];
	EncodeFixed32(buf, value);
	dst->append(buf, sizeof(buf));
}

void PutFixed64(std::string *dst, uint64_t value) {
	char buf[sizeof(value)];
	EncodeFixed64(buf, value);
	dst->append(buf, sizeof(buf));
}

// 变长编码
char *EncodeVarint32(char *dst, uint32_t v) {
	// Operate on characters as unsigneds
	uint8_t *ptr = reinterpret_cast<uint8_t *>(dst);
	static const int B = 128;  //0x 10000000
	if (v < (1 << 7)) {  //0x01111111 + 1 == 0x1000 0000 128
		*(ptr++) = v;
	} else if (v < (1 << 14)) {  // 0x0011 1111 1111 1111 + 1 == 0x0100 0000 0000 0000
		*(ptr++) = v | B;    // 2B  每个字节最高位是1,表示连续
		*(ptr++) = v >> 7;   //
	} else if (v < (1 << 21)) {  //3B
		*(ptr++) = v | B;
		*(ptr++) = (v >> 7) | B;
		*(ptr++) = v >> 14;
	} else if (v < (1 << 28)) {  //4B
		*(ptr++) = v | B;
		*(ptr++) = (v >> 7) | B;
		*(ptr++) = (v >> 14) | B;
		*(ptr++) = v >> 21;
	} else {   //5B
		*(ptr++) = v | B;
		*(ptr++) = (v >> 7) | B;
		*(ptr++) = (v >> 14) | B;
		*(ptr++) = (v >> 21) | B;
		*(ptr++) = v >> 28;
	}
	return reinterpret_cast<char *>(ptr);
}

void PutVarint32(std::string *dst, uint32_t v) {
	char buf[5];
	char *ptr = EncodeVarint32(buf, v);
	dst->append(buf, ptr - buf);
}

char *EncodeVarint64(char *dst, uint64_t v) {
	static const int B = 128;
	uint8_t *ptr = reinterpret_cast<uint8_t *>(dst);
	// 比上面的Varint32简洁
	while (v >= B) {
		*(ptr++) = v | B;
		v >>= 7;
	}
	*(ptr++) = static_cast<uint8_t>(v);
	return reinterpret_cast<char *>(ptr);
}

void PutVarint64(std::string *dst, uint64_t v) {
	char buf[10];
	char *ptr = EncodeVarint64(buf, v);
	dst->append(buf, ptr - buf);
}

// 变长 长度编码 + []byte
void PutLengthPrefixedSlice(std::string *dst, const Slice &value) {
	PutVarint32(dst, value.size());
	dst->append(value.data(), value.size());
}

// 计算可变编码长度
int VarintLength(uint64_t v) {
	int len = 1;
	while (v >= 128) {
		v >>= 7;
		len++;
	}
	return len;
}

const char *GetVarint32PtrFallback(const char *p, const char *limit, uint32_t *value) {
	uint32_t result = 0;
	// 最多左移28 bit
	for (uint32_t shift = 0; shift <= 28 && p < limit; shift += 7) {
		uint32_t byte = *(reinterpret_cast<const uint8_t *>(p));
		p++;
		if (byte & 128) { //  每字节高8位为1, 表示之后的数据也是一部分
			// More bytes are present
			result |= ((byte & 127) << shift);
		} else {
			result |= (byte << shift);
			*value = result;
			return reinterpret_cast<const char *>(p);
		}
	}
	return nullptr;
}

bool GetVarint32(Slice *input, uint32_t *value) {
	const char *p = input->data();
	const char *limit = p + input->size();
	const char *q = GetVarint32Ptr(p, limit, value);
	if (q == nullptr) {
		return false;
	} else {
		*input = Slice(q, limit - q);
		return true;
	}
}

const char *GetVarint64Ptr(const char *p, const char *limit, uint64_t *value) {
	uint64_t result = 0;
	for (uint32_t shift = 0; shift <= 63 && p < limit; shift += 7) {
		uint64_t byte = *(reinterpret_cast<const uint8_t *>(p));
		p++;
		if (byte & 128) {
			// More bytes are present
			result |= ((byte & 127) << shift);
		} else {
			result |= (byte << shift);
			*value = result;
			return reinterpret_cast<const char *>(p);
		}
	}
	return nullptr;
}

bool GetVarint64(Slice *input, uint64_t *value) {
	const char *p = input->data();
	const char *limit = p + input->size();
	const char *q = GetVarint64Ptr(p, limit, value);
	if (q == nullptr) {
		return false;
	} else {
		*input = Slice(q, limit - q);
		return true;
	}
}

const char *GetLengthPrefixedSlice(const char *p, const char *limit, Slice *result) {
	uint32_t len;
	p = GetVarint32Ptr(p, limit, &len);
	if (p == nullptr) return nullptr;
	if (p + len > limit) return nullptr;
	*result = Slice(p, len);
	return p + len;
}

bool GetLengthPrefixedSlice(Slice *input, Slice *result) {
	uint32_t len;
	if (GetVarint32(input, &len) && input->size() >= len) {
		*result = Slice(input->data(), len);
		input->remove_prefix(len);
		return true;
	} else {
		return false;
	}
}

}  // namespace leveldb
