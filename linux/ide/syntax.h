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

static int
sx_word(const char *const *list, const char *word, int length)
{
	for (; *list; list++)
		if ((*list)[0] == word[0] && (int)strlen(*list) == length && !memcmp(*list, word, (size_t)length))
			return 1;
	return 0;
}

/* Fills `kinds` for a line; `in_comment` says whether it begins inside one, and what comes
 * back says whether the next line does. */
static int
sx_line(const char *text, int in_comment, unsigned char *kinds)
{
	int i = 0, n = (int)strlen(text), j, kind, directive = 0;

	while (i < n) {
		int ch = (unsigned char)text[i];

		if (in_comment) {
			for (j = i; j < n && !(text[j] == '*' && text[j + 1] == '/'); j++)
				continue;
			in_comment = j >= n;
			j = in_comment ? n : j + 2;
			memset(kinds + i, SX_COMMENT, (size_t)(j - i));
			i = j;
		} else if (ch == '/' && text[i + 1] == '*') {
			kinds[i] = kinds[i + 1] = SX_COMMENT;
			i += 2;
			in_comment = 1;
		} else if (ch == '/' && text[i + 1] == '/') {
			memset(kinds + i, SX_COMMENT, (size_t)(n - i));
			i = n;
		} else if (ch == '"' || ch == '\'' || (directive == 2 && ch == '<')) {
			int end = ch == '<' ? '>' : ch;

			for (j = i + 1; j < n && text[j] != end; j++)
				if (text[j] == '\\' && j + 1 < n)
					j++;
			j = j < n ? j + 1 : n;
			memset(kinds + i, SX_STRING, (size_t)(j - i));
			i = j;
		} else if (ch == '#' && !directive) {
			/* the directive's own word; what follows is C again (but for #include's name) */
			for (j = i + 1; j < n && text[j] == ' '; j++)
				continue;
			directive = !strncmp(text + j, "include", 7) ? 2 : 1;
			while (j < n && isalpha((unsigned char)text[j]))
				j++;
			memset(kinds + i, SX_DIRECTIVE, (size_t)(j - i));
			i = j;
		} else if (isdigit(ch) || (ch == '.' && isdigit((unsigned char)text[i + 1]))) {
			for (j = i; j < n && (isalnum((unsigned char)text[j]) || text[j] == '.'); j++)
				continue;
			memset(kinds + i, SX_NUMBER, (size_t)(j - i));
			i = j;
		} else if (isalpha(ch) || ch == '_') {
			for (j = i; j < n && (isalnum((unsigned char)text[j]) || text[j] == '_'); j++)
				continue;
			kind = sx_word(sx_keywords, text + i, j - i) ? SX_KEYWORD : sx_word(sx_types, text + i, j - i) ? SX_TYPE : SX_PLAIN;
			if (kind == SX_PLAIN) {
				int after = j;

				while (text[after] == ' ')
					after++;
				if (text[after] == '(')
					kind = SX_CALL;
			}
			memset(kinds + i, kind, (size_t)(j - i));
			i = j;
		} else {
			if (ch != ' ')
				directive = directive ? directive : -1;	/* a # further on is not a directive */
			kinds[i++] = SX_PLAIN;
		}
	}
	return in_comment;
}
#endif
