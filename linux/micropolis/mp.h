/* Micropolis's engine as C sees it (engine.cpp): one city at a time. */
#ifndef MICROPOLIS_MP_H
#define MICROPOLIS_MP_H
#ifdef __cplusplus
extern "C" {
#endif

#define MP_W 120	/* the map in tiles */
#define MP_H 100
#define MP_TILE 0x3ff	/* a cell's tile; the bits above are the engine's own */

/* tools (the engine's EditingTool) and the scenarios (its Scenario, 1 to 8) */
enum { MP_RESIDENTIAL, MP_COMMERCIAL, MP_INDUSTRIAL, MP_FIRE, MP_POLICE, MP_QUERY, MP_WIRE, MP_BULLDOZER,
       MP_RAIL, MP_ROAD, MP_STADIUM, MP_PARK, MP_SEAPORT, MP_COAL, MP_NUCLEAR, MP_AIRPORT, MP_TOOLS };
enum { MP_FIRE_DISASTER, MP_FLOOD, MP_TORNADO, MP_EARTHQUAKE, MP_MONSTER, MP_MELTDOWN };

struct mp_status {
	int funds, year, month, population, tax, score;
	int residential, commercial, industrial;	/* demand, -2000 to 2000 */
	int speed, paused, auto_budget, disasters, scenario;
	char name[48];
};

struct mp_sprite {
	int type, frame;	/* 1 to 8 (train ... bus), 1 to that type's frames */
	int x, y;		/* the picture's top left, in the map's pixels */
};

int mp_init(void);	/* in the folder with cities/: 0, or -1 */
int mp_load(const char *path);
void mp_scenario(int n);
void mp_generate(int seed);
int mp_save(const char *path);
void mp_seed(int seed);
void mp_tick(void);	/* a step of the simulation */
/* Tiles x0 to x1 and y0 to y1 (both included) into a ring of `across` x `across` cells (a power
 * of two) where they wrap; with `animate`, a step of their animated tiles (fire, traffic) first.
 * The whole map's animation is 110 thousand instructions: a front end steps only what it shows. */
void mp_view(int x0, int y0, int x1, int y1, unsigned short *ring, int across, int animate);
const unsigned short *mp_map(void);	/* a column of MP_H cells after another */
int mp_sprites(struct mp_sprite *to, int most);
int mp_tool(int tool, int x, int y);	/* the engine's ToolResult: 1 done, 0 not here, -1 bulldoze first, -2 no money */
int mp_tool_size(int tool);
int mp_tool_cost(int tool);
void mp_status(struct mp_status *to);
void mp_set_speed(int speed);	/* 0 to 3 */
void mp_pause(int paused);
void mp_set_tax(int percent);
void mp_set_auto_budget(int on);
void mp_set_disasters(int on);
void mp_disaster(int which);
/* What the engine said since the last call: a message's number in stri.301, a query's six
 * numbers, a won or lost scenario. 0 when nothing is waiting. */
enum { MP_SAID_NOTHING, MP_SAID_MESSAGE, MP_SAID_QUERY, MP_SAID_WON, MP_SAID_LOST, MP_SAID_SOUND, MP_SAID_BUDGET };
struct mp_said {
	int kind, number, x, y;
	int query[6];
	char sound[24];
};
int mp_next_said(struct mp_said *to);
/* The overview map's layers. For each tile, row by row: the tile it shows (dirt where a layer
 * leaves it out) and a level over it, 0 for none: 1 unpowered, 2 powered, 3 a conductor for
 * MP_SHOW_POWER; 1 to 4 low to very high for the others (growth: 1, 2 falling, 3, 4 rising). */
enum { MP_SHOW_ALL, MP_SHOW_RESIDENTIAL, MP_SHOW_COMMERCIAL, MP_SHOW_INDUSTRIAL, MP_SHOW_TRANSPORT, MP_SHOW_POWER,
       MP_SHOW_DENSITY, MP_SHOW_GROWTH, MP_SHOW_TRAFFIC, MP_SHOW_POLLUTION, MP_SHOW_CRIME, MP_SHOW_VALUE,
       MP_SHOW_FIRE, MP_SHOW_POLICE, MP_SHOWS };
void mp_overview(int show, unsigned short *tiles, unsigned char *levels);

struct mp_budget {
	int tax_rate, taxes;			/* the year's taxes, as the last budget counted them */
	int wanted[3], percent[3], spent[3];	/* roads, fire, police */
	int funds, cash_flow;
};
void mp_budget(struct mp_budget *to);
/* Funding of roads, fire and police in percent, the difference in the year's spending paid now. */
void mp_set_budget(const int percent[3]);

struct mp_evaluation {
	int yes, score, score_delta, population, population_delta, assessed, city_class, level;
	int problems[4], votes[4];		/* the worst problems (0 crime ... 6 fire, -1 none), percent */
};
void mp_evaluation(struct mp_evaluation *to);
/* 120 values of residential, commercial, industrial, money, crime and pollution (0 to 5), the
 * newest first; `years120` the long scale. */
void mp_history(int which, int years120, short *to);

unsigned mp_sum(void);	/* of the map and the city's numbers: the test's */
/* "CITY,TICKS[,SEED]" (CITY a file, or a scenario's number): runs it and prints the sum */
int mp_sum_test(const char *spec);

#ifdef __cplusplus
}
#endif
#endif
