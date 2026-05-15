/** @file src/chalk/lang_table.c — extension/shebang → comment-prefix
 *        table for the source codec.
 *
 *  Ported verbatim from chalk's base config
 *  (chalk/src/configs/chalk.c42spec):
 *   - field language_to_comment_map (line 2937)
 *   - field extensions_to_languages_map (line 2960)
 *
 *  Two stages:
 *    1. shebang interpreter or file extension → language name
 *    2. language name → comment prefix (# or //)
 */

#include "n00b.h"
#include "internal/chalk/lang_table.h"

#include <string.h>

// -----------------------------------------------------------------------
// Stage 1: extension or interpreter → language
// -----------------------------------------------------------------------

typedef struct {
    const char *key;   // extension or interpreter binary name (no dot)
    const char *lang;
} kv_t;

// Combined: chalk's extensions_to_languages_map + interpreter names
// (each language is also reachable by its own binary name from
// language_to_comment_map, so we include both forms).
static const kv_t k_ext_to_lang[] = {
    {"awk",         "awk"},
    {"csh",         "csh"},
    {"gawk",        "gawk"},
    {"ksh",         "ksh"},
    {"mawk",        "mawk"},
    {"nawk",        "nawk"},
    {"bones",       "node"}, {"cjs", "node"},  {"es6", "node"},
    {"jakefile",    "node"}, {"jake", "node"}, {"jsb", "node"},
    {"jscad",       "node"}, {"jsfl", "node"}, {"jsm", "node"},
    {"_js",         "node"}, {"js",   "node"}, {"jss", "node"},
    {"mjs",         "node"}, {"njs",  "node"}, {"pac", "node"},
    {"sjs",         "node"}, {"ssjs", "node"}, {"xsjslib", "node"},
    {"xsjs",        "node"},
    {"ack",         "perl"}, {"al",   "perl"}, {"cpanfile", "perl"},
    {"perl",        "perl"}, {"ph",   "perl"}, {"plh",   "perl"},
    {"pl",          "perl"}, {"plx",  "perl"}, {"pm",    "perl"},
    {"psgi",        "perl"}, {"rexfile", "perl"},
    {"aw",          "php"},  {"ctp",  "php"},  {"dist",  "php"},
    {"phakefile",   "php"},  {"php3", "php"},  {"php4",  "php"},
    {"php5",        "php"},  {"php_cs", "php"},{"php",   "php"},
    {"phps",        "php"},  {"phpt", "php"},  {"phtml", "php"},
    {"bazel",       "python"}, {"buck", "python"}, {"gclient", "python"},
    {"gypi",        "python"}, {"gyp",  "python"}, {"lmi",     "python"},
    {"py3",         "python"}, {"pyde", "python"}, {"pyi",     "python"},
    {"pyp",         "python"}, {"py",   "python"}, {"pyt",     "python"},
    {"pyw",         "python"}, {"sconscript", "python"},
    {"sconstruct",  "python"}, {"snakefile",  "python"},
    {"tac",         "python"}, {"workspace",  "python"},
    {"wscript",     "python"}, {"wsgi",       "python"},
    {"xpy",         "python"},
    {"appraisals",  "ruby"}, {"berksfile",  "ruby"}, {"brewfile", "ruby"},
    {"builder",     "ruby"}, {"buildfile",  "ruby"}, {"capfile",  "ruby"},
    {"dangerfile",  "ruby"}, {"deliverfile","ruby"}, {"eye",      "ruby"},
    {"fastfile",    "ruby"}, {"gemfile.lock","ruby"},{"gemfile",  "ruby"},
    {"gemspec",     "ruby"}, {"god",        "ruby"}, {"guardfile","ruby"},
    {"irbrc",       "ruby"}, {"jarfile",    "ruby"}, {"jbuilder", "ruby"},
    {"mavenfile",   "ruby"}, {"mspec",      "ruby"}, {"podfile",  "ruby"},
    {"podspec",     "ruby"}, {"pryrc",      "ruby"}, {"puppetfile","ruby"},
    {"rabl",        "ruby"}, {"rake",       "ruby"}, {"rb",       "ruby"},
    {"rbuild",      "ruby"}, {"rbw",        "ruby"}, {"rbx",      "ruby"},
    {"ru",          "ruby"}, {"snapfile",   "ruby"}, {"thorfile", "ruby"},
    {"thor",        "ruby"}, {"vagrantfile","ruby"}, {"watchr",   "ruby"},
    {"bash",        "sh"},   {"fish", "sh"},  {"sh", "sh"},
    {"itk",         "tcl"},  {"tcl",  "tcl"}, {"tk", "tcl"},
    {"tcsh",        "tcsh"},
    {"hcl",         "terraform"}, {"nomad", "terraform"}, {"tf", "terraform"},
    {"zsh",         "zsh"},
    {"expect",      "expect"},
    {"tcsh",        "tcsh"},
};
static const size_t k_ext_to_lang_len
    = sizeof(k_ext_to_lang) / sizeof(k_ext_to_lang[0]);

// -----------------------------------------------------------------------
// Stage 2: language → comment prefix
// -----------------------------------------------------------------------

typedef struct {
    const char *lang;
    const char *comment;
} lc_t;

static const lc_t k_lang_to_comment[] = {
    {"sh",        "#"},
    {"csh",       "#"},
    {"tcsh",      "#"},
    {"ksh",       "#"},
    {"zsh",       "#"},
    {"terraform", "//"},
    {"node",      "//"},
    {"php",       "//"},
    {"perl",      "#"},
    {"python",    "#"},
    {"ruby",      "#"},
    {"expect",    "#"},
    {"tcl",       "#"},
    {"ack",       "#"},
    {"awk",       "#"},
    {"gawk",      "#"},
    {"mawk",      "#"},
    {"nawk",      "#"},
};
static const size_t k_lang_to_comment_len
    = sizeof(k_lang_to_comment) / sizeof(k_lang_to_comment[0]);

// -----------------------------------------------------------------------
// Public API
// -----------------------------------------------------------------------

static int
ascii_icmp(const char *a, size_t alen, const char *b)
{
    size_t i = 0;
    while (i < alen && b[i]) {
        char ca = a[i], cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca + 32);
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb + 32);
        if (ca != cb) return ca < cb ? -1 : 1;
        i++;
    }
    if (i == alen && !b[i]) return 0;
    return i == alen ? -1 : 1;
}

const char *
n00b_chalk_lang_lookup_ext(const char *ext, size_t ext_len)
{
    for (size_t i = 0; i < k_ext_to_lang_len; i++) {
        if (ascii_icmp(ext, ext_len, k_ext_to_lang[i].key) == 0) {
            return k_ext_to_lang[i].lang;
        }
    }
    return nullptr;
}

const char *
n00b_chalk_lang_lookup_comment(const char *lang)
{
    for (size_t i = 0; i < k_lang_to_comment_len; i++) {
        if (strcmp(lang, k_lang_to_comment[i].lang) == 0) {
            return k_lang_to_comment[i].comment;
        }
    }
    return nullptr;
}
