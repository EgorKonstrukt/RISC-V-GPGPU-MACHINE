/*
 * A model's triangles for the GPU, in place of gl_mesh.c: three vertices a triangle in the
 * model's own order, and beside them each one's texture coordinates as a tagged compact
 * vertex holds them (docs/gpu.md). GLQuake looked for long strips and fans here, a search per model.
 */
#include <stdlib.h>
#include "quakedef.h"

void GL_MakeAliasModelDisplayLists (model_t *m, aliashdr_t *hdr)
{
	int		i, j, k, order = hdr->numtris * 3;
	unsigned	*coords, *to;
	int		*which;		/* the vertex of the model each of the triangles' is */
	trivertx_t	*verts;

	hdr->poseverts = order;
	coords = Hunk_AllocUnset (order * 4, "coords");
	hdr->commands = (byte *)coords - (byte *)hdr;
	for (i = 0; i < hdr->numtris; i++)
		for (j = 0; j < 3; j++) {
			int s, t;

			k = triangles[i].vertindex[j];
			s = stverts[k].s, t = stverts[k].t;
			if (!triangles[i].facesfront && stverts[k].onseam)
				s += hdr->skinwidth / 2;	/* on the back side */
			/* the middle of the texel, in 1,024ths of the skin */
			*coords++ = ((unsigned)((2 * s + 1) * 512 / hdr->skinwidth) & 0xfff) |
				((unsigned)((2 * t + 1) * 512 / hdr->skinheight) & 0xfff) << 12;
		}

	/* (every byte of both is written here: not cleared first) */
	verts = Hunk_AllocUnset (hdr->numposes * order * sizeof (trivertx_t), "poses");
	hdr->posedata = (byte *)verts - (byte *)hdr;
	which = malloc (order * sizeof (int));
	if (!which)
		Sys_Error ("no memory for a model's %i vertices", order);
	for (j = 0; j < hdr->numtris; j++)
		for (k = 0; k < 3; k++)
			which[j * 3 + k] = triangles[j].vertindex[k];
	for (i = 0, to = (unsigned *)verts; i < hdr->numposes; i++, to += order) {
		const trivertx_t *pose = poseverts[i];

		/* a vertex is four bytes: a word at a time where the file has them at words */
		if (((unsigned long)pose & 3) == 0)
			for (j = 0; j < order; j++)
				to[j] = ((const unsigned *)pose)[which[j]];
		else
			for (j = 0; j < order; j++)
				memcpy (to + j, pose + which[j], 4);
	}
	free (which);
}
