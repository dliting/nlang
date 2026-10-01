/*-----------------------------------------------------------------------------
	compiler/Utf8.cpp
	Strict UTF-8 validation for the input contract of `.n` sources and
	`.nproj` project files (0.7.6). A legacy-encoded file used to pass
	silently into string constants; a UTF-8 BOM rode into the first token
	and corrupted it. Both entry points below are deliberately allocation-
	free on the success path — the caller scans the validated bytes in place.
-----------------------------------------------------------------------------*/

#include "nlang/compiler/Utf8.h"

#include <cstring>

namespace nlang
{

//Total length of the sequence starting with this leading byte
//(1 for ASCII); 0 marks an invalid lead (stray continuation or 0xF8+).
static size_t SequenceLength(unsigned char lead)
{
	if (lead < 0x80) return 1;
	if ((lead & 0xE0) == 0xC0) return 2;
	if ((lead & 0xF0) == 0xE0) return 3;
	if ((lead & 0xF8) == 0xF0) return 4;
	return 0;
}

//Decode the code point of a sequence whose tail bytes are already known
//to be continuations (masked 6-bit groups, big-endian by position).
static unsigned DecodeCodePoint(const char* at, size_t seqLength)
{
	unsigned cp;
	const unsigned char lead = static_cast<unsigned char>(*at);
	switch (seqLength)
	{
	case 2: cp = lead & 0x1Fu; break;
	case 3: cp = lead & 0x0Fu; break;
	case 4: cp = lead & 0x07u; break;
	default: cp = lead; break;  //ASCII
	}
	for (size_t k = 1; k < seqLength; ++k)
		cp = (cp << 6)
			| (static_cast<unsigned char>(at[k]) & 0x3Fu);
	return cp;
}

//Offset of the first byte that breaks well-formedness: a stray or
//truncated continuation, or a sequence whose decoded code point is
//outside its shortest-form range (overlong), in the surrogate block, or
//above U+10FFFF. Returns `length` when the whole input is valid — this
//single walk backs both Utf8Valid and the line diagnosis.
static size_t Utf8FirstInvalid(const char* bytes, size_t length)
{
	size_t i = 0;
	while (i < length)
	{
		unsigned char lead = static_cast<unsigned char>(bytes[i]);
		size_t seq = SequenceLength(lead);
		if (seq == 0)
			return i;
		if (i + seq > length)  //truncated at end of input
			return i;
		for (size_t k = 1; k < seq; ++k)
			if ((static_cast<unsigned char>(bytes[i + k]) & 0xC0) != 0x80)
				return i;
		//Range rules on the decoded value: the shortest-form floor
		//per width, the surrogate block, and the U+10FFFF ceiling.
		unsigned cp = DecodeCodePoint(bytes + i, seq);
		static const unsigned kFloor[] = {0, 0, 0x80, 0x800, 0x10000};
		if (cp < kFloor[seq] || cp > 0x10FFFF
			|| (cp >= 0xD800 && cp <= 0xDFFF))
			return i;
		i += seq;
	}
	return length;
}

bool Utf8Valid(const char* bytes, size_t length)
{
	return Utf8FirstInvalid(bytes, length) == length;
}

//1-based line of an offset (count '\n' strictly before it).
static size_t LineOfOffset(const char* bytes, size_t offset)
{
	size_t line = 1;
	for (size_t i = 0; i < offset; ++i)
		if (bytes[i] == '\n')
			++line;
	return line;
}

bool Utf8ContentCheck(const char* bytes, size_t length,
	const char** content, size_t* contentLength, std::string* reason)
{
	//A UTF-16/32 byte-order mark is the common "Notepad saved it as
	//Unicode" shape — name it instead of complaining about byte 0xFF.
	static const char kUtf16LeBom[] = {(char)0xFF, (char)0xFE};
	static const char kUtf16BeBom[] = {(char)0xFE, (char)0xFF};
	if (length >= 2
		&& (std::memcmp(bytes, kUtf16LeBom, 2) == 0
			|| std::memcmp(bytes, kUtf16BeBom, 2) == 0))
	{
		*reason = "is UTF-16, not UTF-8";
		return false;
	}

	//The UTF-8 BOM is accepted and skipped: editors write it by default
	//("UTF-8 with BOM"), and it is not part of the token stream.
	const char* text = bytes;
	size_t textLength = length;
	static const char kUtf8Bom[] = {(char)0xEF, (char)0xBB, (char)0xBF};
	if (length >= 3 && std::memcmp(bytes, kUtf8Bom, 3) == 0)
	{
		text = bytes + 3;
		textLength = length - 3;
	}

	size_t invalid = Utf8FirstInvalid(text, textLength);
	if (invalid != textLength)
	{
		*reason = "is not valid UTF-8 (first invalid byte at line "
			+ std::to_string(LineOfOffset(text, invalid)) + ")";
		return false;
	}

	*content = text;
	*contentLength = textLength;
	reason->clear();
	return true;
}

} //namespace nlang
