#include <include/wally_bip39.h>
#include <string.h>
#include "internal.h"
#include "mnemonic.h"
#include "wordlist.h"
#include "hmac.h"
#include "pbkdf2.h"
#include "ccan/ccan/crypto/sha256/sha256.h"
#include "ccan/ccan/crypto/sha512/sha512.h"

#include "data/wordlists/chinese_simplified.c"
#include "data/wordlists/chinese_traditional.c"
#include "data/wordlists/english.c"
#include "data/wordlists/french.c"
#include "data/wordlists/italian.c"
#include "data/wordlists/spanish.c"
#include "data/wordlists/japanese.c"


static const struct {
    const char name[4];
    const struct words *words;
} lookup[] = {
    { "en", &en_words}, { "es", &es_words}, { "fr", &fr_words},
    { "it", &it_words}, { "jp", &jp_words}, { "zhs", &zhs_words},
    { "zht", &zht_words},
    /* FIXME: Should 'zh' map to traditional or simplified? */
};

int bip39_get_languages(char **output)
{
    if (!output)
        return WALLY_EINVAL;
    *output = strdup("en es fr it jp zhs zht");
    return *output ? WALLY_OK : WALLY_ENOMEM;
}

int bip39_get_wordlist(const char *lang, const struct words **output)
{
    size_t i;

    if (!output)
        return WALLY_EINVAL;

    *output = &en_words; /* Fallback to English if not found */

    if (lang)
        for (i = 0; i < sizeof(lookup) / sizeof(lookup[0]); ++i)
            if (!strcmp(lang, lookup[i].name)) {
                *output = lookup[i].words;
                break;
            }
    return WALLY_OK;
}

int bip39_get_word(const struct words *w, size_t index,
                   char **output)
{
    const char *word;

    if (output)
        *output = NULL;

    if (!w || !output || !(word = wordlist_lookup_index(w, index)))
        return WALLY_EINVAL;

    *output = word ? strdup(word) : NULL;
    return *output ? WALLY_OK : WALLY_ENOMEM;
}

/* Convert an input entropy length to a mask for checksum bits. As it
 * returns 0 for bad lengths, it serves as a validation function too.
 */
static size_t len_to_mask(size_t len)
{
    switch (len) {
    case BIP39_ENTROPY_LEN_128: return 0xf0;
    case BIP39_ENTROPY_LEN_160: return 0xf8;
    case BIP39_ENTROPY_LEN_192: return 0xfc;
    case BIP39_ENTROPY_LEN_224: return 0xfe;
    case BIP39_ENTROPY_LEN_256: return 0xff;
    }
    return 0;
}

static unsigned char bip39_checksum(const unsigned char *bytes_in, size_t len)
{
    struct sha256 sha;
    unsigned char ret;
    sha256(&sha, bytes_in, len);
    ret = sha.u.u8[0];
    clear(&sha, sizeof(sha));
    return ret;
}

int bip39_mnemonic_from_bytes(const struct words *w,
                              const unsigned char *bytes_in, size_t len,
                              char **output)
{
    /* 128 to 256 bits of entropy require 4-8 bits of checksum */
    unsigned char checksummed_bytes[BIP39_ENTROPY_LEN_256 + sizeof(unsigned char)];

    if (output)
        *output = NULL;

    if (!bytes_in || !len || !output)
        return WALLY_EINVAL;

    w = w ? w : &en_words;

    if (w->bits != 11u || !len_to_mask(len))
        return WALLY_EINVAL;

    memcpy(checksummed_bytes, bytes_in, len);
    checksummed_bytes[len] = bip39_checksum(bytes_in, len);;
    *output = mnemonic_from_bytes(w, checksummed_bytes, len + 1);
    clear(checksummed_bytes, sizeof(checksummed_bytes));
    return *output ? WALLY_OK : WALLY_ENOMEM;
}

static bool checksum_ok(const unsigned char *bytes, size_t idx, size_t mask)
{
    /* The checksum is stored after the data to sum */
    return (bytes[idx] & mask) == (bip39_checksum(bytes, idx) & mask);
}

int bip39_mnemonic_to_bytes(const struct words *w, const char *mnemonic,
                            unsigned char *bytes_out, size_t len,
                            size_t *written)
{
    unsigned char tmp_bytes[BIP39_ENTROPY_LEN_256 + sizeof(unsigned char)];
    size_t mask, tmp_len;
    int ret;

    /* Ideally we would infer the wordlist here. Unfortunately this cannot
     * work reliably because the default word lists overlap. In combination
     * with being sorted lexographically, this means the default lists
     * were poorly chosen. But we are stuck with them now.
     *
     * If the caller doesn't know which word list to use, they should iterate
     * over the available ones and try any resulting list that the mnemonic
     * validates against.
     */
    w = w ? w : &en_words;

    if (written)
        *written = 0;

    if (w->bits != 11u || !mnemonic || !bytes_out)
        return WALLY_EINVAL;

    ret = mnemonic_to_bytes(w, mnemonic, tmp_bytes, sizeof(tmp_bytes), &tmp_len);

    if (!ret) {
        if (tmp_len > sizeof(tmp_bytes))
            ret = WALLY_EINVAL; /* Too big for biggest supported entropy */
        else {
            --tmp_len; /* Ignore checksum byte */
            if (tmp_len <= len) {
                if (!(mask = len_to_mask(tmp_len)) ||
                    !checksum_ok(tmp_bytes, tmp_len, mask)) {
                    tmp_len = 0;
                    ret = WALLY_EINVAL; /* Bad checksum */
                }
                else
                    memcpy(bytes_out, tmp_bytes, tmp_len);
            }
        }
    }

    clear(tmp_bytes, sizeof(tmp_bytes));
    if (!ret && written)
        *written = tmp_len;
    return ret;
}

int bip39_mnemonic_validate(const struct words *w, const char *mnemonic)
{
    unsigned char buf[BIP39_ENTROPY_LEN_256 + sizeof(unsigned char)];
    size_t len;
    int ret = bip39_mnemonic_to_bytes(w, mnemonic, buf, sizeof(buf), &len);
    clear(buf, sizeof(buf));
    return ret;
}

int  bip39_mnemonic_to_seed(const char *mnemonic, const char *password,
                            unsigned char *bytes_out, size_t len,
                            size_t *written)
{
    const size_t bip9_cost = 2048u;
    const char *prefix = "mnemonic";
    const size_t prefix_len = strlen(prefix);
    const size_t password_len = password ? strlen(password) : 0;
    const size_t salt_len = prefix_len + password_len + PBKDF2_HMAC_EXTRA_LEN;
    unsigned char *salt;
    int ret;

    if (written)
        *written = 0;

    if (!mnemonic || !password || !bytes_out || len != BIP39_SEED_LEN_512)
        return WALLY_EINVAL;

    salt = malloc(salt_len);
    if (!salt)
        return WALLY_ENOMEM;

    memcpy(salt, prefix, prefix_len);
    memcpy(salt + prefix_len, password, password_len);

    ret = pbkdf2_hmac_sha512((unsigned char *)mnemonic, strlen(mnemonic),
                             salt, salt_len, PBKDF2_HMAC_FLAG_BLOCK_RESERVED,
                             bip9_cost, bytes_out, len);

    if (!ret && written)
        *written = BIP39_SEED_LEN_512; /* Succeeded */

    clear(salt, salt_len);
    free(salt);

    return ret;
}
