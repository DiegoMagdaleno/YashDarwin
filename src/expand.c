/* Yash: yet another shell */
/* expand.c: word expansion */
/* (C) 2007-2019 magicant */

/* This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.  */


#include "common.h"
#include "expand.h"
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wchar.h>
#include <wctype.h>
#include "arith.h"
#include "exec.h"
#include "input.h"
#include "option.h"
#include "parser.h"
#include "path.h"
#include "plist.h"
#include "sig.h"
#include "strbuf.h"
#include "util.h"
#include "variable.h"
#include "xfnmatch.h"
#include "yash.h"


/* characters that have special meanings in brace expansion, quote removal, and
 * globbing. When an unquoted expansion includes these characters, they are
 * backslashed to protect from unexpected side effects in succeeding expansion
 * steps. */
#define CHARS_ESCAPED L"\\\"\'{,}"

static bool expand_and_split_words(
	const wordunit_T *restrict w, plist_T *restrict list)
    __attribute__((nonnull(2)));

/* data passed between expansion functions */
struct expand_four_T {
    plist_T *valuelist, *splitlist;
    xwcsbuf_T valuebuf;
    xstrbuf_T splitbuf;
    bool zeroword;
};
/* When "$@" appears during expansion and there is no positional parameter, the
 * `zeroword' flag is set so that the quoted empty word can be removed later. */

static bool expand_four_and_remove_quotes(
	const wordunit_T *restrict w,
	tildetype_T tilde, bool processquotes, bool escapeall,
	plist_T *restrict valuelist)
    __attribute__((nonnull(5)));
static bool expand_four(const wordunit_T *restrict w,
	tildetype_T tilde, bool processquotes, bool escapeall, bool rec,
	struct expand_four_T *restrict e)
    __attribute__((nonnull(6)));
static void fill_splitbuf(struct expand_four_T *e, bool splittable)
    __attribute__((nonnull));

static wchar_t *expand_tilde(const wchar_t **ss,
	bool hasnextwordunit, tildetype_T tt)
    __attribute__((nonnull,malloc,warn_unused_result));

enum indextype_T { IDX_NONE, IDX_ALL, IDX_CONCAT, IDX_NUMBER, };

static bool expand_param(const paramexp_T *restrict p, bool indq,
	struct expand_four_T *restrict e)
    __attribute__((nonnull));
static enum indextype_T parse_indextype(const wchar_t *indexstr)
    __attribute__((nonnull,pure));
static wchar_t *trim_wstring(wchar_t *s, ssize_t startindex, ssize_t endindex)
    __attribute__((nonnull));
static void **trim_array(void **a, ssize_t startindex, ssize_t endindex)
    __attribute__((nonnull));
static void print_subst_as_error(const paramexp_T *p)
    __attribute__((nonnull));
static void match_each(void **restrict slist, const wchar_t *restrict pattern,
	paramexptype_T type)
    __attribute__((nonnull));
static void subst_each(void **restrict slist, const wchar_t *pattern,
	const wchar_t *subst, paramexptype_T type)
    __attribute__((nonnull));
static wchar_t *concatenate_values(void **values, bool escape)
    __attribute__((nonnull,malloc,warn_unused_result));
static void **concatenate_values_into_array(void **values, bool escape)
    __attribute__((nonnull,malloc,warn_unused_result));
static void subst_length_each(void **slist)
    __attribute__((nonnull));

static void expand_brace_each(void **restrict values, void **restrict splits,
	plist_T *restrict valuelist, plist_T *restrict splitlist)
    __attribute__((nonnull));
static void expand_brace(wchar_t *restrict word, char *restrict split,
	plist_T *restrict valuelist, plist_T *restrict splitlist)
    __attribute__((nonnull));
static bool try_expand_brace_sequence(
	wchar_t *word, char *restrict split, wchar_t *startc,
	plist_T *restrict valuelist, plist_T *restrict splitlist)
    __attribute__((nonnull));
static bool has_leading_zero(const wchar_t *restrict s, bool *restrict sign)
    __attribute__((nonnull));

static void fieldsplit_all(void **restrict valuelist, void **restrict splitlist,
	plist_T *restrict dest)
    __attribute__((nonnull));
static void fieldsplit(wchar_t *restrict s, char *restrict split,
	const wchar_t *restrict ifs, plist_T *restrict dest)
    __attribute__((nonnull));
static size_t skip_ifs(const wchar_t *s, const char *split,
	bool escaped, const wchar_t *ifs)
    __attribute__((nonnull,pure));
static size_t skip_ifs_whitespaces(const wchar_t *s, const char *split,
	bool escaped, const wchar_t *ifs)
    __attribute__((nonnull,pure));
static size_t skip_field(const wchar_t *s, const char *split,
	bool escaped, const wchar_t *ifs)
    __attribute__((nonnull,pure));
static void add_empty_field(plist_T *dest, const wchar_t *p)
    __attribute__((nonnull));

static inline void add_sq(
	const wchar_t *restrict *ss, xwcsbuf_T *restrict buf, bool escape)
    __attribute__((nonnull));
static wchar_t *escaped_wcspbrk(const wchar_t *s, const wchar_t *accept)
    __attribute__((nonnull));
static wchar_t *escaped_remove(const wchar_t *s, const wchar_t *reject)
    __attribute__((nonnull,malloc,warn_unused_result));
static inline wchar_t *escaped_remove_free(wchar_t *s, const wchar_t *reject)
    __attribute__((nonnull,malloc,warn_unused_result));

static void glob_all(void **restrict patterns, plist_T *restrict list)
    __attribute__((nonnull));
static enum wglobflags_T get_wglobflags(void)
    __attribute__((pure));

static void maybe_exit_on_error(void);


/********** Entry Points **********/

/* Expands a command line.
 * `args' is a NULL-terminated array of pointers to `const wordunit_T'
 * to expand.
 * If successful, the number of resulting words is assigned to `*argcp', a
 * pointer to a newly malloced array of the expanded words is assigned to
 * `*argvp', and true is returned. The array is NULL-terminated and its elements
 * are newly malloced wide strings.
 * If unsuccessful, false is returned and the values of `*argcp' and `*argvp'
 * are indeterminate.
 * On error in a non-interactive shell, the shell exits. */
bool expand_line(void *const *restrict args,
    int *restrict argcp, void ***restrict argvp)
{
    plist_T list;
    pl_init(&list);

    for (; *args != NULL; args++) {
	if (!expand_multiple(*args, &list)) {
	    plfree(pl_toary(&list), free);
	    return false;
	}
    }

    *argcp = list.length;
    *argvp = pl_toary(&list);
    return true;
}

/* Expands a word.
 * The results, which are added to `list' as newly-malloced wide strings, may
 * be multiple words.
 * The return value is true iff successful.
 * On error in a non-interactive shell, the shell exits. */
bool expand_multiple(const wordunit_T *w, plist_T *list)
{
    plist_T templist;

    /* four expansions, brace expansions and field splitting */
    if (!expand_and_split_words(w, pl_init(&templist))) {
	maybe_exit_on_error();
	plfree(pl_toary(&templist), free);
	return false;
    }

    /* glob */
    if (shopt_glob) {
	glob_all(pl_toary(&templist), list);
    } else {
	for (size_t i = 0; i < templist.length; i++)
	    pl_add(list, unescapefree(templist.contents[i]));
	pl_destroy(&templist);
    }

    return true;
}

/* Performs the four expansions, brace expansion and field splitting in a word.
 * The four expansions are tilde expansion, parameter expansion, command
 * substitution and arithmetic expansion.
 * Returns true iff successful. The resulting words are added to `list', which
 * may include backslash escapes.
 * Tilde expansion is performed with TT_SINGLE. */
bool expand_and_split_words(
	const wordunit_T *restrict w, plist_T *restrict list)
{
    plist_T valuelist1, valuelist2, splitlist1, splitlist2;
    pl_init(&valuelist1);
    pl_init(&splitlist1);

    struct expand_four_T expand;
    expand.valuelist = &valuelist1;
    expand.splitlist = &splitlist1;
    wb_init(&expand.valuebuf);
    sb_init(&expand.splitbuf);
    expand.zeroword = false;

    /* four expansions (w -> list1) */
    if (!expand_four(w, TT_SINGLE, true, false, false, &expand)) {
	plfree(pl_toary(&valuelist1), free);
	plfree(pl_toary(&splitlist1), free);
	wb_destroy(&expand.valuebuf);
	sb_destroy(&expand.splitbuf);
	return false;
    }
    assert(expand.valuebuf.length == expand.splitbuf.length);
    pl_add(expand.valuelist, wb_towcs(&expand.valuebuf));
    pl_add(expand.splitlist, sb_tostr(&expand.splitbuf));

    /* brace expansion (list1 -> list2) */
    if (shopt_braceexpand) {
	pl_init(&valuelist2);
	pl_init(&splitlist2);
	expand_brace_each(valuelist1.contents, splitlist1.contents,
		&valuelist2, &splitlist2);
	pl_destroy(&valuelist1);
	pl_destroy(&splitlist1);
    } else {
	valuelist2 = valuelist1;
	splitlist2 = splitlist1;
    }

    /* field splitting (list2 -> list) */
    size_t oldlength = list->length;
    fieldsplit_all(pl_toary(&valuelist2), pl_toary(&splitlist2), list);
    assert(oldlength <= list->length);

    /* empty field removal */
    if (list->length - oldlength == 1) {
	wchar_t *field = list->contents[oldlength];
	if (field[0] == L'\0' ||
		(expand.zeroword && wcscmp(field, L"\"\"") == 0)) {
	    free(field);
	    pl_remove(list, oldlength, 1);
	}
    }

    /* quote removal */
    for (size_t i = oldlength; i < list->length; i++)
	list->contents[i] = escaped_remove_free(list->contents[i], L"\"\'");

    return true;
}

/* Expands a single word: the four expansions and quote removal.
 * This function doesn't perform brace expansion, field splitting, globbing and
 * unescaping.
 * If `processquotes' is true, single- and double-quotations are recognized as
 * quotes. Otherwise, they are treated like backslashed characters.
 * If `escapeall' is true, the expanded words are all backslashed as if the
 * entire expansion is quoted.
 * If `processquotes' and `escapeall' are false, only backslashes not preceding
 * any of $, `, \ are self-backslashed.
 * If successful, the resulting word is returned as a newly malloced string
 * that may include backslash escapes.
 * On error, an error message is printed and NULL is returned.
 * On error in a non-interactive shell, the shell exits. */
wchar_t *expand_single(const wordunit_T *arg,
	tildetype_T tilde, bool processquotes, bool escapeall)
{
    plist_T list;
    pl_init(&list);

    if (!expand_four_and_remove_quotes(
		arg, tilde, processquotes, escapeall, &list)) {
	maybe_exit_on_error();
	plfree(pl_toary(&list), free);
	return NULL;
    }

    return concatenate_values(pl_toary(&list), true);
}

/* Like `expand_single', but the result is unescaped (if successful). */
wchar_t *expand_single_and_unescape(const wordunit_T *arg,
	tildetype_T tilde, bool processquotes, bool escapeall)
{
    wchar_t *result = expand_single(arg, tilde, processquotes, escapeall);
    return result == NULL ? NULL : unescapefree(result);
}

/* Expands a single word: the four expansions, glob, quote removal and unescape.
 * This function doesn't perform brace expansion and field splitting.
 * If the result of glob is more than one word,
 *   - returns the pre-glob pattern string if in the POSIXly correct mode
 *   - treats as an error otherwise.
 * If the "glob" shell option is off, glob is not performed.
 * The "nullglob" shell option is ignored.
 * If successful, the resulting word is returned as a newly malloced string.
 * On error, an error message is printed and NULL is returned.
 * On error in a non-interactive shell, the shell exits. */
char *expand_single_with_glob(const wordunit_T *arg, tildetype_T tilde)
{
    wchar_t *exp = expand_single(arg, tilde, true, false);
    char *result;

    if (exp == NULL)
	return NULL;

    /* glob */
    if (shopt_glob && is_pathname_matching_pattern(exp)) {
	plist_T list;
	bool ok;

	pl_init(&list);
	set_interruptible_by_sigint(true);
	ok = wglob(exp, get_wglobflags(), &list);
	set_interruptible_by_sigint(false);
	if (!ok) {
	    free(exp);
	    plfree(pl_toary(&list), free);
	    xerror(EINTR, Ngt("redirection"));
	    result = NULL;
	} else if (list.length == 1) {
	    free(exp);
	    result = realloc_wcstombs(list.contents[0]);
	    if (result == NULL)
		xerror(EILSEQ, Ngt("redirection"));
	    pl_destroy(&list);
	} else {
	    plfree(pl_toary(&list), free);
	    if (posixly_correct) {
		goto noglob;
	    } else {
		exp = unescapefree(exp);
		xerror(0, Ngt("filename `%ls' matches more than one file"),
			exp);
		free(exp);
		result = NULL;
	    }
	}
    } else {
noglob:
	result = realloc_wcstombs(unescapefree(exp));
	if (result == NULL)
	    xerror(EILSEQ, Ngt("redirection"));
    }
    return result;
}


/********** Four Expansions **********/

/* Performs the four expansions in the specified single word.
 * `w' is the word in which expansions occur.
 * `tilde' is type of tilde expansion that is performed.
 * If `processquotes' is true, single- and double-quotations are recognized as
 * quotes. Otherwise, they are treated like backslashed characters.
 * If `escapeall' is true, the expanded words are all backslashed as if the
 * entire expansion is quoted.
 * If `processquotes' and `escapeall' are false, only backslashes not preceding
 * any of $, `, \ are self-backslashed.
 * The expanded word is added to `valuelist' as a newly malloced wide string.
 * Single- or double-quoted characters are unquoted and backslashed.
 * In most cases, one string is added to `valuelist'. If the word contains "$@",
 * however, any number of strings may be added.
 * The return value is true iff successful. */
bool expand_four_and_remove_quotes(
	const wordunit_T *restrict w,
	tildetype_T tilde, bool processquotes, bool escapeall,
	plist_T *restrict valuelist)
{
    size_t oldlength = valuelist->length;
    struct expand_four_T expand;

    expand.valuelist = valuelist;
    wb_init(&expand.valuebuf);
    expand.splitlist = NULL;
    expand.zeroword = false;

    bool ok = expand_four(w, tilde, processquotes, escapeall, false, &expand);

    /* remove empty word for "$@" if $# == 0 */
    if (valuelist->length == oldlength && expand.zeroword &&
	    wcscmp(expand.valuebuf.contents, L"\"\"") == 0)
	wb_destroy(&expand.valuebuf);
    else
	pl_add(valuelist, wb_towcs(&expand.valuebuf));

    /* quote removal */
    for (size_t i = oldlength; i < valuelist->length; i++)
	valuelist->contents[i] =
	    escaped_remove_free(valuelist->contents[i], L"\"\'");

    return ok;
}

/* Performs the four expansions in the specified single word.
 * The four expansions are tilde expansion, parameter expansion, command
 * substitution, and arithmetic expansion.
 * `w' is the word in which expansions occur.
 * `tilde' specifies the type of tilde expansion that is performed.
 * If `processquotes' is true, single- and double-quotations are recognized as
 * quotes. Otherwise, they are treated like backslashed characters.
 * If `escapeall' is true, the expanded words are all backslashed as if the
 * entire expansion is quoted.
 * If `processquotes' and `escapeall' are false, only backslashes not preceding
 * any of $, `, \ are self-backslashed.
 * `rec' must be true iff this expansion is part of another expansion.
 * `e->valuebuf' must be initialized before calling this function and is used to
 * expand the current word. If `w' expands to multiple words, the last word is
 * put in `e->valuebuf' and the others are inserted to `e->valuelist'.
 * The splittability strings are put in `e->splitbuf' and `e->splitlist'
 * accordingly if `e->splitlist' is non-NULL.
 * Single- and double-quotations remain in the resulting word. In addition,
 * characters inside those quotations are backslashed.
 * The return value is true iff successful. */
/* A splittability string is an array of Boolean values that specifies where
 * the word can be split in field splitting. The word can be split at the nth
 * character iff the nth value of the splittability string is non-zero. */
bool expand_four(const wordunit_T *restrict w,
	tildetype_T tilde, bool processquotes, bool escapeall, bool rec,
	struct expand_four_T *restrict e)
{
    bool ok = true;
    bool indq = false;  /* in a double quote? */
    bool first = true;  /* is the first word unit? */
    const wchar_t *ss;
    wchar_t *s;

#define FILL_SBUF(s) fill_splitbuf(e, !indq && !escapeall && (s));
#define FILL_SBUF_SPLITTABLE   FILL_SBUF(true)
#define FILL_SBUF_UNSPLITTABLE FILL_SBUF(false)

    for (; w != NULL; w = w->next, first = false) {
	switch (w->wu_type) {
	case WT_STRING:
	    ss = w->wu_string;
	    if (first && tilde != TT_NONE) {
		s = expand_tilde(&ss, w->next, tilde);
		if (s != NULL) {
		    wb_catfree(&e->valuebuf, escapefree(s, NULL));
		    FILL_SBUF_UNSPLITTABLE;
		}
	    }
	    while (*ss != L'\0') {
		switch (*ss) {
		case L'"':
		    if (!processquotes)
			goto escape;
		    indq = !indq;
		    wb_wccat(&e->valuebuf, L'"');
		    FILL_SBUF_UNSPLITTABLE;
		    break;
		case L'\'':
		    if (!processquotes || indq)
			goto escape;
		    wb_wccat(&e->valuebuf, L'\'');
		    add_sq(&ss, &e->valuebuf, true);
		    wb_wccat(&e->valuebuf, L'\'');
		    FILL_SBUF_UNSPLITTABLE;
		    break;
		case L'\\':
		    if (!processquotes) {
			if (!escapeall) {
			    wchar_t c = ss[1];
			    if (c == L'$' || c == L'`' || c == L'\\')
				ss++;
			}
			goto escape;
		    }

		    if (indq && wcschr(CHARS_ESCAPABLE, ss[1]) == NULL) {
			goto escape;
		    } else {
			wb_wccat(&e->valuebuf, L'\\');
			if (*++ss != L'\0')
			    wb_wccat(&e->valuebuf, *ss++);
			FILL_SBUF_UNSPLITTABLE;
			continue;
		    }
		case L':':
		    if (!indq && tilde == TT_MULTI) {
			/* perform tilde expansion after a colon */
			wb_wccat(&e->valuebuf, L':');
			ss++;
			s = expand_tilde(&ss, w->next, tilde);
			if (s != NULL) {
			    wb_catfree(&e->valuebuf, escapefree(s, NULL));
			    FILL_SBUF_UNSPLITTABLE;
			}
			continue;
		    }
		    /* falls thru! */
		default:
		    if (indq || escapeall)
escape:
			wb_wccat(&e->valuebuf, L'\\');
		    wb_wccat(&e->valuebuf, *ss);
		    FILL_SBUF(rec);
		    break;
		}
		ss++;
	    }
	    break;
	case WT_PARAM:
	    if (!expand_param(w->wu_param, indq || escapeall, e))
		ok = false;
	    break;
	case WT_CMDSUB:
	    s = exec_command_substitution(&w->wu_cmdsub);
	    goto cat_s;
	case WT_ARITH:
	    s = expand_single_and_unescape(w->wu_arith, TT_NONE, true, false);
	    if (s != NULL)
		s = evaluate_arithmetic(s);
cat_s:
	    if (s != NULL) {
		wb_catfree(&e->valuebuf, escapefree(s,
			    (indq || escapeall) ? NULL : CHARS_ESCAPED));
		FILL_SBUF_SPLITTABLE;
	    } else {
		ok = false;
	    }
	    break;
	}
    }

#undef FILL_SBUF_UNSPLITTABLE
#undef FILL_SBUF_SPLITTABLE
#undef FILL_SBUF

    return ok;
}

/* Appends to `e->splitbuf' as many `splittable' as needed to match the length
 * with `e->valuebuf'. */
void fill_splitbuf(struct expand_four_T *e, bool splittable)
{
    if (e->splitlist == NULL)
	return;
    sb_ccat_repeat(
	    &e->splitbuf, splittable, e->valuebuf.length - e->splitbuf.length);
}

/* Performs tilde expansion.
 * `ss' is a pointer to a pointer to the tilde character. The pointer is
 * increased so that it points to the character right after the expanded string.
 * The string pointed by the pointer pointed by `ss' should be contents of a
 * word unit of type WT_STRING. Iff there is a next word unit, `hasnextwordunit'
 * must be true.
 * If `**ss' is not L'~' or expansion fails, this function has no side effects
 * and returns NULL. If successful, `*ss' is incremented and the result is
 * returned as a newly malloced string. */
wchar_t *expand_tilde(const wchar_t **ss, bool hasnextwordunit, tildetype_T tt)
{
    const wchar_t *s = *ss;
    if (*s != L'~')
	return NULL;
    s++;

    const wchar_t *end = wcspbrk(s, tt == TT_SINGLE ? L"/" : L"/:");
    wchar_t *username;
    const wchar_t *home;
    size_t usernamelen;

    if (end != NULL) {
	usernamelen = end - s;
    } else {
	if (hasnextwordunit)
	    return NULL;
	usernamelen = wcslen(s);
    }
    username = xwcsndup(s, usernamelen);
    if (username[0] == L'\0') {
	/* empty user name: use $HOME */
	home = getvar(L VAR_HOME);
	goto finish;
    } else if (wcspbrk(username, L"\"'\\") != 0) {
	/* don't expand if the user name is quoted */
	free(username);
	return NULL;
    }
    if (!posixly_correct) {
	if (username[0] == L'+' && username[1] == L'\0') {
	    home = getvar(L VAR_PWD);
	    goto finish;
	}
	if (username[0] == L'-' && username[1] == L'\0') {
	    home = getvar(L VAR_OLDPWD);
	    goto finish;
	}
#if YASH_ENABLE_DIRSTACK
	if (username[0] == L'+' || username[0] == L'-') {
	    size_t index;
	    if (parse_dirstack_index(username, &index, &home, false)
		    && index != SIZE_MAX) {
		goto finish;
	    }
	}
#endif
    }
    home = get_home_directory(username, false);
finish:
    free(username);
    if (home == NULL)
	return NULL;
    *ss = s + usernamelen;
    return xwcsdup(home);
}

/* Performs parameter expansion.
 * The result is put in `e'.
 * Returns true iff successful. */
bool expand_param(const paramexp_T *restrict p, bool indq,
	struct expand_four_T *restrict e)
{
    /* parse indices first */
    ssize_t startindex, endindex;
    enum indextype_T indextype;
    if (p->pe_start == NULL) {
	startindex = 0, endindex = SSIZE_MAX, indextype = IDX_NONE;
    } else {
	wchar_t *start = expand_single_and_unescape(
		p->pe_start, TT_NONE, true, false);
	if (start == NULL)
	    return false;
	indextype = parse_indextype(start);
	if (indextype != IDX_NONE) {
	    startindex = 0, endindex = SSIZE_MAX;
	    free(start);
	    if (p->pe_end != NULL) {
		xerror(0, Ngt("the parameter index is invalid"));
		return false;
	    }
	} else if (!evaluate_index(start, &startindex)) {
	    return false;
	} else {
	    if (p->pe_end == NULL) {
		endindex = (startindex == -1) ? SSIZE_MAX : startindex;
	    } else {
		wchar_t *end = expand_single_and_unescape(
			p->pe_end, TT_NONE, true, false);
		if (end == NULL || !evaluate_index(end, &endindex))
		    return false;
	    }
	    if (startindex == 0)
		startindex = SSIZE_MAX;
	    else if (startindex >= 0)
		startindex--;
	}
    }
    /* Here, `startindex' and `endindex' are zero-based. `startindex' is
     * included in the range but `endindex' is not. A negative index will be
     * wrapped around the length. */

    /* get the value of parameter or nested expansion */
    struct get_variable_T v;
    bool unset;   /* parameter is not set? */
    if (p->pe_type & PT_NEST) {
	plist_T plist;
	pl_init(&plist);
	if (!expand_four_and_remove_quotes(
		    p->pe_nest, TT_NONE, true, true, &plist)) {
	    plfree(pl_toary(&plist), free);
	    return false;
	}
	v.type = (plist.length == 1) ? GV_SCALAR : GV_ARRAY;
	v.count = plist.length;
	v.values = pl_toary(&plist);
	v.freevalues = true;
	unset = false;
	for (size_t i = 0; v.values[i] != NULL; i++)
	    v.values[i] = unescapefree(v.values[i]);
    } else {
	v = get_variable(p->pe_name);
	if (v.type == GV_NOTFOUND) {
	    /* if the variable is not set, return empty string */
	    v.type = GV_SCALAR;
	    v.count = 1;
	    v.values = xmallocn(2, sizeof *v.values);
	    v.values[0] = xwcsdup(L"");
	    v.values[1] = NULL;
	    v.freevalues = true;
	    unset = true;
	} else {
	    unset = false;
	}
    }

    /* here, the contents of `v.values' are not escaped by backslashes. */

    /* modify the elements of `v.values' according to the indices */
    void **values;  /* the result */
    bool concat;    /* concatenate array elements? */
    switch (v.type) {
	case GV_SCALAR:
	    assert(v.values != NULL && v.count == 1);
	    save_get_variable_values(&v);
	    if (indextype != IDX_NUMBER) {
		trim_wstring(v.values[0], startindex, endindex);
	    } else {
		size_t len = wcslen(v.values[0]);
		free(v.values[0]);
		v.values[0] = malloc_wprintf(L"%zu", len);
	    }
	    values = v.values, concat = false;
	    break;
	case GV_ARRAY:
	    concat = false;
	    goto treat_array;
	case GV_ARRAY_CONCAT:
	    concat = true;
treat_array:
	    switch (indextype) {
	    case IDX_CONCAT:
		concat = true;
		/* falls thru! */
	    case IDX_NONE:
	    case IDX_ALL:
		if (startindex >= 0) {
#if SIZE_MAX >= SSIZE_MAX
		    if ((size_t) startindex > v.count)
#else
		    if (startindex > (ssize_t) v.count)
#endif
			startindex = v.count;
		} else {
		    startindex += v.count;
		    if (startindex < 0)
			startindex = 0;
		}
		if (endindex < 0)
		    endindex += v.count + 1;
		if (endindex < startindex)
		    endindex = startindex;
#if SSIZE_MAX > SIZE_MAX
		else if (endindex > (ssize_t) SIZE_MAX)
		    endindex = SIZE_MAX;
#endif
		assert(0 <= startindex && startindex <= endindex);
		values = v.freevalues
		    ? trim_array(v.values, startindex, endindex)
		    : plndup(v.values + startindex,
			    endindex - startindex, copyaswcs);
		break;
	    case IDX_NUMBER:
		if (v.freevalues)
		    plfree(v.values, free);
		values = xmallocn(2, sizeof *values);
		values[0] = malloc_wprintf(L"%zu", v.count);
		values[1] = NULL;
		concat = false;
		break;
	    default:
		assert(false);
	    }
	    break;
	default:
	    assert(false);
    }

    /* if `PT_COLON' is true, empty string is treated as unset */
    if (p->pe_type & PT_COLON)
	if (values[0] == NULL ||
		(((wchar_t *) values[0])[0] == L'\0' && values[1] == NULL))
	    unset = true;

    /* PT_PLUS, PT_MINUS, PT_ASSIGN, PT_ERROR */
    wchar_t *subst;
    switch (p->pe_type & PT_MASK) {
    case PT_PLUS:
	if (!unset)
	    goto subst;
	unset = false;
	break;
    case PT_MINUS:
	if (unset) {
subst:
	    plfree(values, free);
	    return expand_four(p->pe_subst, TT_SINGLE, true, indq, true, e);
	}
	break;
    case PT_ASSIGN:
	if (unset) {
	    plfree(values, free);
	    if (p->pe_type & PT_NEST) {
		xerror(0,
		    Ngt("a nested parameter expansion cannot be assigned"));
		return false;
	    } else if (!is_name(p->pe_name)) {
		xerror(0, Ngt("cannot assign to parameter `%ls' "
			    "in parameter expansion"),
			p->pe_name);
		return false;
	    } else if ((v.type == GV_ARRAY_CONCAT)
		    || (v.type == GV_ARRAY && startindex + 1 != endindex)) {
                xerror(0, Ngt("the specified index does not support assignment "
			    "in the parameter expansion of array `%ls'"),
			p->pe_name);
		return false;
	    }
	    subst = expand_single_and_unescape(
		    p->pe_subst, TT_SINGLE, true, false);
	    if (subst == NULL)
		return false;
	    if (v.type != GV_ARRAY) {
		assert(v.type == GV_NOTFOUND || v.type == GV_SCALAR);
		if (!set_variable(
			    p->pe_name, xwcsdup(subst), SCOPE_GLOBAL, false)) {
		    free(subst);
		    return false;
		}
	    } else {
		assert(0 <= startindex && (size_t) startindex <= v.count);
		if (!set_array_element(p->pe_name, startindex, xwcsdup(subst))){
		    free(subst);
		    return false;
		}
	    }
	    values = xmallocn(2, sizeof *values);
	    values[0] = subst;
	    values[1] = NULL;
	    unset = false;
	}
	break;
    case PT_ERROR:
	if (unset) {
	    plfree(values, free);
	    print_subst_as_error(p);
	    return false;
	}
	break;
    }

    if (unset && !shopt_unset) {
	plfree(values, free);
	xerror(0, Ngt("parameter `%ls' is not set"), p->pe_name);
	return false;
    }

    /* PT_MATCH, PT_SUBST */
    wchar_t *match;
    switch (p->pe_type & PT_MASK) {
    case PT_MATCH:
	match = expand_single(p->pe_match, TT_SINGLE, true, false);
	if (match == NULL) {
	    plfree(values, free);
	    return false;
	}
	match_each(values, match, p->pe_type);
	free(match);
	break;
    case PT_SUBST:
	match = expand_single(p->pe_match, TT_SINGLE, true, false);
	subst = expand_single_and_unescape(p->pe_subst, TT_SINGLE, true, false);
	if (match == NULL || subst == NULL) {
	    free(match);
	    free(subst);
	    plfree(values, free);
	    return false;
	}
	subst_each(values, match, subst, p->pe_type);
	free(match);
	free(subst);
	break;
    }

    /* concatenate the elements of `values' */
    if (concat && indq)
	values = concatenate_values_into_array(values, false);

    /* PT_NUMBER */
    if (p->pe_type & PT_NUMBER)
	subst_length_each(values);

    /* backslash escape */
    for (size_t i = 0; values[i] != NULL; i++)
	values[i] = escapefree(values[i], indq ? NULL : CHARS_ESCAPED);

    /* add the elements of `values' to `e->valuelist' */
    if (values[0] == NULL) {
	if (indq)
	    e->zeroword = true;
    } else {
	/* add the first element */
	wb_catfree(&e->valuebuf, values[0]);
	fill_splitbuf(e, !indq);
	if (values[1] != NULL) {
	    pl_add(e->valuelist, wb_towcs(&e->valuebuf));
	    if (e->splitlist != NULL)
		pl_add(e->splitlist, sb_tostr(&e->splitbuf));

	    /* add the remaining but last */
	    size_t i;
	    for (i = 1; values[i + 1] != NULL; i++) {
		pl_add(e->valuelist, values[i]);
		if (e->splitlist != NULL) {
		    size_t len = wcslen(values[i]);
		    pl_add(e->splitlist, memset(xmalloc(len), !indq, len));
		}
	    }

	    /* add the last element */
	    wb_initwith(&e->valuebuf, values[i]);
	    if (e->splitlist != NULL) {
		sb_init(&e->splitbuf);
		fill_splitbuf(e, !indq);
	    }
	}
    }
    free(values);

    return true;
}

/* Returns IDX_ALL, IDX_CONCAT, IDX_NUMBER if `indexstr' is L"@", L"*",
 * L"#" respectively. Otherwise returns IDX_NONE. */
enum indextype_T parse_indextype(const wchar_t *indexstr)
{
    if (indexstr[0] != L'\0' && indexstr[1] == L'\0') {
	switch (indexstr[0]) {
	    case L'@':  return IDX_ALL;
	    case L'*':  return IDX_CONCAT;
	    case L'#':  return IDX_NUMBER;
	}
    }
    return IDX_NONE;
}

/* Trims some leading and trailing characters of the wide string.
 * Characters in the range [`startindex', `endindex') remain.
 * Returns the string `s'. */
wchar_t *trim_wstring(wchar_t *s, ssize_t startindex, ssize_t endindex)
{
    if (startindex == 0 && endindex == SSIZE_MAX)
	return s;
    if (startindex < 0 || endindex < 0) {
	ssize_t len = wcslen(s);
	if (startindex < 0) {
	    startindex += len;
	    if (startindex < 0)
		startindex = 0;
	}
	if (endindex < 0) {
	    endindex += len + 1;
	    if (endindex <= startindex)
		goto return_empty;
	}
    }

    assert(startindex >= 0 && endindex >= 0);
    if (startindex >= endindex)
	goto return_empty;
    for (ssize_t i = 0; i < startindex; i++)
	if (s[i] == L'\0')
	    goto return_empty;
    for (ssize_t i = 0; i < endindex - startindex; i++)
	if ((s[i] = s[startindex + i]) == L'\0')
	    return s;
    s[endindex - startindex] = L'\0';
    return s;

return_empty:
    s[0] = L'\0';
    return s;
}

/* Trims some leading and trailing elements of the NULL-terminated array of
 * pointers.
 * Elements in the range [`startindex', `endindex') remain. `startindex' must
 * not be negative and `endindex' must not be less than `startindex'.
 * Removed elements are freed.
 * Returns the array `a'. */
/* `startindex' and/or `endindex' may be >= the length of the array. */
void **trim_array(void **a, ssize_t startindex, ssize_t endindex)
{
    assert(0 <= startindex && startindex <= endindex);
    if (startindex == 0 && endindex == SSIZE_MAX)
	return a;

    ssize_t len = endindex - startindex;
    for (ssize_t i = 0; i < startindex; i++) {
	if (a[i] == NULL) {
	    a[0] = NULL;
	    return a;
	}
	free(a[i]);
    }
    for (ssize_t i = 0; i < len; i++)
	if ((a[i] = a[startindex + i]) == NULL)
	    return a;
    for (ssize_t i = endindex; a[i] != NULL; i++)
	free(a[i]);
    a[len] = NULL;
    return a;
}

/* Expands `p->pe_subst' and prints it as an error message. */
void print_subst_as_error(const paramexp_T *p)
{
    if (p->pe_subst != NULL) {
	wchar_t *subst = expand_single_and_unescape(
		p->pe_subst, TT_SINGLE, true, false);
	if (subst != NULL) {
	    if (p->pe_type & PT_NEST)
		xerror(0, "%ls", subst);
	    else
		xerror(0, "%ls: %ls", p->pe_name, subst);
	    free(subst);
	}
    } else {
	/* use the default error message */
	if (p->pe_type & PT_NEST)
	    xerror(0, Ngt("the parameter value is empty"));
	else
	    xerror(0, (p->pe_type & PT_COLON)
		    ? Ngt("parameter `%ls' is not set or has an empty value")
		    : Ngt("parameter `%ls' is not set"),
		    p->pe_name);
    }
}

/* Matches each string in array `slist' to pattern `pattern' and removes the
 * matching part of the string.
 * `slist' is a NULL-terminated array of pointers to `free'able wide strings.
 * `type' must contain at least one of PT_MATCHHEAD, PT_MATCHTAIL and
 * PT_MATCHLONGEST. If both of PT_MATCHHEAD and PT_MATCHTAIL are specified,
 * PT_MATCHLONGEST must be specified too.
 * Elements of `slist' may be modified and/or `realloc'ed in this function. */
void match_each(void **restrict slist, const wchar_t *restrict pattern,
	paramexptype_T type)
{
    xfnmflags_T flags = 0;
    assert(type & (PT_MATCHHEAD | PT_MATCHTAIL | PT_MATCHLONGEST));
    if (type & PT_MATCHHEAD)
	flags |= XFNM_HEADONLY;
    if (type & PT_MATCHTAIL)
	flags |= XFNM_TAILONLY;
    if (!(type & PT_MATCHLONGEST))
	flags |= XFNM_SHORTEST;

    xfnmatch_T *xfnm = xfnm_compile(pattern, flags);
    if (xfnm == NULL)
	return;

    for (size_t i = 0; slist[i] != NULL; i++) {
	wchar_t *s = slist[i];
	xfnmresult_T result = xfnm_wmatch(xfnm, s);
	if (result.start != (size_t) -1) {
	    xwcsbuf_T buf;
	    wb_initwith(&buf, s);
	    wb_remove(&buf, result.start, result.end - result.start);
	    slist[i] = wb_towcs(&buf);
	}
    }
    xfnm_free(xfnm);
}

/* Matches each string in array `slist' to pattern `pattern' and substitutes
 * the matching portions with `subst'.
 * `slist' is a NULL-terminated array of pointers to `free'able wide strings.
 * `type' may contain PT_MATCHHEAD, PT_MATCHTAIL and PT_SUBSTALL.
 * PT_MATCHLONGEST is always assumed to be specified.
 * Elements of `slist' may be modified and/or `realloc'ed in this function. */
void subst_each(void **restrict slist, const wchar_t *pattern,
	const wchar_t *subst, paramexptype_T type)
{
    xfnmflags_T flags = 0;
    if (type & PT_MATCHHEAD)
	flags |= XFNM_HEADONLY;
    if (type & PT_MATCHTAIL)
	flags |= XFNM_TAILONLY;

    xfnmatch_T *xfnm = xfnm_compile(pattern, flags);
    if (xfnm == NULL)
	return;

    for (size_t i = 0; slist[i] != NULL; i++) {
	wchar_t *s = slist[i];
	slist[i] = xfnm_subst(xfnm, s, subst, type & PT_SUBSTALL);
	free(s);
    }
    xfnm_free(xfnm);
}

/* Concatenates the wide strings in the specified array.
 * Array `*values' must be a NULL-terminated array of pointers to wide strings.
 * The strings are concatenated into one, each separated by the first $IFS
 * character. The separators are backslashed if `escape' is true.
 * The array and its element strings are all freed in this function.
 * The return value is a pointer to the newly malloced wide string that is the
 * result of concatenation. */
wchar_t *concatenate_values(void **values, bool escape)
{
    wchar_t *first = values[0];
    if (first != NULL && values[1] == NULL) {
	// no actual concatenation needed
	free(values);
	return first;
    }

    const wchar_t *ifs = getvar(L VAR_IFS);
    wchar_t padding[] = { L'\\', ifs != NULL ? ifs[0] : L' ', L'\0' };
    wchar_t *result = joinwcsarray(values, escape ? padding : &padding[1]);
    plfree(values, free);
    return result;
}

/* Like `concatenate_values', but returns a pointer to a newly malloced
 * NULL-terminated array containing the concatenated string. */
void **concatenate_values_into_array(void **values, bool escape)
{
    if (values[0] != NULL && values[1] == NULL)
	return values;

    void **results = xmallocn(2, sizeof *values);
    results[0] = concatenate_values(values, escape);
    results[1] = NULL;
    return results;
}

/* Substitutes each string in the specified array with a string that contains
 * the number of characters in the original string.
 * `slist' is a NULL-terminated array of pointers to `free'able wide strings.
 * The strings are `realloc'ed and modified in this function. */
void subst_length_each(void **slist)
{
    for (size_t i = 0; slist[i] != NULL; i++) {
	size_t len = wcslen(slist[i]);
	free(slist[i]);
	slist[i] = malloc_wprintf(L"%zu", len);
    }
}


/********** Brace Expansions **********/

/* Performs brace expansion in each element of the specified array.
 * `values' is an array of pointers to `free'able wide strings to be expanded.
 * `splits' is an array of pointers to `free'able splittability strings.
 * `values' and 'splits' must contain the same number of elements.
 * Both the arrays must be NULL-terminated and their elements are freed in this
 * function. The arrays themselves are not freed.
 * Newly malloced results are added to `valuelist' and `splitlist'. */
void expand_brace_each(void **restrict values, void **restrict splits,
	plist_T *restrict valuelist, plist_T *restrict splitlist)
{
    while (*values != NULL) {
	expand_brace(*values, *splits, valuelist, splitlist);
	values++, splits++;
    }
}

/* Performs brace expansion in the specified single word.
 * `split' is the splittability string corresponding to `word'.
 * `word' and `split' are freed in this function.
 * `Free'able results are added to `valuelist' and `splitlist'. */
void expand_brace(wchar_t *restrict const word, char *restrict const split,
	plist_T *restrict valuelist, plist_T *restrict splitlist)
{
    wchar_t *c = word;

start:
    c = escaped_wcspbrk(c, L"{");
    if (c == NULL || *++c == L'\0') {
	/* don't expand if there is no L'{' or L'{' is at the end of string */
	pl_add(valuelist, word);
	pl_add(splitlist, split);
	return;
    } else if (try_expand_brace_sequence(word, split, c, valuelist, splitlist)){
	return;
    }

    plist_T splitpoints;
    unsigned nest;

    /* collect pointers to characters where the word is split */
    /* The pointers point to the character just after L'{', L',' or L'}'. */
    pl_init(&splitpoints);
    pl_add(&splitpoints, c);
    nest = 0;
    while ((c = escaped_wcspbrk(c, L"{,}")) != NULL) {
	switch (*c++) {
	    case L'{':
		nest++;
		break;
	    case L',':
		if (nest == 0)
		    pl_add(&splitpoints, c);
		break;
	    case L'}':
		if (nest > 0) {
		    nest--;
		    break;
		} else if (splitpoints.length == 1) {
		    goto restart;
		} else {
		    pl_add(&splitpoints, c);
		    goto done;
		}
	}
    }
restart:
    /* if there is no L',' or L'}' corresponding to L'{',
     * find the next L'{' and try again */
    c = splitpoints.contents[0];
    pl_destroy(&splitpoints);
    goto start;

done:;
#define idx(p)  ((wchar_t *) (p) - word)
#define wtos(p) (split + idx(p))
    size_t lastelemindex = splitpoints.length - 1;
    size_t headlen = idx(splitpoints.contents[0]) - 1;
    size_t taillen = wcslen(splitpoints.contents[lastelemindex]);
    for (size_t i = 0; i < lastelemindex; i++) {
	xwcsbuf_T buf;
	xstrbuf_T sbuf;
	wb_init(&buf);
	sb_init(&sbuf);

	wb_ncat_force(&buf, word, headlen);
	sb_ncat_force(&sbuf, split, headlen);

	size_t len = (wchar_t *) splitpoints.contents[i + 1] -
	             (wchar_t *) splitpoints.contents[i    ] - 1;
	wb_ncat_force(&buf, splitpoints.contents[i], len);
	sb_ncat_force(&sbuf, wtos(splitpoints.contents[i]), len);

	wb_ncat_force(&buf, splitpoints.contents[lastelemindex], taillen);
	sb_ncat_force(&sbuf, wtos(splitpoints.contents[lastelemindex]), taillen);
	assert(buf.length == sbuf.length);

	/* expand the remaining portion recursively */
	expand_brace(wb_towcs(&buf), sb_tostr(&sbuf), valuelist, splitlist);
    }
    pl_destroy(&splitpoints);
    free(word);
    free(split);
#undef idx
#undef wtos
}

/* Tries numeric brace expansion like "{01..05}".
 * If unsuccessful, this function returns false without any side effects.
 * If successful, `word' and `split' are freed and the full expansion results
 * are added to `valuelist' and `splitlist'.
 * `startc' is a pointer to the character right after L'{' in `word'.
 */
bool try_expand_brace_sequence(
	wchar_t *word, char *restrict split, wchar_t *startc,
	plist_T *restrict valuelist, plist_T *restrict splitlist)
{
    long start, end, delta, value;
    wchar_t *dotp, *dotbracep, *bracep, *c;
    int startlen, endlen, len, wordlen;
    bool sign = false;

    assert(startc[-1] == L'{');
    c = startc;

    /* parse the starting point */
    dotp = wcschr(c, L'.');
    if (dotp == NULL || c == dotp || dotp[1] != L'.')
	return false;
    startlen = has_leading_zero(c, &sign) ? (dotp - c) : 0;
    errno = 0;
    start = wcstol(c, &c, 10);
    if (errno != 0 || c != dotp)
	return false;

    c = dotp + 2;

    /* parse the ending point */
    dotbracep = wcspbrk(c, L".}");
    if (dotbracep == NULL || c == dotbracep ||
	    (dotbracep[0] == L'.' && dotbracep[1] != L'.'))
	return false;
    endlen = has_leading_zero(c, &sign) ? (dotbracep - c) : 0;
    errno = 0;
    end = wcstol(c, &c, 10);
    if (errno != 0 || c != dotbracep)
	return false;

    /* parse the delta */
    if (dotbracep[0] == L'.') {
	assert(dotbracep[1] == L'.');
	c = dotbracep + 2;
	bracep = wcschr(c, L'}');
	if (bracep == NULL || c == bracep)
	    return false;
	errno = 0;
	delta = wcstol(c, &c, 10);
	if (delta == 0 || errno != 0 || c != bracep)
	    return false;
    } else {
	assert(dotbracep[0] == L'}');
	bracep = dotbracep;
	if (start <= end)
	    delta = 1;
	else
	    delta = -1;
    }

    /* expand the sequence */
    value = start;
    len = (startlen > endlen) ? startlen : endlen;
    wordlen = wcslen(word);
    do {
	xwcsbuf_T buf;
	xstrbuf_T sbuf;
	wb_init(&buf);
	sb_init(&sbuf);

	wb_ncat_force(&buf, word, startc - 1 - word);
	sb_ncat_force(&sbuf, split, startc - 1 - word);

	int plen = wb_wprintf(&buf, sign ? L"%0+*ld" : L"%0*ld", len, value);
	if (plen >= 0)
	    sb_ccat_repeat(&sbuf, 0, plen);

	wb_ncat_force(&buf,
		bracep + 1,
		wordlen - (bracep + 1 - word));
	sb_ncat_force(&sbuf,
		split   + (bracep + 1 - word),
		wordlen - (bracep + 1 - word));
	assert(buf.length == sbuf.length);

	/* expand the remaining portion recursively */
	expand_brace(wb_towcs(&buf), sb_tostr(&sbuf), valuelist, splitlist);

	if (delta >= 0) {
	    if (LONG_MAX - delta < value)
		break;
	} else {
	    if (LONG_MIN - delta > value)
		break;
	}
	value += delta;
    } while (delta >= 0 ? value <= end : value >= end);
    free(word);
    free(split);
    return true;
}

/* Checks if the specified numeral starts with a L'0'.
 * Leading spaces are ignored.
 * If the numeral has a plus sign L'+', true is assigned to `*sign'.
 * If not, false is assigned. */
bool has_leading_zero(const wchar_t *restrict s, bool *restrict sign)
{
    while (iswspace(*s))
	s++;
    if (*s == L'+') {
	*sign = true;
	s++;
    } else if (*s == L'-') {
	s++;
    }
    return *s == L'0';
}


/********** Field Splitting **********/

/* Performs field splitting.
 * `valuelist' is a NULL-terminated array of pointers to wide strings to split.
 * `splitlist' is an array of pointers to corresponding splittability strings.
 * `valuelist' and `splitlist' are `plfree'ed in this function.
 * The results are added to `dest'. */
void fieldsplit_all(void **restrict valuelist, void **restrict splitlist,
	plist_T *restrict dest)
{
    void **restrict s;
    void **restrict t;
    const wchar_t *ifs;

    ifs = getvar(L VAR_IFS);
    if (ifs == NULL)
	ifs = DEFAULT_IFS;

    for (s = valuelist, t = splitlist; *s != NULL; s++, t++)
	fieldsplit(*s, *t, ifs, dest);
    free(valuelist);
    free(splitlist);
}

/* Performs field splitting.
 * `s' is the word to split and freed in this function.
 * `split' is the splittability string corresponding to `s' and also freed.
 * The results are added to `dest' as newly-malloced wide strings.
 * `ifs' must not be NULL. */
void fieldsplit(wchar_t *restrict s, char *restrict split,
	const wchar_t *restrict ifs, plist_T *restrict dest)
{
    plist_T fields;

    pl_init(&fields);
    extract_fields(s, split, true, ifs, &fields);
    assert(fields.length % 2 == 0);

    for (size_t i = 0; i < fields.length; i += 2) {
	const wchar_t *start = fields.contents[i], *end = fields.contents[i+1];
	pl_add(dest, xwcsndup(start, end - start));
    }

    pl_destroy(&fields);
    free(s);
    free(split);
}

/* Extracts fields from a string.
 * `s' is the word to split.
 * `split' is the splittability string corresponding to `s'. It must be at least
 * as long as `wcslen(s)'.
 * If `escaped' is true, backslashes in `s' are treated as escapes. But
 * backslashes do not prevent splitting.
 * `ifs' must not be NULL.
 *
 * The results are appended to `dest'. If n fields are found, 2n pointers are
 * appended to `dest'. The first pointer points to the first character of the
 * first field in `s'. The second to the character past the last character of
 * the first field. The third to the first character of the second field. And so
 * on.
 *
 * The word is split at characters that are contained in `ifs' and whose
 * corresponding character in the splittability string is non-zero. Refer to
 * POSIX for how whitespaces are treated in field splitting.
 *
 * If an IFS non-whitespace delimits an empty field, the field is assumed just
 * before the non-whitespace delimiter. The empty last field is removed if
 * `shopt_emptylastfield' is false.
 *
 * The return value is a pointer to the end of the input string (but before
 * trailing IFS whitespaces). */
/* Split examples (assuming `ifs' = L" -" and `shopt_emptylastfield' is true)
 *   ""                  ->   ""
 *   "  "                ->   ""
 *   " abc 123 "         ->   "abc" "123"
 *   "  abc  123  "      ->   "abc" "123"
 *   "-abc-123-"         ->   "" "abc" "123" ""
 *   " - abc - 123 - "   ->   "" "abc" "123" ""
 *   "abc--123"          ->   "abc" "" "123"
 *   "abc - - 123"       ->   "abc" "" "123"
 */
wchar_t *extract_fields(const wchar_t *restrict s, const char *restrict split,
	bool escaped, const wchar_t *restrict ifs, plist_T *restrict dest)
{
    size_t index = 0;
    size_t ifswhitestartindex;
    size_t oldlen = dest->length;

    /* true when the currently skipping IFS whitespaces immediately follow a
     * previously split field. */
    bool afterfield = false;

    for (;;) {
	ifswhitestartindex = index;
	index += skip_ifs_whitespaces(&s[index], &split[index], escaped, ifs);

	/* extract next field, if any */
	size_t fieldstartindex = index;
	index += skip_field(&s[index], &split[index], escaped, ifs);
	if (index != fieldstartindex) {
	    pl_add(pl_add(dest, &s[fieldstartindex]), &s[index]);
	    afterfield = true;
	    continue;
	}

	/* Now the current char is either null or a IFS non-whitespace. */

	if (!afterfield)
	    add_empty_field(dest, &s[index]);

	/* skip (only one) IFS non-whitespace */
	size_t ifsstartindex = index;
	index += skip_ifs(&s[index], &split[index], escaped, ifs);
	if (index != ifsstartindex) {
	    afterfield = false;
	    continue;
	}

	/* Now the current char is null. We're done. */
	break;
    }

    /* remove the empty last field */
    size_t newlen = dest->length;
    if (!shopt_emptylastfield && newlen - oldlen >= 2 * 2 &&
	    dest->contents[newlen - 2] == dest->contents[newlen - 1])
	pl_remove(dest, newlen - 2, 2);

    assert(dest->length - oldlen >= 2);
    return (wchar_t *) &s[ifswhitestartindex];
}

/* If `*s' is a (possibly escaped if `escaped') IFS character, returns the
 * number of characters to skip it. Otherwise returns zero. */
size_t skip_ifs(const wchar_t *s, const char *split,
	bool escaped, const wchar_t *ifs)
{
    size_t i = 0;
    if (escaped && s[i] == L'\\')
	i++;
    if (s[i] == L'\0')
	return 0;
    if (split[i] && wcschr(ifs, s[i]) != NULL)
	return i + 1;
    else
	return 0;
}

/* Returns the length of IFS whitespace sequence starting at `*s'. */
size_t skip_ifs_whitespaces(const wchar_t *s, const char *split,
	bool escaped, const wchar_t *ifs)
{
    size_t total = 0;
    for (;;) {
	size_t current = skip_ifs(&s[total], &split[total], escaped, ifs);
	if (current == 0 || !iswspace(s[total + current - 1]))
	    return total;
	total += current;
    }
}

/* Returns the length of a field starting at `*s'. */
size_t skip_field(const wchar_t *s, const char *split,
	bool escaped, const wchar_t *ifs)
{
    size_t index = 0;
    for (;;) {
	size_t saveindex = index;
	if (escaped && s[index] == L'\\')
	    index++;
	if (s[index] == L'\0')
	    return saveindex;
	if (split[index] && wcschr(ifs, s[index]) != NULL)
	    return saveindex;
	index++;
    }
}

void add_empty_field(plist_T *dest, const wchar_t *p)
{
    pl_add(dest, p);
    pl_add(dest, p);
}


/********** Escaping **********/

/* Unquotes the specified single-quoted string and adds it to the specified
 * buffer.
 * `ss' is a pointer to a pointer to the opening quote in the string.
 * `*ss' is incremented so that it points to the closing quote.
 * If `escape' is true, all the characters added are backslashed. */
void add_sq(const wchar_t *restrict *ss, xwcsbuf_T *restrict buf, bool escape)
{
    assert(**ss == L'\'');
    for (;;) {
	(*ss)++;
	switch (**ss) {
	    case L'\0':
		assert(false);
	    case L'\'':
		return;
	    default:
		if (escape)
		    wb_wccat(buf, L'\\');
		wb_wccat(buf, **ss);
		break;
	}
    }
}

/* Backslashes characters in `s' that are contained in `t'.
 * Returns a newly-malloced wide string.
 * `t' may be NULL, in which case all the characters are backslashed. */
wchar_t *escape(const wchar_t *restrict s, const wchar_t *restrict t)
{
    xwcsbuf_T buf;
    wb_init(&buf);
    for (size_t i = 0; s[i] != L'\0'; i++) {
	if (t == NULL || wcschr(t, s[i]) != NULL)
	    wb_wccat(&buf, L'\\');
	wb_wccat(&buf, s[i]);
    }
    return wb_towcs(&buf);
}

/* Same as `escape', except that the first argument is freed. */
wchar_t *escapefree(wchar_t *restrict s, const wchar_t *restrict t)
{
    if (t != NULL && wcspbrk(s, t) == NULL) {
	return s;
    } else {
	wchar_t *result = escape(s, t);
	free(s);
	return result;
    }
}

/* Removes backslash escapes. The result is a newly malloced string.
 * If there is an unescaped backslash before the null character, the backslash
 * is ignored. */
wchar_t *unescape(const wchar_t *s)
{
    xwcsbuf_T buf;
    wb_init(&buf);
    for (size_t i = 0; s[i] != L'\0'; i++) {
	if (s[i] == L'\\') {
	    if (s[i + 1] == L'\0')
		break;
	    else
		i++;
	}
	wb_wccat(&buf, s[i]);
    }
    return wb_towcs(&buf);
}

/* Same as `unescape', except that the first argument is freed. */
wchar_t *unescapefree(wchar_t *s)
{
    if (wcschr(s, L'\\') == NULL) {
	return s;
    } else {
	wchar_t *result = unescape(s);
	free(s);
	return result;
    }
}

/* Quotes the specified string using backslashes and single-quotes. The result
 * is suitable for re-parsing as a shell command word that would expand to the
 * original string. The result is a newly malloced string. */
wchar_t *quote_as_word(const wchar_t *s)
{
    xwcsbuf_T buf;
    wb_init(&buf);
    wb_quote_as_word(&buf, s);
    return wb_towcs(&buf);
}

/* Quotes string `s' using backslashes and single-quotes. The result
 * is suitable for re-parsing as a shell command word that would expand to the
 * original string. The result is appended to the given buffer, which must
 * have been initialized before calling this function. */
xwcsbuf_T *wb_quote_as_word(xwcsbuf_T *restrict buf, const wchar_t *restrict s)
{
    if (*s == L'\0') {
	wb_wccat(buf, L'\'');
	wb_wccat(buf, L'\'');
	return buf;
    }

    while (*s != L'\0') {
	if (*s == L'\'') {
	    wb_wccat(buf, L'\\');
	    wb_wccat(buf, L'\'');
	    s++;
	    continue;
	}

	const wchar_t *end = s;
	while (*end == L'.' || *end == L'-' || *end == L'_' || *end == L'/' ||
		iswalnum(*end))
	    end++;
	if (*end == L'\0' || *end == L'\'') {
	    /* No characters have to be quoted until `*end'. */
	    wb_ncat_force(buf, s, end - s);
	    s = end;
	    continue;
	}

	/* Quote characters until the next single-quote or end-of-string. */
	wb_ensuremax(buf, buf->length + (end - s) + 2);
	wb_wccat(buf, L'\'');
	while (*s != L'\0' && *s != L'\'') {
	    wb_wccat(buf, *s);
	    s++;
	}
	wb_wccat(buf, L'\'');
    }

    return buf;
}

/* Removes quotes (', ", \). The result is a newly malloced string. */
wchar_t *unquote(const wchar_t *s)
{
    bool indq = false;
    xwcsbuf_T buf;
    wb_init(&buf);
    for (;;) {
	switch (*s) {
	case L'\0':
	    return wb_towcs(&buf);
	case L'\'':
	    if (indq)
		goto default_case;
	    add_sq(&s, &buf, false);
	    break;
	case L'"':
	    indq = !indq;
	    break;
	case L'\\':
	    if (s[1] != L'\0' && (!indq || wcschr(CHARS_ESCAPABLE, s[1]))) {
		wb_wccat(&buf, s[1]);
		s += 2;
		continue;
	    }
	    /* falls thru! */
	default:  default_case:
	    wb_wccat(&buf, *s);
	    break;
	}
	s++;
    }
}

/* Like `wcspbrk', but ignores backslashed characters in `s'. */
wchar_t *escaped_wcspbrk(const wchar_t *s, const wchar_t *accept)
{
    for (; *s != L'\0'; s++) {
	if (*s == L'\\') {
	    s++;
	    if (*s == L'\0')
		break;
	    continue;
	}
	if (wcschr(accept, *s) != NULL)
	    return (wchar_t *) s;
    }
    return NULL;
}

/* Removes characters in `reject' from `s'.
 * Backslash escapes in `s' are recognized. Escapes and escaped characters are
 * kept in the result.
 * The result is a newly malloced string. */
wchar_t *escaped_remove(const wchar_t *s, const wchar_t *reject)
{
    xwcsbuf_T result;
    wb_init(&result);
    for (;;) {
	const wchar_t *rejectchar = escaped_wcspbrk(s, reject);
	if (rejectchar == NULL)
	    break;
	wb_ncat_force(&result, s, rejectchar - s);
	s = rejectchar + 1;
    }
    wb_cat(&result, s);
    return wb_towcs(&result);
}

/* Like `escaped_remove', but frees `s' before returning the result. */
wchar_t *escaped_remove_free(wchar_t *s, const wchar_t *reject)
{
    wchar_t *result = escaped_remove(s, reject);
    free(s);
    return result;
}


/********** File Name Expansion (Glob) **********/

/* Makes a option value from the current shell settings. */
enum wglobflags_T get_wglobflags(void)
{
    enum wglobflags_T flags = 0;
    if (!shopt_caseglob)    flags |= WGLB_CASEFOLD;
    if (shopt_dotglob)      flags |= WGLB_PERIOD;
    if (shopt_markdirs)     flags |= WGLB_MARK;
    if (shopt_extendedglob) flags |= WGLB_RECDIR;
    return flags;
}

/* Performs file name expansion to the specified patterns.
 * `patterns' is a NULL-terminated array of pointers to `free'able wide strings
 * cast to (void *). `patterns' is `plfree'd in this function.
 * The results are added to `list' as newly-malloced wide strings. */
void glob_all(void **restrict patterns, plist_T *restrict list)
{
    enum wglobflags_T flags = get_wglobflags();
    bool unblock = false;

    for (size_t i = 0; patterns[i] != NULL; i++) {
	wchar_t *pat = patterns[i];
	if (is_pathname_matching_pattern(pat)) {
	    if (!unblock) {
		set_interruptible_by_sigint(true);
		unblock = true;
	    }

	    size_t oldlen = list->length;
	    wglob(pat, flags, list);
	    if (!shopt_nullglob && oldlen == list->length)
		goto addpattern;
	    free(pat);
	} else {
	    /* If the pattern doesn't contain characters like L'*' and L'?',
	     * we don't need to glob. */
addpattern:
	    pl_add(list, unescapefree(pat));
	}
    }
    if (unblock)
	set_interruptible_by_sigint(false);
    free(patterns);
}


/********** Auxiliary functions **********/

/* Performs parameter expansion, command substitution of the form "$(...)", and
 * arithmetic expansion in the specified string.
 * If `name' is non-NULL, it is printed in error messages on error.
 * If `esc' is true, backslashes preceding $, `, \ are removed. Otherwise,
 * no quotations are removed.
 * Returns a newly malloced string if successful. Otherwise NULL is returned.
 * This function uses the parser, so the parser state must have been saved if
 * this function is called during another parse. */
wchar_t *parse_and_expand_string(const wchar_t *s, const char *name, bool esc)
{
    struct input_wcs_info_T winfo = {
	.src = s,
    };
    parseparam_T info = {
	.print_errmsg = true,
	.enable_verbose = false,
	.enable_alias = true,
	.filename = name,
	.lineno = 1,
	.input = input_wcs,
	.inputinfo = &winfo,
	.interactive = false,
    };
    wordunit_T *word;
    wchar_t *result;

    if (!parse_string(&info, &word))
	return NULL;
    result = expand_single_and_unescape(word, TT_NONE, false, !esc);
    wordfree(word);
    return result;
}

/* This function is called when an expansion error occurred.
 * The shell exits if it is non-interactive. */
void maybe_exit_on_error(void)
{
    if (shell_initialized && !is_interactive_now)
	exit_shell_with_status(Exit_EXPERROR);
}


/* vim: set ts=8 sts=4 sw=4 noet tw=80: */
