/* syntax.h: what each character of a line of C is, for its colour (docs/ide.md). A lexer, not
 * a compiler: it knows words, numbers, strings, comments and lines for the preprocessor, and
 * the one thing a line needs from the lines before it, whether it begins inside a comment. */
#ifndef IDE_SYNTAX_H
#define IDE_SYNTAX_H
#include <ctype.h>
#include <string.h>

enum { SX_PLAIN, SX_KEYWORD, SX_TYPE, SX_DIRECTIVE, SX_STRING, SX_NUMBER, SX_COMMENT, SX_CALL, SX_KINDS };

static const char *const sx_keywords[] = { "if", "else", "for", "while", "do", "return", "break", "continue", "switch", "case",
	"default", "goto", "sizeof", "typedef", "struct", "union", "enum", "static", "extern", "const", "volatile", "inline",
	"register", "restrict", NULL };
static const char *const sx_types[] = { "int", "char", "float", "double", "void", "long", "short", "unsigned", "signed",
	"uint8_t", "uint16_t", "uint32_t", "uint64_t", "int8_t", "int16_t", "int32_t", "int64_t", "size_t", "ssize_t", "uintptr_t",
	"bool", "FILE", "NULL", "GLfloat", "GLint", "GLuint", "GLubyte", "GLfixed", "GR_EVENT", "GR_WINDOW_ID", "GR_GC_ID", NULL };

/* What a character may be part of: a name's letters, digits, the underscore. */
enum { SX_ALPHA = 1, SX_DIGIT = 2, SX_UNDER = 4 };
static const unsigned char sx_class[256] = {
	['A' ... 'Z'] = SX_ALPHA, ['a' ... 'z'] = SX_ALPHA, ['0' ... '9'] = SX_DIGIT, ['_'] = SX_UNDER,
};

/* The two lists by their words' first letters, made at the first word looked up. */
static struct { const char *word; unsigned char length, kind; } sx_words[64];
static unsigned char sx_from[129];

static void
sx_index(void)
{
	const char *const *list;
	int c, n = 0, pass;

	for (c = 0; c < 128; c++) {
		sx_from[c] = (unsigned char)n;
		for (pass = 0; pass < 2; pass++)
			for (list = pass ? sx_types : sx_keywords; *list; list++)
				if ((*list)[0] == c) {
					sx_words[n].word = *list;
					sx_words[n].length = (unsigned char)strlen(*list);
					sx_words[n++].kind = pass ? SX_TYPE : SX_KEYWORD;
				}
	}
	sx_from[128] = (unsigned char)n;
}

/* A word's kind: a keyword, a type, or plain. */
static int
sx_word(const char *word, int length)
{
	int c = (unsigned char)word[0], i, k;

	if (!sx_from[128])
		sx_index();
	if (c >= 128)
		return SX_PLAIN;
	for (i = sx_from[c]; i < sx_from[c + 1]; i++) {
		const char *is = sx_words[i].word;

		if (sx_words[i].length != length)
			continue;
		for (k = 1; k < length && is[k] == word[k]; k++)
			continue;
		if (k == length)
			return sx_words[i].kind;
	}
	return SX_PLAIN;
}

/* Fills `kinds` for a line; `in_comment` says whether it begins inside a comment, and what comes
 * back says whether the next line does. */
static int
sx_line(const char *text, int in_comment, unsigned char *kinds)
{
	int i = 0, n = (int)strlen(text), j, kind, directive = 0;

	while (i < n) {
		int ch = (unsigned char)text[i], is = sx_class[ch];

		if (in_comment) {
			for (; i < n && !(text[i] == '*' && text[i + 1] == '/'); i++)
				kinds[i] = SX_COMMENT;
			in_comment = i >= n;
			if (!in_comment) {
				kinds[i] = kinds[i + 1] = SX_COMMENT;
				i += 2;
			}
		} else if (is & (SX_ALPHA | SX_UNDER)) {
			for (j = i + 1; sx_class[(unsigned char)text[j]]; j++)
				continue;
			kind = sx_word(text + i, j - i);
			if (kind == SX_PLAIN) {
				int after = j;

				while (text[after] == ' ')
					after++;
				if (text[after] == '(')
					kind = SX_CALL;
			}
			while (i < j)
				kinds[i++] = (unsigned char)kind;
		} else if (ch == ' ') {
			kinds[i++] = SX_PLAIN;
		} else if (ch == '/' && text[i + 1] == '*') {
			kinds[i] = kinds[i + 1] = SX_COMMENT;
			i += 2;
			in_comment = 1;
		} else if (ch == '/' && text[i + 1] == '/') {
			while (i < n)
				kinds[i++] = SX_COMMENT;
		} else if (ch == '"' || ch == '\'' || (directive == 2 && ch == '<')) {
			int end = ch == '<' ? '>' : ch;

			for (j = i + 1; j < n && text[j] != end; j++)
				if (text[j] == '\\' && j + 1 < n)
					j++;
			j = j < n ? j + 1 : n;
			while (i < j)
				kinds[i++] = SX_STRING;
		} else if (ch == '#' && !directive) {
			/* the directive's own word; what follows is C again (but for #include's name) */
			for (j = i + 1; j < n && text[j] == ' '; j++)
				continue;
			directive = !strncmp(text + j, "include", 7) ? 2 : 1;
			while (sx_class[(unsigned char)text[j]] & SX_ALPHA)
				j++;
			while (i < j)
				kinds[i++] = SX_DIRECTIVE;
		} else if ((is & SX_DIGIT) || (ch == '.' && (sx_class[(unsigned char)text[i + 1]] & SX_DIGIT))) {
			for (; (sx_class[(unsigned char)text[i]] & (SX_ALPHA | SX_DIGIT)) || text[i] == '.'; i++)
				kinds[i] = SX_NUMBER;
		} else {
			directive = directive ? directive : -1;	/* a # further on is not a directive */
			kinds[i++] = SX_PLAIN;
		}
	}
	return in_comment;
}
#endif
