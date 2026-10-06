// w32_locale.cpp -- the locale and string stand-ins (w32_kernel.h): code pages, MultiByteToWideChar /
// WideCharToMultiByte, LCMapStringA/W, GetStringTypeA/W, GetLocaleInfoA, GetCurrencyFormatA, GetDateFormatA.
//
// One locale, the reference machine's: en-US (LCID 0x409), ANSI code page 1252, OEM code page 437 -- byte for byte
// what this machine's Windows answers, from w32_locale_tables.inc (generated there by test/w32_locale_gen.cpp: both
// code pages both ways with Windows' best fit, every GetLocaleInfoA string, case maps and character types over the
// BMP). Portable C++: the same on Windows and Linux, so a session plays the same on both.
//
// What the game asks (krn_res.cpp, crt_str.cpp, crt_start.cpp):
//   LocaleBegin: GetLocaleInfoA(LOCALE_USER_DEFAULT, SNATIVELANGNAME / IMEASURE / SDECIMAL / SMONDECIMALSEP).
//   LocaleMoney: GetCurrencyFormatA(LOCALE_USER_DEFAULT, 0, "%d" or "%d.%d", NULL, out, 0x40) -- "$1,234.00".
//   LocaleFormatShortDate: GetDateFormatA(LOCALE_USER_DEFAULT, DATE_SHORTDATE, {y, m, d}, NULL, out, n) -- "M/d/yyyy".
//   The C runtime at start: GetEnvironmentStringsW converted by WideCharToMultiByte(CP_ACP, 0, ...); GetACP /
//   GetCPInfo for its multibyte table; wctomb / mbtowc / the "C" locale's LCMapString / GetStringType paths (probes).
// Not covered (never asked by the game; each fails or is noted where it is): other locales and code pages (UTF-8
// included), LCMAP_SORTKEY and the NORM_ / kana / width mappings, MB_COMPOSITE / MB_USEGLYPHCHARS, and
// WC_COMPOSITECHECK's composition of a base character with a following combining mark (single characters, all the C
// runtime converts, are exact). LCMAP_TITLECASE's word rule is matched on samples only (single characters: exact).
// Quirks of this machine's Windows kept (test/w32_kernel_test.cpp found them): any negative length means "to the
// terminator"; a buffer too small gets what fits, unterminated; GetCurrencyFormatA takes "", "-" and "." as zero and
// a negative zero as positive; GetDateFormatA ignores the time of day and refuses DATE_LTRREADING / RTLREADING /
// MONTHDAY for en-US; GetLocaleInfoA(LOCALE_USER_DEFAULT, LOCALE_SLANGUAGE) leaves the last error 0.
#include "w32_kernel.h"
#include <string.h>
#include <time.h>
#include <string>
#include <vector>

namespace {
#include "w32_locale_tables.inc"

const uint32_t ERR_INVALID_FLAGS = 1004;

void fail(uint32_t e) { w32::set_last_error(e); }

// ---- code pages ------------------------------------------------------------------------------------------------------
struct CodePage {
    uint32_t id;
    const uint16_t* to_u;                        // [256]
    const uint16_t (*from_u)[2];                 // sorted {code unit, byte}
    int n_from;
};
const CodePage k_1252 = {1252, k_cp1252_to_u, k_cp1252_from_u, sizeof k_cp1252_from_u / sizeof k_cp1252_from_u[0]};
const CodePage k_437 = {437, k_cp437_to_u, k_cp437_from_u, sizeof k_cp437_from_u / sizeof k_cp437_from_u[0]};

// CP_ACP 0, CP_OEMCP 1, CP_THREAD_ACP 3, or the page itself
const CodePage* code_page(uint32_t cp) {
    if (cp == 0 || cp == 3 || cp == 1252) return &k_1252;
    if (cp == 1 || cp == 437) return &k_437;
    return 0;
}
// a code unit to a byte (best fit); false: none -- the default character
bool to_byte(const CodePage* cp, uint16_t u, bool best_fit, uint8_t* b) {
    if (u == 0) { *b = 0; return true; }
    int lo = 0, hi = cp->n_from - 1;
    while (lo <= hi) {
        const int mid = (lo + hi) / 2;
        const uint16_t k = cp->from_u[mid][0];
        if (k == u) {
            *b = (uint8_t)cp->from_u[mid][1];
            return best_fit || cp->to_u[*b] == u;   // WC_NO_BEST_FIT_CHARS: only what converts back to itself
        }
        if (k < u) lo = mid + 1;
        else hi = mid - 1;
    }
    return false;
}

// a sorted {key, value} table's value for key, or key
uint16_t mapped(const uint16_t (*t)[2], int n, uint16_t u) {
    int lo = 0, hi = n - 1;
    while (lo <= hi) {
        const int mid = (lo + hi) / 2;
        if (t[mid][0] == u) return t[mid][1];
        if (t[mid][0] < u) lo = mid + 1;
        else hi = mid - 1;
    }
    return u;
}
const uint16_t* ctype_run(uint16_t u) {
    const int n = sizeof k_ctype_runs / sizeof k_ctype_runs[0];
    int lo = 0, hi = n - 1;
    while (lo < hi) {                            // the last run starting at or before u
        const int mid = (lo + hi + 1) / 2;
        if (k_ctype_runs[mid][0] <= u) lo = mid;
        else hi = mid - 1;
    }
    return k_ctype_runs[lo];
}

// the locales answered: the user's and the system's default (both en-US on the reference machine), en-US itself
bool our_locale(uint32_t lcid) { return lcid == 0x400 || lcid == 0x800 || lcid == 0x409 || lcid == 0; }

const LocaleString* locale_entry(uint32_t type) {
    for (const LocaleString& s : k_locale_strings)
        if (s.type == type) return &s;
    return 0;
}
const char* locale_string(uint32_t type) {
    const LocaleString* e = locale_entry(type);
    return e ? e->text : 0;
}
int locale_int(uint32_t type) {
    const char* s = locale_string(type);
    int v = 0;
    while (s && *s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
    return v;
}

// a result (need bytes, the terminator included) into (out, size) with Windows' size rules: size 0 asks for the
// length; too small: as much as fits, unterminated, and ERROR_INSUFFICIENT_BUFFER, 0
int32_t give(const char* bytes, int32_t need, char* out, int32_t size) {
    if (size == 0) return need;
    if (size < need) {
        memcpy(out, bytes, (size_t)size);
        fail(w32::ERR_INSUFFICIENT_BUFFER);
        return 0;
    }
    memcpy(out, bytes, (size_t)need);
    return need;
}
int32_t give(const std::string& s, char* out, int32_t size) { return give(s.c_str(), (int32_t)s.size() + 1, out, size); }

}  // namespace

uint32_t W32K_CALL w32_GetACP() { return 1252; }
uint32_t W32K_CALL w32_GetOEMCP() { return 437; }

int32_t W32K_CALL w32_GetCPInfo(uint32_t cp, W32CpInfo* info) {
    if (!code_page(cp) || !info) {
        fail(w32::ERR_INVALID_PARAMETER);
        return 0;
    }
    info->max_char_size = 1;
    info->default_char[0] = '?';
    info->default_char[1] = 0;
    memset(info->lead_byte, 0, sizeof info->lead_byte);
    return 1;
}

int32_t W32K_CALL w32_MultiByteToWideChar(uint32_t cp_id, uint32_t flags, const char* src, int32_t src_len,
                                          uint16_t* dst, int32_t dst_len) {
    const CodePage* cp = code_page(cp_id);
    if (!cp || !src || src_len == 0 || dst_len < 0 || (dst_len && !dst) || (const void*)src == (const void*)dst) {
        fail(w32::ERR_INVALID_PARAMETER);
        return 0;
    }
    if ((flags & ~0xfu) || (flags & 3) == 3) {
        fail(ERR_INVALID_FLAGS);
        return 0;
    }
    const int32_t n = src_len < 0 ? (int32_t)strlen(src) + 1 : src_len;   // (any negative length: to the terminator)
    if (dst_len == 0) return n;                  // (one code unit a byte)
    for (int32_t i = 0; i < n; i++) {
        if (i >= dst_len) {
            fail(w32::ERR_INSUFFICIENT_BUFFER);
            return 0;
        }
        dst[i] = cp->to_u[(uint8_t)src[i]];
    }
    return n;
}

int32_t W32K_CALL w32_WideCharToMultiByte(uint32_t cp_id, uint32_t flags, const uint16_t* src, int32_t src_len,
                                          char* dst, int32_t dst_len, const char* default_char, int32_t* used_default) {
    const CodePage* cp = code_page(cp_id);
    if (!cp || !src || src_len == 0 || dst_len < 0 || (dst_len && !dst) || (const void*)src == (const void*)dst) {
        fail(w32::ERR_INVALID_PARAMETER);
        return 0;
    }
    // WC_COMPOSITECHECK 0x200, NO_BEST_FIT_CHARS 0x400; WC_DISCARDNS 0x10, SEPCHARS 0x20, DEFAULTCHAR 0x40 only with
    // WC_COMPOSITECHECK
    if ((flags & ~0x670u) || ((flags & 0x70) && !(flags & 0x200))) {
        fail(ERR_INVALID_FLAGS);
        return 0;
    }
    if (used_default) *used_default = 0;         // (set before converting: a failure leaves it FALSE)
    int32_t n = src_len;
    if (n < 0) {
        n = 0;
        while (src[n]) n++;
        n++;
    }
    const uint8_t def = default_char ? (uint8_t)*default_char : '?';
    const bool best_fit = !(flags & 0x400);
    bool used = false;
    int32_t out = 0;
    for (int32_t i = 0; i < n; i++) {
        const uint16_t u = src[i];               // (a surrogate pair: each half the default character)
        uint8_t b;
        if (!to_byte(cp, u, best_fit, &b)) {
            b = def;
            used = true;
        }
        if (dst_len) {
            if (out >= dst_len) {
                fail(w32::ERR_INSUFFICIENT_BUFFER);
                return 0;
            }
            dst[out] = (char)b;
        }
        out++;
    }
    if (used_default) *used_default = used;
    return out;
}

// ---- LCMapString / GetStringType -----------------------------------------------------------------------------------
namespace {
uint16_t to_lower(uint16_t u, bool ling) {
    const uint16_t l = ling ? mapped(k_ling_lower, sizeof k_ling_lower / sizeof k_ling_lower[0], u) : u;
    return l != u ? l : mapped(k_lower, sizeof k_lower / sizeof k_lower[0], u);
}
uint16_t to_upper(uint16_t u, bool ling) {
    const uint16_t l = ling ? mapped(k_ling_upper, sizeof k_ling_upper / sizeof k_ling_upper[0], u) : u;
    return l != u ? l : mapped(k_upper, sizeof k_upper / sizeof k_upper[0], u);
}
uint16_t to_title(uint16_t u, bool ling) {      // upper case, but the Latin digraphs' title forms (DZ+caron, LJ, NJ, DZ)
    if (u >= 0x1c4 && u <= 0x1cc) return (uint16_t)(0x1c5 + (u - 0x1c4) / 3 * 3);
    if (u >= 0x1f1 && u <= 0x1f3) return 0x1f2;
    return to_upper(u, ling);
}
bool word_char(uint16_t u) {         // letters, digits, apostrophes, and anything with a case (Roman numerals)
    return u == 0x27 || (ctype_run(u)[1] & 0x104) != 0 || to_upper(u, false) != u || to_lower(u, false) != u;
}
}  // namespace


int32_t W32K_CALL w32_LCMapStringW(uint32_t lcid, uint32_t flags, const uint16_t* src, int32_t src_len, uint16_t* dst,
                                   int32_t dst_len) {
    if (!src || src_len == 0 || dst_len < 0 || (dst_len && !dst)) {
        fail(w32::ERR_INVALID_PARAMETER);
        return 0;
    }
    (void)lcid;
    const bool ling = (flags & 0x01000000u) != 0;   // LCMAP_LINGUISTIC_CASING
    const uint32_t f = flags & ~0x01000000u;
    const uint32_t cs = f & 0x300;               // LCMAP_LOWERCASE 0x100, LCMAP_UPPERCASE 0x200, both: LCMAP_TITLECASE
    if (!f || (f & ~0xb00u)) {                   // (and LCMAP_BYTEREV 0x800); LCMAP_SORTKEY etc.: not covered
        fail(ERR_INVALID_FLAGS);
        return 0;
    }
    int32_t n = src_len;
    if (n < 0) {
        n = 0;
        while (src[n]) n++;
        n++;
    }
    if (dst_len == 0) return n;
    if (dst_len < n) {
        fail(w32::ERR_INSUFFICIENT_BUFFER);
        return 0;
    }
    for (int32_t i = 0; i < n; i++) {
        uint16_t u = src[i];
        if (cs == 0x100) u = to_lower(u, ling);
        else if (cs == 0x200) u = to_upper(u, ling);
        dst[i] = u;
    }
    if (cs == 0x300) {
        // LCMAP_TITLECASE, as Windows does it on the samples test/w32_kernel_test.cpp checks (every single character,
        // and words): a word (letters, digits, apostrophes, anything with a case) with a lower-case letter in it gets
        // its first letter in title case and the rest in lower case; a word without one (an acronym) stays as it is
        for (int32_t i = 0; i < n;) {
            if (!word_char(src[i])) { i++; continue; }
            int32_t j = i;
            bool lower = false;
            while (j < n && word_char(src[j])) lower |= to_upper(src[j], false) != src[j], j++;   // (it has an upper case)
            if (lower) {
                bool first = true;               // (the first letter, digits skipped: "2nd" -> "2Nd")
                for (int32_t k = i; k < j; k++) {
                    const bool letter = (ctype_run(src[k])[1] & 0x100) || to_upper(src[k], false) != src[k] ||
                                        to_lower(src[k], false) != src[k];
                    dst[k] = first && letter ? to_title(src[k], ling) : to_lower(src[k], ling);
                    if (letter) first = false;
                }
            }
            i = j;
        }
    }
    if (f & 0x800)
        for (int32_t i = 0; i < n; i++) dst[i] = (uint16_t)((dst[i] >> 8) | (dst[i] << 8));
    return n;
}

int32_t W32K_CALL w32_LCMapStringA(uint32_t lcid, uint32_t flags, const char* src, int32_t src_len, char* dst,
                                   int32_t dst_len) {
    if (!src || src_len == 0 || dst_len < 0 || (dst_len && !dst)) {
        fail(w32::ERR_INVALID_PARAMETER);
        return 0;
    }
    if (flags & 0x800) {                         // LCMAP_BYTEREV: wide characters only
        fail(ERR_INVALID_FLAGS);
        return 0;
    }
    const int32_t n = src_len < 0 ? (int32_t)strlen(src) + 1 : src_len;
    std::vector<uint16_t> w(n), m(n);
    w32_MultiByteToWideChar(0, 0, src, n, w.data(), n);
    const int32_t r = w32_LCMapStringW(lcid, flags, w.data(), n, m.data(), n);
    if (!r) return 0;
    if (dst_len == 0) return r;
    if (dst_len < r) {
        fail(w32::ERR_INSUFFICIENT_BUFFER);
        return 0;
    }
    return w32_WideCharToMultiByte(0, 0, m.data(), r, dst, dst_len, 0, 0);
}

int32_t W32K_CALL w32_GetStringTypeW(uint32_t info_type, const uint16_t* src, int32_t src_len, uint16_t* types) {
    const int k = info_type == 1 ? 1 : info_type == 2 ? 2 : info_type == 4 ? 3 : 0;   // CT_CTYPE1 / 2 / 3
    if (!src || !types || src_len == 0) {
        fail(w32::ERR_INVALID_PARAMETER);
        return 0;
    }
    if (!k) {
        fail(ERR_INVALID_FLAGS);
        return 0;
    }
    int32_t n = src_len;
    if (n < 0) {
        n = 0;
        while (src[n]) n++;
        n++;
    }
    for (int32_t i = 0; i < n; i++) types[i] = ctype_run(src[i])[k];
    return 1;
}

int32_t W32K_CALL w32_GetStringTypeA(uint32_t lcid, uint32_t info_type, const char* src, int32_t src_len,
                                     uint16_t* types) {
    (void)lcid;
    if (!src || !types || src_len == 0) {
        fail(w32::ERR_INVALID_PARAMETER);
        return 0;
    }
    const int32_t n = src_len < 0 ? (int32_t)strlen(src) + 1 : src_len;
    std::vector<uint16_t> w(n);
    w32_MultiByteToWideChar(0, 0, src, n, w.data(), n);
    return w32_GetStringTypeW(info_type, w.data(), n, types);
}

// ---- GetLocaleInfoA -------------------------------------------------------------------------------------------------
int32_t W32K_CALL w32_GetLocaleInfoA(uint32_t lcid, uint32_t type, char* buf, int32_t size) {
    if (size < 0 || (size && !buf) || !our_locale(lcid)) {
        fail(w32::ERR_INVALID_PARAMETER);
        return 0;
    }
    const bool number = (type & 0x20000000u) != 0;   // LOCALE_RETURN_NUMBER
    const LocaleString* e = locale_entry(type & ~0xe0000000u);   // (LOCALE_NOUSEROVERRIDE, LOCALE_USE_CP_ACP: the same)
    if (!e) {
        fail(ERR_INVALID_FLAGS);
        return 0;
    }
    if (number) {
        const char* p = e->text;
        uint32_t v = 0;
        if (!*p) { fail(ERR_INVALID_FLAGS); return 0; }
        for (; *p; p++) {
            if (*p < '0' || *p > '9') { fail(ERR_INVALID_FLAGS); return 0; }
            v = v * 10 + (uint32_t)(*p - '0');
        }
        if (size == 0) return 4;                 // (sizeof(DWORD) / sizeof(CHAR))
        if (size < 4) { fail(w32::ERR_INSUFFICIENT_BUFFER); return 0; }
        memcpy(buf, &v, 4);
        return 4;
    }
    // Windows' quirk, kept: the user default's LOCALE_SLANGUAGE (with user overrides) leaves the last error 0
    if (type == 2 && lcid == 0x400) w32::set_last_error(0);
    return give(e->text, e->n, buf, size);
}

// ---- GetCurrencyFormatA -----------------------------------------------------------------------------------------------
namespace {
struct CurrencyFmt {                             // CURRENCYFMTA
    uint32_t digits, leading_zero, grouping;
    const char* decimal;
    const char* thousand;
    uint32_t negative_order, positive_order;
    const char* symbol;
};

// "3;0" -> {3} repeating; "3;2;0" -> {3, 2} repeating the last; "3" -> {3} once. As CURRENCYFMT's Grouping:
// 3 = "3;0", 32 = "3;2;0", 30 = "3"
void grouping_of(uint32_t g, std::vector<int>* sizes, bool* repeat) {
    std::string d = std::to_string(g);
    sizes->clear();
    *repeat = true;
    if (d.size() > 1 && d.back() == '0') {
        *repeat = false;
        d.pop_back();
    }
    for (char c : d) sizes->push_back(c - '0');
}
uint32_t grouping_number(const char* s) {        // "3;0" -> 3, "3;2;0" -> 32, "3" -> 30
    uint32_t v = 0;
    bool last_zero = false;
    for (; *s; s++) {
        if (*s >= '0' && *s <= '9') {
            v = v * 10 + (uint32_t)(*s - '0');
            last_zero = *s == '0';
        }
    }
    if (last_zero) return v / 10;
    return v * 10;
}

std::string group(const std::string& digits, uint32_t grouping, const char* sep) {
    std::vector<int> sizes;
    bool repeat;
    grouping_of(grouping, &sizes, &repeat);
    if (sizes.empty() || sizes[0] == 0) return digits;
    std::string out;
    int i = (int)digits.size();
    size_t k = 0;
    std::vector<std::string> parts;
    while (i > 0) {
        int g;
        if (k < sizes.size()) g = sizes[k++];
        else if (repeat) g = sizes.back();
        else g = i;
        if (g <= 0) g = i;
        const int start = i - g > 0 ? i - g : 0;
        parts.push_back(digits.substr((size_t)start, (size_t)(i - start)));
        i = start;
    }
    for (size_t p = parts.size(); p-- > 0;) {
        out += parts[p];
        if (p) out += sep;
    }
    return out;
}
}  // namespace

int32_t W32K_CALL w32_GetCurrencyFormatA(uint32_t lcid, uint32_t flags, const char* value, const void* format,
                                         char* out, int32_t size) {
    if (size < 0 || (size && !out) || !value || !our_locale(lcid)) {
        fail(w32::ERR_INVALID_PARAMETER);
        return 0;
    }
    if (format ? flags != 0 : (flags & ~0x80000000u) != 0) {   // (only LOCALE_NOUSEROVERRIDE, and not with a format)
        fail(ERR_INVALID_FLAGS);
        return 0;
    }
    CurrencyFmt f;
    if (format) {
        const uint8_t* p = (const uint8_t*)format;   // CURRENCYFMTA, 32-bit: pointers are 4 bytes
        uint32_t w[8];
        memcpy(w, p, sizeof w);
        f.digits = w[0];
        f.leading_zero = w[1];
        f.grouping = w[2];
        f.decimal = (const char*)(uintptr_t)w[3];
        f.thousand = (const char*)(uintptr_t)w[4];
        f.negative_order = w[5];
        f.positive_order = w[6];
        f.symbol = (const char*)(uintptr_t)w[7];
        if (f.digits > 9 || f.leading_zero > 1 || f.negative_order > 15 || f.positive_order > 3 || !f.decimal ||
            !f.thousand || !f.symbol) {
            fail(w32::ERR_INVALID_PARAMETER);
            return 0;
        }
    } else {
        f.digits = (uint32_t)locale_int(0x19);           // LOCALE_ICURRDIGITS
        f.leading_zero = (uint32_t)locale_int(0x12);     // LOCALE_ILZERO
        f.grouping = grouping_number(locale_string(0x18));   // LOCALE_SMONGROUPING
        f.decimal = locale_string(0x16);                 // LOCALE_SMONDECIMALSEP
        f.thousand = locale_string(0x17);                // LOCALE_SMONTHOUSANDSEP
        f.negative_order = (uint32_t)locale_int(0x1c);   // LOCALE_INEGCURR
        f.positive_order = (uint32_t)locale_int(0x1b);   // LOCALE_ICURRENCY
        f.symbol = locale_string(0x14);                  // LOCALE_SCURRENCY
    }
    // the value: [-]digits[.digits] -- nothing else
    const char* p = value;
    const bool negative = *p == '-';
    if (negative) p++;
    std::string ip, fp;
    bool dot = false;
    for (; *p; p++) {
        if (*p >= '0' && *p <= '9') (dot ? fp : ip) += *p;
        else if (*p == '.' && !dot) dot = true;
        else {
            fail(w32::ERR_INVALID_PARAMETER);
            return 0;
        }
    }
    // ("", "-", "." are zero, as Windows takes them)
    // rounded to the digits (half up), padded with zeros
    bool carry = fp.size() > f.digits && fp[f.digits] >= '5';
    fp.resize(f.digits, '0');
    for (size_t i = fp.size(); carry && i-- > 0;) {
        if (fp[i] == '9') fp[i] = '0';
        else { fp[i]++; carry = false; }
    }
    for (size_t i = ip.size(); carry && i-- > 0;) {
        if (ip[i] == '9') ip[i] = '0';
        else { ip[i]++; carry = false; }
    }
    if (carry) ip.insert(ip.begin(), '1');
    size_t z = 0;
    while (z < ip.size() && ip[z] == '0') z++;
    ip.erase(0, z);
    const bool zero = ip.empty() && fp.find_first_not_of('0') == std::string::npos;
    if (ip.empty() && f.leading_zero) ip = "0";
    std::string num = group(ip, f.grouping, f.thousand);
    if (f.digits) num += std::string(f.decimal) + fp;
    const std::string sym = f.symbol, neg = locale_string(0x51) ? locale_string(0x51) : "-";
    std::string s;
    if (!negative || zero) {                     // (a negative zero, "-0.001" too, is positive)
        switch (f.positive_order) {
        case 0: s = sym + num; break;
        case 1: s = num + sym; break;
        case 2: s = sym + " " + num; break;
        default: s = num + " " + sym; break;
        }
    } else {
        switch (f.negative_order) {
        case 0: s = "(" + sym + num + ")"; break;
        case 1: s = neg + sym + num; break;
        case 2: s = sym + neg + num; break;
        case 3: s = sym + num + neg; break;
        case 4: s = "(" + num + sym + ")"; break;
        case 5: s = neg + num + sym; break;
        case 6: s = num + neg + sym; break;
        case 7: s = num + sym + neg; break;
        case 8: s = neg + num + " " + sym; break;
        case 9: s = neg + sym + " " + num; break;
        case 10: s = num + " " + sym + neg; break;
        case 11: s = sym + " " + num + neg; break;
        case 12: s = sym + " " + neg + num; break;
        case 13: s = num + neg + " " + sym; break;
        case 14: s = "(" + sym + " " + num + ")"; break;
        default: s = "(" + num + " " + sym + ")"; break;
        }
    }
    return give(s, out, size);
}

// ---- GetDateFormatA -------------------------------------------------------------------------------------------------
namespace {
bool leap(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }
int days_in(int y, int m) {
    static const int d[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return m == 2 && leap(y) ? 29 : d[m - 1];
}
int day_of_week(int y, int m, int d) {           // 0 = Sunday (Sakamoto's)
    static const int t[12] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    if (m < 3) y--;
    return (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;
}
std::string num(int v, int width) {
    std::string s = std::to_string(v);
    while ((int)s.size() < width) s.insert(s.begin(), '0');
    return s;
}
}  // namespace

int32_t W32K_CALL w32_GetDateFormatA(uint32_t lcid, uint32_t flags, const W32SystemTime* date, const char* format,
                                     char* out, int32_t size) {
    if (size < 0 || (size && !out) || !our_locale(lcid)) {
        fail(w32::ERR_INVALID_PARAMETER);
        return 0;
    }
    // DATE_SHORTDATE 1, LONGDATE 2, USE_ALT_CALENDAR 4 (Gregorian anyway), YEARMONTH 8, AUTOLAYOUT 0x40; Windows
    // refuses LTRREADING 0x10, RTLREADING 0x20 and MONTHDAY 0x80 for en-US
    const uint32_t kind = flags & 0xb;
    if ((flags & ~0x8000004fu) || (kind & (kind - 1)) || (format && kind) || (format && (flags & 0x80000000u))) {
        fail(ERR_INVALID_FLAGS);
        return 0;
    }
    W32SystemTime st;
    if (date) st = *date;
    else w32_GetLocalTime(&st);
    if (st.month < 1 || st.month > 12 || st.year < 1601 || st.year > 30827 || st.day < 1 ||
        st.day > days_in(st.year, st.month)) {  // (the time of day isn't looked at)
        fail(w32::ERR_INVALID_PARAMETER);
        return 0;
    }
    const char* pic = format;
    if (!pic) {
        uint32_t t = 0x1f;                       // LOCALE_SSHORTDATE
        if (kind == 2) t = 0x20;                 // LOCALE_SLONGDATE
        else if (kind == 8) t = 0x1006;          // LOCALE_SYEARMONTH
        pic = locale_string(t);
        if (!pic) {
            fail(ERR_INVALID_FLAGS);
            return 0;
        }
    }
    const int dow = day_of_week(st.year, st.month, st.day);   // (the date's own, not wDayOfWeek)
    const bool lrm = (flags & 0x40) != 0;        // DATE_AUTOLAYOUT: a left-to-right mark before each element ('?' in 1252)
    std::string s;
    for (const char* p = pic; *p;) {
        const char c = *p;
        if (lrm && (c == '\'' || c == 'd' || c == 'M' || c == 'y' || c == 'g')) s += '?';
        if (c == '\'') {                         // 'quoted text'; '' is a quote
            p++;
            while (*p) {
                if (*p == '\'') {
                    if (p[1] == '\'') { s += '\''; p += 2; continue; }
                    p++;
                    break;
                }
                s += *p++;
            }
            continue;
        }
        int n = 0;
        while (p[n] == c) n++;
        if (c == 'd') {
            if (n == 1) s += num(st.day, 1);
            else if (n == 2) s += num(st.day, 2);
            else if (n == 3) s += locale_string(0x31 + (dow + 6) % 7);   // LOCALE_SABBREVDAYNAME1 = Monday
            else s += locale_string(0x2a + (dow + 6) % 7);               // LOCALE_SDAYNAME1
        } else if (c == 'M') {
            if (n == 1) s += num(st.month, 1);
            else if (n == 2) s += num(st.month, 2);
            else if (n == 3) s += locale_string(0x44 + st.month - 1);    // LOCALE_SABBREVMONTHNAME1
            else s += locale_string(0x38 + st.month - 1);                // LOCALE_SMONTHNAME1
        } else if (c == 'y') {
            if (n == 1) s += num(st.year % 100, 1);
            else if (n == 2) s += num(st.year % 100, 2);
            else s += num(st.year, 4);
        } else if (c == 'g') {
            s += "A.D.";
        } else {
            s.append((size_t)n, c);
        }
        p += n;
    }
    return give(s, out, size);
}
