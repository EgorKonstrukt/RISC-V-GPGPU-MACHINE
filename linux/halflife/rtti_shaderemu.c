/*
rtti_shaderemu.c - the Itanium ABI's type information and __dynamic_cast for freevgui's casts,
in place of libsupc++ (the toolchain's is built for the double-float ABI)
*/
#include <stddef.h>
#include <string.h>

typedef struct type_info { const void *vptr; const char *name; } type_info;
typedef struct { type_info t; const type_info *base; } si_type_info;
typedef struct { const type_info *type; long offset_flags; } base_info;
typedef struct { type_info t; unsigned flags, count; base_info base[]; } vmi_type_info;

// the vtables the compiler points type information at: nothing calls through them
const void *class_vt[8] __asm__( "_ZTVN10__cxxabiv117__class_type_infoE" );
const void *si_vt[8] __asm__( "_ZTVN10__cxxabiv120__si_class_type_infoE" );
const void *vmi_vt[8] __asm__( "_ZTVN10__cxxabiv121__vmi_class_type_infoE" );

static int same( const type_info *a, const type_info *b )
{
	return a == b || ( a->name[0] != '*' && b->name[0] != '*' && !strcmp( a->name, b->name ));
}

// where in the object at `obj` (of type `t`) a public `want` lies, or NULL
static const char *find( const char *obj, const type_info *t, const type_info *want )
{
	const vmi_type_info *vmi = (const vmi_type_info *)t;
	const char *got;
	unsigned i;
	long off;

	if( same( t, want ))
		return obj;
	if( t->vptr == &si_vt[2] )
		return find( obj, ((const si_type_info *)t)->base, want );
	if( t->vptr != &vmi_vt[2] )
		return NULL;
	for( i = 0; i < vmi->count; i++ )
	{
		if( !( vmi->base[i].offset_flags & 2 ))
			continue;
		off = vmi->base[i].offset_flags >> 8;
		if( vmi->base[i].offset_flags & 1 )
			off = *(const ptrdiff_t *)( *(const char *const *)obj + off );
		if(( got = find( obj + off, vmi->base[i].type, want )))
			return got;
	}
	return NULL;
}

void *__dynamic_cast( const void *src, const type_info *src_type, const type_info *dst_type, ptrdiff_t src2dst )
{
	const void *const *vptr = *(const void *const *const *)src;
	const char *whole = (const char *)src + (ptrdiff_t)vptr[-2];
	const type_info *whole_type = vptr[-1];

	(void)src_type;
	if( src2dst >= 0 && find( whole, whole_type, dst_type ) == (const char *)src - src2dst )
		return (char *)src - src2dst;
	return (void *)find( whole, whole_type, dst_type );
}
