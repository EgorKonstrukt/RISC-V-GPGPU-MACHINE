/* nxedit_syntax.h: what each character of a line of C is, for its colour; the lexer of
 * linux/ide/syntax.h, copied, with the one thing a line needs from the lines before it
 * (whether it begins inside a comment) worked out apart, fast, for the lines not drawn. */
#ifndef NXEDIT_SYNTAX_H
#define NXEDIT_SYNTAX_H
#include <ctype.h>
#include <string.h>

enum { ES_PLAIN, ES_KEYWORD, ES_TYPE, ES_DIRECTIVE, ES_STRING, ES_NUMBER, ES_COMMENT, ES_KINDS };

static const char *const es_keywords[] = { "if", "else", "for", "while", "do", "return", "break", "continue", "switch", "case",
	"default", "goto", "sizeof", "typedef", "struct", "union", "enum", "static", "extern", "const", "volatile", "inline",
	"register", "restrict", "class", "public", "private", "protected", "namespace", "template", "new", "delete", "this",
	"true", "false", NULL };
static const char *const es_types[] = { "int", "char", "float", "double", "void", "long", "short", "unsigned", "signed",
	"uint8_t", "uint16_t", "uint32_t", "uint64_t", "int8_t", "int16_t", "int32_t", "int64_t", "size_t", "ssize_t", "uintptr_t",
	"bool", "FILE", "NULL", "auto", NULL };

enum { ES_ALPHA = 1, ES_DIGIT = 2, ES_UNDER = 4 };
static const unsigned char es_class[256] = {
	['A' ... 'Z'] = ES_ALPHA, ['a' ... 'z'] = ES_ALPHA, ['0' ... '9'] = ES_DIGIT, ['_'] = ES_UNDER,
};

static struct { const char *word; unsigned char length, kind; } es_words[80];
static unsigned char es_from[129];

static void
es_index(void)
{
	const char *const *list;
	int c, n = 0, pass;

	for (c = 0; c < 128; c++) {
		es_from[c] = (unsigned char)n;
		for (pass = 0; pass < 2; pass++)
			for (list = pass ? es_types : es_keywords; *list; list++)
				if ((*list)[0] == c) {
					es_words[n].word = *list;
					es_words[n].length = (unsigned char)strlen(*list);
					es_words[n++].kind = pass ? ES_TYPE : ES_KEYWORD;
				}
	}
	es_from[128] = (unsigned char)n;
}

static int
es_word(const char *word, int length)
{
	int c = (unsigned char)word[0], i;

	if (!es_from[128])
		es_index();
	if (c >= 128)
		return ES_PLAIN;
	for (i = es_from[c]; i < es_from[c + 1]; i++)
		if (es_words[i].length == length && !memcmp(es_words[i].word, word, length))
			return es_words[i].kind;
	return ES_PLAIN;
}

/* Fills `kinds` for a line of n bytes ending in a 0; returns whether the next line begins in a comment. */
static int
es_line(const char *text, int n, int in_comment, unsigned char *kinds)
{
	int i = 0, j, directive = 0;

	while (i < n) {
		int ch = (unsigned char)text[i], is = es_class[ch];

		if (in_comment) {
			for (; i < n && !(text[i] == '*' && text[i + 1] == '/'); i++)
				kinds[i] = ES_COMMENT;
			in_comment = i >= n;
			if (!in_comment) {
				kinds[i] = kinds[i + 1] = ES_COMMENT;
				i += 2;
			}
		} else if (is & (ES_ALPHA | ES_UNDER)) {
			int kind;

			for (j = i + 1; es_class[(unsigned char)text[j]]; j++)
				continue;
			kind = es_word(text + i, j - i);
			while (i < j)
				kinds[i++] = (unsigned char)kind;
		} else if (ch == '/' && text[i + 1] == '*') {
			kinds[i] = kinds[i + 1] = ES_COMMENT;
			i += 2;
			in_comment = 1;
		} else if (ch == '/' && text[i + 1] == '/') {
			while (i < n)
				kinds[i++] = ES_COMMENT;
		} else if (ch == '"' || ch == '\'' || (directive == 2 && ch == '<')) {
			int end = ch == '<' ? '>' : ch;

			for (j = i + 1; j < n && text[j] != end; j++)
				if (text[j] == '\\' && j + 1 < n)
					j++;
			j = j < n ? j + 1 : n;
			while (i < j)
				kinds[i++] = ES_STRING;
		} else if (ch == '#' && !directive) {
			for (j = i + 1; j < n && (text[j] == ' ' || text[j] == '\t'); j++)
				continue;
			directive = !strncmp(text + j, "include", 7) ? 2 : 1;
			while (es_class[(unsigned char)text[j]] & ES_ALPHA)
				j++;
			while (i < j)
				kinds[i++] = ES_DIRECTIVE;
		} else if ((is & ES_DIGIT) || (ch == '.' && (es_class[(unsigned char)text[i + 1]] & ES_DIGIT))) {
			for (; i < n && ((es_class[(unsigned char)text[i]] & (ES_ALPHA | ES_DIGIT)) || text[i] == '.'); i++)
				kinds[i] = ES_NUMBER;
		} else {
			if (ch != ' ' && ch != '\t')
				directive = directive ? directive : -1;	/* a # further on is not a directive */
			kinds[i++] = ES_PLAIN;
		}
	}
	return in_comment;
}

/* Only whether the line after n bytes is inside a comment, for a line that is not drawn. */
static int
es_carry(const char *t, int n, int in)
{
	int i = 0, c;

	while (i < n) {
		if (in) {
			for (; i + 1 < n && !(t[i] == '*' && t[i + 1] == '/'); i++)
				continue;
			if (i + 1 >= n)
				return 1;
			i += 2, in = 0;
			continue;
		}
		c = t[i];
		if (c == '/' && i + 1 < n && t[i + 1] == '*')
			in = 1, i++;
		else if (c == '/' && i + 1 < n && t[i + 1] == '/')
			return 0;
		else if (c == '"' || c == '\'')
			for (i++; i < n && t[i] != c; i++)
				i += t[i] == '\\';
		i++;
	}
	return in;
}
#endif
