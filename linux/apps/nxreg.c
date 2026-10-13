/*
 * nxreg: the registry (registry.h, docs/persist.md) for a script.
 *
 *	nxreg get KEY [OTHERWISE]	prints the key's value (or OTHERWISE; exit 1 with neither)
 *	nxreg set KEY VALUE		nxreg unset KEY		nxreg list [PREFIX]
 */
#include "registry.h"

int
main(int argc, char **argv)
{
	const char *value;
	char *at;

	if (argc > 2 && !strcmp(argv[1], "get")) {
		value = reg_get(argv[2], argc > 3 ? argv[3] : NULL);
		if (value)
			printf("%s\n", value);
		return value ? 0 : 1;
	}
	if (argc > 3 && !strcmp(argv[1], "set"))
		return !reg_set(argv[2], argv[3]);
	if (argc > 2 && !strcmp(argv[1], "unset"))
		return !reg_set(argv[2], NULL);
	if (argc > 1 && !strcmp(argv[1], "list")) {
		reg_load();
		for (at = reg_text; *at; at += strcspn(at, "\n") + (strchr(at, '\n') != NULL))
			if (argc < 3 || !strncmp(at, argv[2], strlen(argv[2])))
				printf("%.*s\n", (int)strcspn(at, "\n"), at);
		return 0;
	}
	fprintf(stderr, "usage: nxreg get KEY [OTHERWISE] | set KEY VALUE | unset KEY | list [PREFIX]\n");
	return 2;
}
