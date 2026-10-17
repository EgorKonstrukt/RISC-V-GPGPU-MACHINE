/*
 * For the files whose code draws the frame (docs/games/quake.md, "The picture on a worker core"): the
 * client's state and the view they read are the copy a frame was given while a worker draws it,
 * and the first core's own otherwise (scene_shaderemu.c). Included after quakedef.h and after
 * the definitions of what it renames; se_real.h undoes it round code of the first core's.
 */
#ifndef SE_SCENE_EXTERNS
#define SE_SCENE_EXTERNS
extern client_state_t	*se_cl_now;
extern entity_t	*se_ents_now, **se_vis_now;
extern int	*se_numvis_now, *se_framecount_now, *se_styles_now;
extern dlight_t	*se_dlights_now;
extern float	*se_frametime_now, *se_blend_now;
extern unsigned	*se_realtime_now;
extern refdef_t	*se_refdef_now;
extern vec3_t	*se_origin_now, *se_vpn_now, *se_vright_now, *se_vup_now;
extern mleaf_t	**se_viewleaf_now, **se_oldviewleaf_now;
extern int	se_scene_split;
#endif

#define cl		(*se_cl_now)
#define cl_entities	se_ents_now
#define cl_visedicts	se_vis_now
#define cl_numvisedicts	(*se_numvis_now)
#define cl_dlights	se_dlights_now
#define host_framecount	(*se_framecount_now)
#define host_frametime	(*se_frametime_now)
#define realtime_ms	(*se_realtime_now)
#define v_blend		se_blend_now
#define r_refdef	(*se_refdef_now)
#define r_origin	(*se_origin_now)
#define vpn		(*se_vpn_now)
#define vright		(*se_vright_now)
#define vup		(*se_vup_now)
#define r_viewleaf	(*se_viewleaf_now)
#define r_oldviewleaf	(*se_oldviewleaf_now)
#define d_lightstylevalue	se_styles_now
