#pragma once

// Internal text helpers of BG Vox Lyrics: lossy UTF-8 handling and the
// normalisation used by the alignment. Operates on code points, so a multi-byte
// sequence is never split.

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace bgmedia::vox::detail {

// Decodes one code point at s[i] and advances i. Returns false (and advances by
// one byte) for an invalid / overlong / surrogate sequence; cp is then U+FFFD.
bool decodeUtf8(std::string_view s, std::size_t& i, char32_t& cp) noexcept;
void appendUtf8(std::string& out, char32_t cp);
// Valid UTF-8 copy: every invalid byte becomes U+FFFD.
std::string sanitizeUtf8(std::string_view s);
std::u32string decodeLossy(std::string_view s);

bool isSpaceCp(char32_t cp) noexcept;
// Letters and digits of Latin / Cyrillic / other alphabets; false for
// punctuation, symbols, spaces.
bool isWordCp(char32_t cp) noexcept;
// Lower case for Basic Latin, Latin-1, Latin Extended-A and Cyrillic; ё -> е.
char32_t foldCp(char32_t cp) noexcept;
bool isVowelCp(char32_t folded) noexcept;

// Whitespace-separated surface words (UTF-8, unchanged).
std::vector<std::string> splitWords(std::string_view text);
// Folded letters and digits only (apostrophes and punctuation dropped).
std::u32string normalizeWord(std::string_view word);
// Similarity 0..1 of two normalised tokens (1 - Levenshtein / max length);
// 0 when below the length-dependent acceptance threshold.
float tokenSimilarity(const std::u32string& a, const std::u32string& b) noexcept;
// Vowel count (ru/en); letters / 3 (at least 1) for a non-empty word without vowels.
std::size_t syllableWeight(std::string_view text);

} // namespace bgmedia::vox::detail
