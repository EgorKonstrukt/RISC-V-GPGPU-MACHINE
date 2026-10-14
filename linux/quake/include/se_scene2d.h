/*
 * For the files whose code draws the 2D screen (gl_screen.c, sbar.c): the client's state they
 * read is the copy of the frame whose screen a worker records, and the first core's own otherwise
 * (scene_shaderemu.c, "The 2D screen on a worker of its own"). Included after quakedef.h.
 */
#ifndef SE_SCENE2D_EXTERNS
#define SE_SCENE2D_EXTERNS
extern client_state_t	*se2_cl_now;
extern int	*se2_framecount_now;
extern float	*se2_frametime_now;
extern unsigned	*se2_realtime_now;
#endif

#define cl		(*se2_cl_now)
#define host_framecount	(*se2_framecount_now)
#define host_frametime	(*se2_frametime_now)
#define realtime_ms	(*se2_realtime_now)
