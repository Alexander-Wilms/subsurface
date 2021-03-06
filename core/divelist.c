// SPDX-License-Identifier: GPL-2.0
/* divelist.c */
/* core logic for the dive list -
 * accessed through the following interfaces:
 *
 * struct trip_table trip_table
 * void process_loaded_dives()
 * void process_imported_dives(bool prefer_imported)
 * unsigned int amount_selected
 * void dump_selection(void)
 * void get_dive_gas(const struct dive *dive, int *o2_p, int *he_p, int *o2low_p)
 * char *get_dive_gas_string(const struct dive *dive)
 * int total_weight(const struct dive *dive)
 * int get_divenr(const struct dive *dive)
 * int get_divesite_idx(const struct dive_site *ds)
 * int init_decompression(struct dive *dive)
 * void update_cylinder_related_info(struct dive *dive)
 * void dump_trip_list(void)
 * void insert_trip(dive_trip_t *dive_trip_p, struct trip_table *trip_table)
 * void unregister_trip(dive_trip_t *trip, struct trip_table *table)
 * void free_trip(dive_trip_t *trip)
 * void remove_dive_from_trip(struct dive *dive, struct trip_table *trip_table)
 * struct dive_trip *unregister_dive_from_trip(struct dive *dive, struct trip_table *trip_table)
 * void add_dive_to_trip(struct dive *dive, dive_trip_t *trip)
 * dive_trip_t *create_and_hookup_trip_from_dive(struct dive *dive, struct trip_table *trip_table)
 * dive_trip_t *get_dives_to_autogroup(sruct dive_table *table, int start, int *from, int *to, bool *allocated)
 * dive_trip_t *get_trip_for_new_dive(struct dive *new_dive, bool *allocated)
 * void combine_trips(struct dive_trip *trip_a, struct dive_trip *trip_b)
 * dive_trip_t *combine_trips_create(struct dive_trip *trip_a, struct dive_trip *trip_b)
 * struct dive *unregister_dive(int idx)
 * void delete_single_dive(int idx)
 * void add_single_dive(int idx, struct dive *dive)
 * void select_dive(struct dive *dive)
 * void deselect_dive(struct dive *dive)
 * void mark_divelist_changed(int changed)
 * int unsaved_changes()
 * bool dive_less_than(const struct dive *a, const struct dive *b)
 * bool trip_less_than(const struct dive_trip *a, const struct dive_trip *b)
 * bool dive_or_trip_less_than(struct dive_or_trip a, struct dive_or_trip b)
 * void sort_dive_table(struct dive_table *table)
 * void sort_trip_table(struct trip_table *table)
 * bool is_trip_before_after(const struct dive *dive, bool before)
 * void delete_dive_from_table(struct dive_table *table, int idx)
 * int find_next_visible_dive(timestamp_t when);
 * void clear_dive_file_data()
 * void clear_table(struct dive_table *table)
 * bool trip_is_single_day(const struct dive_trip *trip)
 * int trip_shown_dives(const struct dive_trip *trip)
 */
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include "gettext.h"
#include <assert.h>
#include <zip.h>
#include <libxslt/transform.h>

#include "dive.h"
#include "subsurface-string.h"
#include "divelist.h"
#include "display.h"
#include "planner.h"
#include "qthelper.h"
#include "git-access.h"

static bool dive_list_changed = false;

bool autogroup = false;

struct trip_table trip_table;

unsigned int amount_selected;

#if DEBUG_SELECTION_TRACKING
void dump_selection(void)
{
	int i;
	struct dive *dive;

	printf("currently selected are %u dives:", amount_selected);
	for_each_dive(i, dive) {
		if (dive->selected)
			printf(" %d", i);
	}
	printf("\n");
}
#endif

void set_autogroup(bool value)
{
	/* if we keep the UI paradigm, this needs to toggle
	 * the checkbox on the autogroup menu item */
	autogroup = value;
}

/*
 * Get "maximal" dive gas for a dive.
 * Rules:
 *  - Trimix trumps nitrox (highest He wins, O2 breaks ties)
 *  - Nitrox trumps air (even if hypoxic)
 * These are the same rules as the inter-dive sorting rules.
 */
void get_dive_gas(const struct dive *dive, int *o2_p, int *he_p, int *o2max_p)
{
	int i;
	int maxo2 = -1, maxhe = -1, mino2 = 1000;


	for (i = 0; i < MAX_CYLINDERS; i++) {
		const cylinder_t *cyl = dive->cylinder + i;
		int o2 = get_o2(cyl->gasmix);
		int he = get_he(cyl->gasmix);

		if (!is_cylinder_used(dive, i))
			continue;
		if (cylinder_none(cyl))
			continue;
		if (o2 > maxo2)
			maxo2 = o2;
		if (he > maxhe)
			goto newmax;
		if (he < maxhe)
			continue;
		if (o2 <= maxo2)
			continue;
	newmax:
		maxhe = he;
		mino2 = o2;
	}
	/* All air? Show/sort as "air"/zero */
	if ((!maxhe && maxo2 == O2_IN_AIR && mino2 == maxo2) ||
			(maxo2 == -1 && maxhe == -1 && mino2 == 1000))
		maxo2 = mino2 = 0;
	*o2_p = mino2;
	*he_p = maxhe;
	*o2max_p = maxo2;
}

int total_weight(const struct dive *dive)
{
	int i, total_grams = 0;

	if (dive)
		for (i = 0; i < MAX_WEIGHTSYSTEMS; i++)
			total_grams += dive->weightsystem[i].weight.grams;
	return total_grams;
}

static int active_o2(const struct dive *dive, const struct divecomputer *dc, duration_t time)
{
	struct gasmix gas = get_gasmix_at_time(dive, dc, time);
	return get_o2(gas);
}

/* Calculate OTU for a dive - this only takes the first divecomputer into account.
   Implement the protocol in Erik Baker's document "Oxygen Toxicity Calculations". This code
   implements a third-order continuous approximation of Baker's Eq. 2 and enables OTU
   calculation for rebreathers. Baker obtained his information from:
   Comroe Jr. JH et al. (1945)  Oxygen toxicity. J. Am. Med. Assoc. 128,710-717
   Clark JM & CJ Lambertsen (1970) Pulmonary oxygen tolerance in man and derivation of pulmonary
      oxygen tolerance curves. Inst. env. Med. Report 1-70, University of Pennsylvania, Philadelphia, USA. */
static int calculate_otu(const struct dive *dive)
{
	int i;
	double otu = 0.0;
	const struct divecomputer *dc = &dive->dc;
	for (i = 1; i < dc->samples; i++) {
		int t;
		int po2i, po2f;
		double pm;
		struct sample *sample = dc->sample + i;
		struct sample *psample = sample - 1;
		t = sample->time.seconds - psample->time.seconds;
		if (sample->o2sensor[0].mbar) {			// if dive computer has o2 sensor(s) (CCR & PSCR) ..
			po2i = psample->o2sensor[0].mbar;
			po2f = sample->o2sensor[0].mbar;	// ... use data from the first o2 sensor
		} else {
			if (dc->divemode == CCR) {
				po2i = psample->setpoint.mbar;		// if CCR has no o2 sensors then use setpoint
				po2f = sample->setpoint.mbar;
			} else {						// For OC and rebreather without o2 sensor/setpoint
				int o2 = active_o2(dive, dc, psample->time);	// 	... calculate po2 from depth and FiO2.
				po2i = lrint(o2 * depth_to_atm(psample->depth.mm, dive));	// (initial) po2 at start of segment
				po2f = lrint(o2 * depth_to_atm(sample->depth.mm, dive));	// (final) po2 at end of segment
			}
		}
		if ((po2i > 500) || (po2f > 500)) {			// If PO2 in segment is above 500 mbar then calculate otu
			if (po2i <= 500) {				// For descent segment with po2i <= 500 mbar ..
				t = t * (po2f - 500) / (po2f - po2i);	// .. only consider part with PO2 > 500 mbar
				po2i = 501;				// Mostly important for the dive planner with long segments
			} else {
				if (po2f <= 500){
					t = t * (po2i - 500) / (po2i - po2f);	// For ascent segment with po2f <= 500 mbar ..
					po2f = 501;				// .. only consider part with PO2 > 500 mbar
				}
			}
			pm = (po2f + po2i)/1000.0 - 1.0;
			// This is a 3rd order continuous approximation of Baker's eq. 2, therefore Baker's eq. 1 is not used:
			otu += t / 60.0 * pow(pm, 5.0/6.0) * (1.0 - 5.0 * (po2f - po2i) * (po2f - po2i) / 216000000.0 / (pm * pm));
		}
	}
	return lrint(otu);
}
/* Table of maximum oxygen exposure durations, used in CNS calulations.
   This table shows the official NOAA maximum O2 exposure limits (in seconds) for different PO2 values. It also gives
   slope values for linear interpolation for intermediate PO2 values between the tabulated PO2 values in the 1st column.
   Top & bottom rows are inserted that are not in the NOAA table: (1) For PO2 > 1.6 the same slope value as between
   1.5 & 1.6 is used. This exptrapolation for PO2 > 1.6 likely gives an underestimate above 1.6 but is better than the
   value for PO2=1.6 (45 min). (2) The NOAA table only tabulates values for PO2 >= 0.6. Since O2-uptake occurs down to
   PO2=0.5, the same slope is used as for 0.7 > PO2 > 0.6. This gives a conservative estimate for 0.6 > PO2 > 0.5. To
   preserve the integer structure of the table, all slopes are given as slope*10: divide by 10 to get the valid slope.
   The columns below are: 
   po2 (mbar), Maximum Single Exposure (seconds), single_slope, Maximum 24 hour Exposure (seconds), 24h_slope */
int const cns_table[][5] = {
	{ 1600,  45 * 60, 456, 150 * 60, 180 },
	{ 1550,  83 * 60, 456, 165 * 60, 180 },
	{ 1500, 120 * 60, 444, 180 * 60, 180 },
	{ 1450, 135 * 60, 180, 180 * 60,  00 },
	{ 1400, 150 * 60, 180, 180 * 60,  00 },
	{ 1350, 165 * 60, 180, 195 * 60, 180 },
	{ 1300, 180 * 60, 180, 210 * 60, 180 },
	{ 1250, 195 * 60, 180, 225 * 60, 180 },
	{ 1200, 210 * 60, 180, 240 * 60, 180 },
	{ 1100, 240 * 60, 180, 270 * 60, 180 },
	{ 1000, 300 * 60, 360, 300 * 60, 180 },
	{ 900,  360 * 60, 360, 360 * 60, 360 },
	{ 800,  450 * 60, 540, 450 * 60, 540 },
	{ 700,  570 * 60, 720, 570 * 60, 720 },
	{ 600,  720 * 60, 900, 720 * 60, 900 },
	{ 500,  870 * 60, 900, 870 * 60, 900 }
};

/* Calculate the CNS for a single dive  - this only takes the first divecomputer into account.
   The CNS contributions are summed for dive segments defined by samples. The maximum O2 exposure duration for each
   segment is calculated based on the mean depth of the two samples (start & end) that define each segment. The CNS
   contribution of each segment is found by dividing the time duration of the segment by its maximum exposure duration.
   The contributions of all segments of the dive are summed to get the total CNS% value. This is a partial implementation
   of the proposals in Erik Baker's document "Oxygen Toxicity Calculations" using fixed-depth calculations for the mean
   po2 for each segment. Empirical testing showed that, for large changes in depth, the cns calculation for the mean po2
   value is extremely close, if not identical to the additive calculations for 0.1 bar increments in po2 from the start
   to the end of the segment, assuming a constant rate of change in po2 (i.e. depth) with time. */
static double calculate_cns_dive(const struct dive *dive)
{
	int n;
	size_t j;
	const struct divecomputer *dc = &dive->dc;
	double cns = 0.0;
	/* Calculate the CNS for each sample in this dive and sum them */
	for (n = 1; n < dc->samples; n++) {
		int t;
		int po2i, po2f;
		bool trueo2 = false;
		struct sample *sample = dc->sample + n;
		struct sample *psample = sample - 1;
		t = sample->time.seconds - psample->time.seconds;
		if (sample->o2sensor[0].mbar) {			// if dive computer has o2 sensor(s) (CCR & PSCR)
			po2i = psample->o2sensor[0].mbar;
			po2f = sample->o2sensor[0].mbar;	// then use data from the first o2 sensor
			trueo2 = true;
		}
		if ((dc->divemode == CCR) && (!trueo2)) {
			po2i = psample->setpoint.mbar;		// if CCR has no o2 sensors then use setpoint
			po2f = sample->setpoint.mbar;
			trueo2 = true;
		}
		if (!trueo2) {
			int o2 = active_o2(dive, dc, psample->time);			// For OC and rebreather without o2 sensor:
			po2i = lrint(o2 * depth_to_atm(psample->depth.mm, dive));	// (initial) po2 at start of segment
			po2f = lrint(o2 * depth_to_atm(sample->depth.mm, dive));	// (final) po2 at end of segment
		}
		po2i = (po2i + po2f) / 2;	// po2i now holds the mean po2 of initial and final po2 values of segment.
		/* Don't increase CNS when po2 below 500 matm */
		if (po2i <= 500)
			continue;
		/* Find the table-row for calculating the maximum exposure at this PO2 */
		for (j = 1; j < sizeof(cns_table) / (sizeof(int) * NO_COLUMNS); j++)
			if (po2i > cns_table[j][PO2VAL])
				break;
		/* Increment CNS with simple linear interpolation: 100 * time / (single-exposure-time + delta-PO2 * single-slope) */
		cns += (double)t / ((double)cns_table[j][SINGLE_EXP] - ((double)po2i - (double)cns_table[j][PO2VAL]) * (double)cns_table[j][SINGLE_SLOPE] / 10.0) * 100;
	}
	return cns;
}

/* this only gets called if dive->maxcns == 0 which means we know that
 * none of the divecomputers has tracked any CNS for us
 * so we calculated it "by hand" */
static int calculate_cns(struct dive *dive)
{
	int i, divenr;
	double cns = 0.0;
	timestamp_t last_starttime, last_endtime = 0;

	/* shortcut */
	if (dive->cns)
		return dive->cns;

	divenr = get_divenr(dive);
	i = divenr >= 0 ? divenr : dive_table.nr;
#if DECO_CALC_DEBUG & 2
	if (i >= 0 && i < dive_table.nr)
		printf("\n\n*** CNS for dive #%d %d\n", i, get_dive(i)->number);
	else
		printf("\n\n*** CNS for dive #%d\n", i);
#endif
	/* Look at next dive in dive list table and correct i when needed */
	while (i < dive_table.nr - 1) {
		struct dive *pdive = get_dive(i);
		if (!pdive || pdive->when > dive->when)
			break;
		i++;
	}
	/* Look at previous dive in dive list table and correct i when needed */
	while (i > 0) {
		struct dive *pdive = get_dive(i - 1);
		if (!pdive || pdive->when < dive->when)
			break;
		i--;
	}
#if DECO_CALC_DEBUG & 2
	printf("Dive number corrected to #%d\n", i);
#endif
	last_starttime = dive->when;
	/* Walk backwards to check previous dives - how far do we need to go back? */
	while (i--) {
		if (i == divenr && i > 0)
			i--;
#if DECO_CALC_DEBUG & 2
		printf("Check if dive #%d %d has to be considered as prev dive: ", i, get_dive(i)->number);
#endif
		struct dive *pdive = get_dive(i);
		/* we don't want to mix dives from different trips as we keep looking
		 * for how far back we need to go */
		if (dive->divetrip && pdive->divetrip != dive->divetrip) {
#if DECO_CALC_DEBUG & 2
			printf("No - other dive trip\n");
#endif
			continue;
		}
		if (!pdive || pdive->when >= dive->when || dive_endtime(pdive) + 12 * 60 * 60 < last_starttime) {
#if DECO_CALC_DEBUG & 2
			printf("No\n");
#endif
			break;
		}
		last_starttime = pdive->when;
#if DECO_CALC_DEBUG & 2
		printf("Yes\n");
#endif
	}
	/* Walk forward and add dives and surface intervals to CNS */
	while (++i < dive_table.nr) {
#if DECO_CALC_DEBUG & 2
		printf("Check if dive #%d %d will be really added to CNS calc: ", i, get_dive(i)->number);
#endif
		struct dive *pdive = get_dive(i);
		/* again skip dives from different trips */
		if (dive->divetrip && dive->divetrip != pdive->divetrip) {
#if DECO_CALC_DEBUG & 2
			printf("No - other dive trip\n");
#endif
			continue;
		}
		/* Don't add future dives */
		if (pdive->when >= dive->when) {
#if DECO_CALC_DEBUG & 2
			printf("No - future or same dive\n");
#endif
			break;
		}
		/* Don't add the copy of the dive itself */
		if (i == divenr) {
#if DECO_CALC_DEBUG & 2
			printf("No - copy of dive\n");
#endif
			continue;
		}
#if DECO_CALC_DEBUG & 2
		printf("Yes\n");
#endif

		/* CNS reduced with 90min halftime during surface interval */
		if (last_endtime)
			cns /= pow(2, (pdive->when - last_endtime) / (90.0 * 60.0));
#if DECO_CALC_DEBUG & 2
		printf("CNS after surface interval: %f\n", cns);
#endif

		cns += calculate_cns_dive(pdive);
#if DECO_CALC_DEBUG & 2
		printf("CNS after previous dive: %f\n", cns);
#endif

		last_starttime = pdive->when;
		last_endtime = dive_endtime(pdive);
	}

	/* CNS reduced with 90min halftime during surface interval */
	if (last_endtime)
		cns /= pow(2, (dive->when - last_endtime) / (90.0 * 60.0));
#if DECO_CALC_DEBUG & 2
	printf("CNS after last surface interval: %f\n", cns);
#endif

	cns += calculate_cns_dive(dive);
#if DECO_CALC_DEBUG & 2
	printf("CNS after dive: %f\n", cns);
#endif

	/* save calculated cns in dive struct */
	dive->cns = lrint(cns);
	return dive->cns;
}
/*
 * Return air usage (in liters).
 */
static double calculate_airuse(const struct dive *dive)
{
	int airuse = 0;
	int i;

	for (i = 0; i < MAX_CYLINDERS; i++) {
		pressure_t start, end;
		const cylinder_t *cyl = dive->cylinder + i;

		start = cyl->start.mbar ? cyl->start : cyl->sample_start;
		end = cyl->end.mbar ? cyl->end : cyl->sample_end;
		if (!end.mbar || start.mbar <= end.mbar) {
			// If a cylinder is used but we do not have info on amout of gas used
			// better not pretend we know the total gas use.
			// Eventually, logic should be fixed to compute average depth and total time
			// for those segments where cylinders with known pressure drop are breathed from.
			if (is_cylinder_used(dive, i))
				return 0.0;
			else
				continue;
		}

		airuse += gas_volume(cyl, start) - gas_volume(cyl, end);
	}
	return airuse / 1000.0;
}

/* this only uses the first divecomputer to calculate the SAC rate */
static int calculate_sac(const struct dive *dive)
{
	const struct divecomputer *dc = &dive->dc;
	double airuse, pressure, sac;
	int duration, meandepth;

	airuse = calculate_airuse(dive);
	if (!airuse)
		return 0;

	duration = dc->duration.seconds;
	if (!duration)
		return 0;

	meandepth = dc->meandepth.mm;
	if (!meandepth)
		return 0;

	/* Mean pressure in ATM (SAC calculations are in atm*l/min) */
	pressure = depth_to_atm(meandepth, dive);
	sac = airuse / pressure * 60 / duration;

	/* milliliters per minute.. */
	return lrint(sac * 1000);
}

/* for now we do this based on the first divecomputer */
static void add_dive_to_deco(struct deco_state *ds, struct dive *dive)
{
	struct divecomputer *dc = &dive->dc;
	struct gasmix gasmix = gasmix_air;
	int i;
	const struct event *ev = NULL, *evd = NULL;
	enum divemode_t current_divemode = UNDEF_COMP_TYPE;

	if (!dc)
		return;

	for (i = 1; i < dc->samples; i++) {
		struct sample *psample = dc->sample + i - 1;
		struct sample *sample = dc->sample + i;
		int t0 = psample->time.seconds;
		int t1 = sample->time.seconds;
		int j;

		for (j = t0; j < t1; j++) {
			int depth = interpolate(psample->depth.mm, sample->depth.mm, j - t0, t1 - t0);
			gasmix = get_gasmix(dive, dc, j, &ev, gasmix);
			add_segment(ds, depth_to_bar(depth, dive), gasmix, 1, sample->setpoint.mbar,
				get_current_divemode(&dive->dc, j, &evd, &current_divemode), dive->sac);
		}
	}
}

int get_divenr(const struct dive *dive)
{
	int i;
	const struct dive *d;
	// tempting as it may be, don't die when called with dive=NULL
	if (dive)
		for_each_dive(i, d) {
			if (d->id == dive->id) // don't compare pointers, we could be passing in a copy of the dive
				return i;
		}
	return -1;
}

int get_divesite_idx(const struct dive_site *ds)
{
	int i;
	const struct dive_site *d;
	// tempting as it may be, don't die when called with dive=NULL
	if (ds)
		for_each_dive_site(i, d) {
			if (d->uuid == ds->uuid) // don't compare pointers, we could be passing in a copy of the dive
				return i;
		}
	return -1;
}

static struct gasmix air = { .o2.permille = O2_IN_AIR, .he.permille = 0 };

/* take into account previous dives until there is a 48h gap between dives */
/* return last surface time before this dive or dummy value of 48h */
/* return negative surface time if dives are overlapping */
/* The place you call this function is likely the place where you want
 * to create the deco_state */
int init_decompression(struct deco_state *ds, struct dive *dive)
{
	int i, divenr = -1;
	int surface_time = 48 * 60 * 60;
	timestamp_t last_endtime = 0, last_starttime = 0;
	bool deco_init = false;
	double surface_pressure;

	if (!dive)
		return false;

	divenr = get_divenr(dive);
	i = divenr >= 0 ? divenr : dive_table.nr;
#if DECO_CALC_DEBUG & 2
	if (i >= 0 && i < dive_table.nr)
		printf("\n\n*** Init deco for dive #%d %d\n", i, get_dive(i)->number);
	else
		printf("\n\n*** Init deco for dive #%d\n", i);
#endif
	/* Look at next dive in dive list table and correct i when needed */
	while (i < dive_table.nr - 1) {
		struct dive *pdive = get_dive(i);
		if (!pdive || pdive->when > dive->when)
			break;
		i++;
	}
	/* Look at previous dive in dive list table and correct i when needed */
	while (i > 0) {
		struct dive *pdive = get_dive(i - 1);
		if (!pdive || pdive->when < dive->when)
			break;
		i--;
	}
#if DECO_CALC_DEBUG & 2
	printf("Dive number corrected to #%d\n", i);
#endif
	last_starttime = dive->when;
	/* Walk backwards to check previous dives - how far do we need to go back? */
	while (i--) {
		if (i == divenr && i > 0)
			i--;
#if DECO_CALC_DEBUG & 2
		printf("Check if dive #%d %d has to be considered as prev dive: ", i, get_dive(i)->number);
#endif
		struct dive *pdive = get_dive(i);
		/* we don't want to mix dives from different trips as we keep looking
		 * for how far back we need to go */
		if (dive->divetrip && pdive->divetrip != dive->divetrip) {
#if DECO_CALC_DEBUG & 2
			printf("No - other dive trip\n");
#endif
			continue;
		}
		if (!pdive || pdive->when >= dive->when || dive_endtime(pdive) + 48 * 60 * 60 < last_starttime) {
#if DECO_CALC_DEBUG & 2
			printf("No\n");
#endif
			break;
		}
		last_starttime = pdive->when;
#if DECO_CALC_DEBUG & 2
		printf("Yes\n");
#endif
	}
	/* Walk forward an add dives and surface intervals to deco */
	while (++i < dive_table.nr) {
#if DECO_CALC_DEBUG & 2
		printf("Check if dive #%d %d will be really added to deco calc: ", i, get_dive(i)->number);
#endif
		struct dive *pdive = get_dive(i);
		/* again skip dives from different trips */
		if (dive->divetrip && dive->divetrip != pdive->divetrip) {
#if DECO_CALC_DEBUG & 2
			printf("No - other dive trip\n");
#endif
			continue;
		}
		/* Don't add future dives */
		if (pdive->when >= dive->when) {
#if DECO_CALC_DEBUG & 2
			printf("No - future or same dive\n");
#endif
			break;
		}
		/* Don't add the copy of the dive itself */
		if (i == divenr) {
#if DECO_CALC_DEBUG & 2
			printf("No - copy of dive\n");
#endif
			continue;
		}
#if DECO_CALC_DEBUG & 2
		printf("Yes\n");
#endif

		surface_pressure = get_surface_pressure_in_mbar(pdive, true) / 1000.0;
		/* Is it the first dive we add? */
		if (!deco_init) {
#if DECO_CALC_DEBUG & 2
			printf("Init deco\n");
#endif
			clear_deco(ds, surface_pressure);
			deco_init = true;
#if DECO_CALC_DEBUG & 2
			printf("Tissues after init:\n");
			dump_tissues(ds);
#endif
		}
		else {
			surface_time = pdive->when - last_endtime;
			if (surface_time < 0) {
#if DECO_CALC_DEBUG & 2
				printf("Exit because surface intervall is %d\n", surface_time);
#endif
				return surface_time;
			}
			add_segment(ds, surface_pressure, air, surface_time, 0, dive->dc.divemode, prefs.decosac);
#if DECO_CALC_DEBUG & 2
			printf("Tissues after surface intervall of %d:%02u:\n", FRACTION(surface_time, 60));
			dump_tissues(ds);
#endif
		}

		add_dive_to_deco(ds, pdive);

		last_starttime = pdive->when;
		last_endtime = dive_endtime(pdive);
		clear_vpmb_state(ds);
#if DECO_CALC_DEBUG & 2
		printf("Tissues after added dive #%d:\n", pdive->number);
		dump_tissues(ds);
#endif
	}

	surface_pressure = get_surface_pressure_in_mbar(dive, true) / 1000.0;
	/* We don't have had a previous dive at all? */
	if (!deco_init) {
#if DECO_CALC_DEBUG & 2
		printf("Init deco\n");
#endif
		clear_deco(ds, surface_pressure);
#if DECO_CALC_DEBUG & 2
		printf("Tissues after no previous dive, surface time set to 48h:\n");
		dump_tissues(ds);
#endif
	}
	else {
		surface_time = dive->when - last_endtime;
		if (surface_time < 0) {
#if DECO_CALC_DEBUG & 2
			printf("Exit because surface intervall is %d\n", surface_time);
#endif
			return surface_time;
		}
		add_segment(ds, surface_pressure, air, surface_time, 0, dive->dc.divemode, prefs.decosac);
#if DECO_CALC_DEBUG & 2
		printf("Tissues after surface intervall of %d:%02u:\n", FRACTION(surface_time, 60));
		dump_tissues(ds);
#endif
	}

	// I do not dare to remove this call. We don't need the result but it might have side effects. Bummer.
	tissue_tolerance_calc(ds, dive, surface_pressure);
	return surface_time;
}

void update_cylinder_related_info(struct dive *dive)
{
	if (dive != NULL) {
		dive->sac = calculate_sac(dive);
		dive->otu = calculate_otu(dive);
		if (dive->maxcns == 0)
			dive->maxcns = calculate_cns(dive);
	}
}

#define MAX_GAS_STRING 80
#define UTF8_ELLIPSIS "\xE2\x80\xA6"

/* callers needs to free the string */
char *get_dive_gas_string(const struct dive *dive)
{
	int o2, he, o2max;
	char *buffer = malloc(MAX_GAS_STRING);

	if (buffer) {
		get_dive_gas(dive, &o2, &he, &o2max);
		o2 = (o2 + 5) / 10;
		he = (he + 5) / 10;
		o2max = (o2max + 5) / 10;

		if (he)
			if (o2 == o2max)
				snprintf(buffer, MAX_GAS_STRING, "%d/%d", o2, he);
			else
				snprintf(buffer, MAX_GAS_STRING, "%d/%d" UTF8_ELLIPSIS "%d%%", o2, he, o2max);
		else if (o2)
			if (o2 == o2max)
				snprintf(buffer, MAX_GAS_STRING, "%d%%", o2);
			else
				snprintf(buffer, MAX_GAS_STRING, "%d" UTF8_ELLIPSIS "%d%%", o2, o2max);
		else
			strcpy(buffer, translate("gettextFromC", "air"));
	}
	return buffer;
}

/*
 * helper functions for dive_trip handling
 */
#ifdef DEBUG_TRIP
void dump_trip_list(void)
{
	dive_trip_t *trip;
	int i = 0;
	timestamp_t last_time = 0;

	for (i = 0; i < trip_table.nr; ++i) {
		struct tm tm;
		trip = trip_table.trips[i];
		utc_mkdate(trip_date(trip), &tm);
		if (trip_date(trip) < last_time)
			printf("\n\ntrip_table OUT OF ORDER!!!\n\n\n");
		printf("%s trip %d to \"%s\" on %04u-%02u-%02u %02u:%02u:%02u (%d dives - %p)\n",
		       trip->autogen ? "autogen " : "",
		       i + 1, trip->location,
		       tm.tm_year, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec,
		       trip->dives.nr, trip);
		last_time = trip_date(trip);
	}
	printf("-----\n");
}
#endif

/* free resources associated with a trip structure */
void free_trip(dive_trip_t *trip)
{
	if (trip) {
		free(trip->location);
		free(trip->notes);
		free(trip);
	}
}

timestamp_t trip_date(const struct dive_trip *trip)
{
	if (!trip || trip->dives.nr == 0)
		return 0;
	return trip->dives.dives[0]->when;
}

static timestamp_t trip_enddate(const struct dive_trip *trip)
{
	if (!trip || trip->dives.nr == 0)
		return 0;
	return dive_endtime(trip->dives.dives[trip->dives.nr - 1]);
}

/* check if we have a trip right before / after this dive */
bool is_trip_before_after(const struct dive *dive, bool before)
{
	int idx = get_idx_by_uniq_id(dive->id);
	if (before) {
		if (idx > 0 && get_dive(idx - 1)->divetrip)
			return true;
	} else {
		if (idx < dive_table.nr - 1 && get_dive(idx + 1)->divetrip)
			return true;
	}
	return false;
}

struct dive *first_selected_dive()
{
	int idx;
	struct dive *d;

	for_each_dive (idx, d) {
		if (d->selected)
			return d;
	}
	return NULL;
}

struct dive *last_selected_dive()
{
	int idx;
	struct dive *d, *ret = NULL;

	for_each_dive (idx, d) {
		if (d->selected)
			ret = d;
	}
	return ret;
}

/* This function defines the sort ordering of dives. The core
 * and the UI models should use the same sort function, which
 * should be stable. This is not crucial at the moment, as the
 * indices in core and UI are independent, but ultimately we
 * probably want to unify the models.
 * After editing a key used in this sort-function, the order of
 * the dives must be re-astablished.
 * Currently, this does a lexicographic sort on the
 * (start-time, trip-time, id) tuple.
 * trip-time is defined such that dives that do not belong to
 * a trip are sorted *after* dives that do. Thus, in the default
 * chronologically-descending sort order, they are shown *before*.
 * "id" is a stable, strictly increasing unique number, that
 * is handed out when a dive is added to the system.
 * We might also consider sorting by end-time and other criteria,
 * but see the caveat above (editing means rearrangement of the dives).
 */
static int comp_dives(const struct dive *a, const struct dive *b)
{
	if (a->when < b->when)
		return -1;
	if (a->when > b->when)
		return 1;
	if (a->divetrip != b->divetrip) {
		if (!b->divetrip)
			return -1;
		if (!a->divetrip)
			return 1;
		if (trip_date(a->divetrip) < trip_date(b->divetrip))
			return -1;
		if (trip_date(a->divetrip) > trip_date(b->divetrip))
			return 1;
	}
	if (a->id < b->id)
		return -1;
	if (a->id > b->id)
		return 1;
	return 0; /* this should not happen for a != b */
}

/* Trips are compared according to the first dive in the trip. */
static int comp_trips(const struct dive_trip *a, const struct dive_trip *b)
{
	/* This should never happen, nevertheless don't crash on trips
	 * with no (or worse a negative number of) dives. */
	if (a->dives.nr <= 0)
		return b->dives.nr <= 0 ? 0 : -1;
	if (b->dives.nr <= 0)
		return 1;
	return comp_dives(a->dives.dives[0], b->dives.dives[0]);
}

#define MAKE_GROW_TABLE(table_type, item_type, array_name) \
	item_type *grow_##table_type(struct table_type *table)				\
	{										\
		int nr = table->nr, allocated = table->allocated;			\
		item_type *items = table->array_name;					\
											\
		if (nr >= allocated) {							\
			allocated = (nr + 32) * 3 / 2;					\
			items = realloc(items, allocated * sizeof(item_type));		\
			if (!items)							\
				exit(1);						\
			table->array_name = items;					\
			table->allocated = allocated;					\
		}									\
		return items;								\
	}

MAKE_GROW_TABLE(dive_table, struct dive *, dives)
static MAKE_GROW_TABLE(trip_table, struct dive_trip *, trips)

/* get the index where we want to insert an object so that everything stays
 * ordered according to a comparison function() */
#define MAKE_GET_INSERTION_INDEX(table_type, item_type, array_name, fun)		\
	int table_type##_get_insertion_index(struct table_type *table, item_type item)	\
	{										\
		/* we might want to use binary search here */				\
		for (int i = 0; i < table->nr; i++) {					\
			if (fun(item, table->array_name[i]))				\
				return i;						\
		}									\
		return table->nr;							\
	}

MAKE_GET_INSERTION_INDEX(dive_table, struct dive *, dives, dive_less_than)
static MAKE_GET_INSERTION_INDEX(trip_table, struct dive_trip *, trips, trip_less_than)

/* add object at the given index to a table. */
#define MAKE_ADD_TO(table_type, item_type, array_name)					\
	void add_to_##table_type(struct table_type *table, int idx, item_type item)	\
	{										\
		int i;									\
		grow_##table_type(table);						\
		table->nr++;								\
											\
		for (i = idx; i < table->nr; i++) {					\
			item_type tmp = table->array_name[i];				\
			table->array_name[i] = item;					\
			item = tmp;							\
		}									\
	}

static MAKE_ADD_TO(dive_table, struct dive *, dives)
static MAKE_ADD_TO(trip_table, struct dive_trip *, trips)

#define MAKE_REMOVE_FROM(table_type, array_name)				\
	void remove_from_##table_type(struct table_type *table, int idx)	\
	{									\
		int i;								\
		for (i = idx; i < table->nr - 1; i++)				\
			table->array_name[i] = table->array_name[i + 1];	\
		table->array_name[--table->nr] = NULL;				\
	}

static MAKE_REMOVE_FROM(dive_table, dives)
static MAKE_REMOVE_FROM(trip_table, trips)

#define MAKE_GET_IDX(table_type, item_type, array_name)						\
	int get_idx_in_##table_type(const struct table_type *table, const item_type item)	\
	{											\
		for (int i = 0; i < table->nr; ++i) {						\
			if (table->array_name[i] == item)					\
				return i;							\
		}										\
		return -1;									\
	}

static MAKE_GET_IDX(dive_table, struct dive *, dives)
static MAKE_GET_IDX(trip_table, struct dive_trip *, trips)

#define MAKE_SORT(table_type, item_type, array_name, fun)					\
	static int sortfn_##table_type(const void *_a, const void *_b)				\
	{											\
		const item_type a = (const item_type)*(const void **)_a;			\
		const item_type b = (const item_type)*(const void **)_b;			\
		return fun(a, b);								\
	}											\
												\
	void sort_##table_type(struct table_type *table)					\
	{											\
		qsort(table->array_name, table->nr, sizeof(item_type), sortfn_##table_type);	\
	}

MAKE_SORT(dive_table, struct dive *, dives, comp_dives)
MAKE_SORT(trip_table, struct dive_trip *, trips, comp_trips)

static void remove_dive(struct dive_table *table, const struct dive *dive)
{
	int idx = get_idx_in_dive_table(table, dive);
	if (idx >= 0)
		remove_from_dive_table(table, idx);
}

/* remove a dive from the trip it's associated to, but don't delete the
 * trip if this was the last dive in the trip. the caller is responsible
 * for removing the trip, if the trip->dives.nr went to 0.
 */
struct dive_trip *unregister_dive_from_trip(struct dive *dive)
{
	dive_trip_t *trip = dive->divetrip;

	if (!trip)
		return NULL;

	remove_dive(&trip->dives, dive);
	dive->divetrip = NULL;
	return trip;
}

static void delete_trip(dive_trip_t *trip, struct trip_table *trip_table)
{
	unregister_trip(trip, trip_table);
	free_trip(trip);
}

void remove_dive_from_trip(struct dive *dive, struct trip_table *trip_table)
{
	struct dive_trip *trip = unregister_dive_from_trip(dive);
	if (trip && trip->dives.nr == 0)
		delete_trip(trip, trip_table);
}

/* Add dive to a trip. Caller is responsible for removing dive
 * from trip beforehand. */
void add_dive_to_trip(struct dive *dive, dive_trip_t *trip)
{
	int idx;
	if (dive->divetrip == trip)
		return;
	if (dive->divetrip)
		fprintf(stderr, "Warning: adding dive to trip that has trip set\n");
	idx = dive_table_get_insertion_index(&trip->dives, dive);
	add_to_dive_table(&trip->dives, idx, dive);
	dive->divetrip = trip;
}

dive_trip_t *alloc_trip(void)
{
	return calloc(1, sizeof(dive_trip_t));
}

/* insert the trip into the trip table */
void insert_trip(dive_trip_t *dive_trip, struct trip_table *trip_table)
{
	int idx = trip_table_get_insertion_index(trip_table, dive_trip);
	add_to_trip_table(trip_table, idx, dive_trip);
#ifdef DEBUG_TRIP
	dump_trip_list();
#endif
}

dive_trip_t *create_trip_from_dive(struct dive *dive)
{
	dive_trip_t *trip;

	trip = alloc_trip();
	trip->location = copy_string(get_dive_location(dive));

	return trip;
}

dive_trip_t *create_and_hookup_trip_from_dive(struct dive *dive, struct trip_table *trip_table)
{
	dive_trip_t *dive_trip = alloc_trip();

	dive_trip = create_trip_from_dive(dive);

	add_dive_to_trip(dive, dive_trip);
	insert_trip(dive_trip, trip_table);
	return dive_trip;
}

/* remove trip from the trip-list, but don't free its memory.
 * caller takes ownership of the trip. */
void unregister_trip(dive_trip_t *trip, struct trip_table *trip_table)
{
	int idx = get_idx_in_trip_table(trip_table, trip);
	assert(!trip->dives.nr);
	if (idx >= 0)
		remove_from_trip_table(trip_table, idx);
}

/*
 * Find a trip a new dive should be autogrouped with. If no such trips
 * exist, allocate a new trip. The bool "*allocated" is set to true
 * if a new trip was allocated.
 */
dive_trip_t *get_trip_for_new_dive(struct dive *new_dive, bool *allocated)
{
	struct dive *d;
	dive_trip_t *trip;
	int i;

	/* Find dive that is within TRIP_THRESHOLD of current dive */
	for_each_dive(i, d) {
		/* Check if we're past the range of possible dives */
		if (d->when >= new_dive->when + TRIP_THRESHOLD)
			break;

		if (d->when + TRIP_THRESHOLD >= new_dive->when && d->divetrip) {
			/* Found a dive with trip in the range */
			*allocated = false;
			return d->divetrip;
		}
	}

	/* Didn't find a trip -> allocate a new one */
	trip = create_trip_from_dive(new_dive);
	trip->autogen = true;
	*allocated = true;
	return trip;
}

/*
 * Collect dives for auto-grouping. Pass in first dive which should be checked.
 * Returns range of dives that should be autogrouped and trip it should be
 * associated to. If the returned trip was newly allocated, the last bool
 * is set to true. Caller still has to register it in the system. Note
 * whereas this looks complicated - it is needed by the undo-system, which
 * manually injects the new trips. If there are no dives to be autogrouped,
 * return NULL.
 */
dive_trip_t *get_dives_to_autogroup(struct dive_table *table, int start, int *from, int *to, bool *allocated)
{
	int i;
	struct dive *lastdive = NULL;

	/* Find first dive that should be merged and remember any previous
	 * dive that could be merged into.
	 */
	for (i = start; i < table->nr; i++) {
		struct dive *dive = table->dives[i];
		dive_trip_t *trip;

		if (dive->divetrip) {
			lastdive = dive;
			continue;
		}

		/* Only consider dives that have not been explicitly removed from
		 * a dive trip by the user.  */
		if (dive->notrip) {
			lastdive = NULL;
			continue;
		}

		/* We found a dive, let's see if we have to allocate a new trip */
		if (!lastdive || dive->when >= lastdive->when + TRIP_THRESHOLD) {
			/* allocate new trip */
			trip = create_trip_from_dive(dive);
			trip->autogen = true;
			*allocated = true;
		} else {
			/* use trip of previous dive */
			trip = lastdive->divetrip;
			*allocated = false;
		}

		// Now, find all dives that will be added to this trip
		lastdive = dive;
		*from = i;
		for (*to = *from + 1; *to < table->nr; (*to)++) {
			dive = table->dives[*to];
			if (dive->divetrip || dive->notrip ||
			    dive->when >= lastdive->when + TRIP_THRESHOLD)
				break;
			if (get_dive_location(dive) && !trip->location)
				trip->location = copy_string(get_dive_location(dive));
			lastdive = dive;
		}
		return trip;
	}

	/* Did not find anyhting - mark as end */
	return NULL;
}

/*
 * Walk the dives from the oldest dive in the given table, and see if we
 * can autogroup them. But only do this when the user selected autogrouping.
 */
static void autogroup_dives(struct dive_table *table, struct trip_table *trip_table)
{
	int from, to;
	dive_trip_t *trip;
	int i, j;
	bool alloc;

	if (!autogroup)
		return;

	for (i = 0; (trip = get_dives_to_autogroup(table, i, &from, &to, &alloc)) != NULL; i = to) {
		for (j = from; j < to; ++j)
			add_dive_to_trip(table->dives[j], trip);
		/* If this was newly allocated, add trip to list */
		if (alloc)
			insert_trip(trip, trip_table);
	}
	sort_trip_table(trip_table);
#ifdef DEBUG_TRIP
	dump_trip_list();
#endif
}

/* Remove a dive from a dive table. This assumes that the
 * dive was already removed from any trip and deselected.
 * It simply shrinks the table and frees the trip */
void delete_dive_from_table(struct dive_table *table, int idx)
{
	free_dive(table->dives[idx]);
	remove_from_dive_table(table, idx);
}

/* This removes a dive from the global dive table but doesn't free the
 * resources associated with the dive. The caller must removed the dive
 * from the trip-list. Returns a pointer to the unregistered dive.
 * The unregistered dive has the selection- and hidden-flags cleared. */
struct dive *unregister_dive(int idx)
{
	struct dive *dive = get_dive(idx);
	if (!dive)
		return NULL; /* this should never happen */
	remove_from_dive_table(&dive_table, idx);
	if (dive->selected)
		amount_selected--;
	dive->selected = false;
	return dive;
}

/* this implements the mechanics of removing the dive from the global
 * dive table and the trip, but doesn't deal with updating dive trips, etc */
void delete_single_dive(int idx)
{
	struct dive *dive = get_dive(idx);
	if (!dive)
		return; /* this should never happen */
	if (dive->selected)
		deselect_dive(dive);
	remove_dive_from_trip(dive, &trip_table);
	delete_dive_from_table(&dive_table, idx);
}

/* add a dive at the given index in the global dive table and keep track
 * of the number of selected dives. if the index is negative, the dive will
 * be added according to dive_less_than() order */
void add_single_dive(int idx, struct dive *dive)
{
	add_to_dive_table(&dive_table, idx, dive);
	if (dive->selected)
		amount_selected++;
}

bool consecutive_selected()
{
	struct dive *d;
	int i;
	bool consecutive = true;
	bool firstfound = false;
	bool lastfound = false;

	if (amount_selected == 0 || amount_selected == 1)
		return true;

	for_each_dive(i, d) {
		if (d->selected) {
			if (!firstfound)
				firstfound = true;
			else if (lastfound)
				consecutive = false;
		} else if (firstfound) {
			lastfound = true;
		}
	}
	return consecutive;
}

void select_dive(struct dive *dive)
{
	if (!dive)
		return;
	if (!dive->selected) {
		dive->selected = 1;
		amount_selected++;
	}
	current_dive = dive;
}

void deselect_dive(struct dive *dive)
{
	int idx;
	if (dive && dive->selected) {
		dive->selected = 0;
		if (amount_selected)
			amount_selected--;
		if (current_dive == dive && amount_selected > 0) {
			/* pick a different dive as selected */
			int selected_dive = idx = get_divenr(dive);
			while (--selected_dive >= 0) {
				dive = get_dive(selected_dive);
				if (dive && dive->selected) {
					current_dive = dive;
					return;
				}
			}
			selected_dive = idx;
			while (++selected_dive < dive_table.nr) {
				dive = get_dive(selected_dive);
				if (dive && dive->selected) {
					current_dive = dive;
					return;
				}
			}
		}
		current_dive = NULL;
	}
}

void deselect_dives_in_trip(struct dive_trip *trip)
{
	if (!trip)
		return;
	for (int i = 0; i < trip->dives.nr; ++i)
		deselect_dive(trip->dives.dives[i]);
}

void select_dives_in_trip(struct dive_trip *trip)
{
	struct dive *dive;
	if (!trip)
		return;
	for (int i = 0; i < trip->dives.nr; ++i) {
		dive = trip->dives.dives[i];
		if (!dive->hidden_by_filter)
			select_dive(dive);
	}
}

void filter_dive(struct dive *d, bool shown)
{
	if (!d)
		return;
	d->hidden_by_filter = !shown;
	if (!shown && d->selected)
		deselect_dive(d);
}


/* Out of two strings, copy the string that is not empty (if any). */
static char *copy_non_empty_string(const char *a, const char *b)
{
	return copy_string(empty_string(b) ? a : b);
}

/* This combines the information of two trips, generating a
 * new trip. To support undo, we have to preserve the old trips. */
dive_trip_t *combine_trips(struct dive_trip *trip_a, struct dive_trip *trip_b)
{
	dive_trip_t *trip;

	trip = alloc_trip();
	trip->location = copy_non_empty_string(trip_a->location, trip_b->location);
	trip->notes = copy_non_empty_string(trip_a->notes, trip_b->notes);

	return trip;
}

void mark_divelist_changed(bool changed)
{
	if (dive_list_changed == changed)
		return;
	dive_list_changed = changed;
	updateWindowTitle();
}

int unsaved_changes()
{
	return dive_list_changed;
}

void process_loaded_dives()
{
	int i;
	struct dive *dive;

	/* Register dive computer nick names */
	for_each_dive(i, dive)
		set_dc_nickname(dive);

	sort_dive_table(&dive_table);
	sort_trip_table(&trip_table);

	/* Autogroup dives if desired by user. */
	autogroup_dives(&dive_table, &trip_table);
}

/*
 * Merge subsequent dives in a table, if mergeable. This assumes
 * that the dives are neither selected, not part of a trip, as
 * is the case of freshly imported dives.
 */
static void merge_imported_dives(struct dive_table *table)
{
	int i;
	for (i = 1; i < table->nr; i++) {
		struct dive *prev = table->dives[i - 1];
		struct dive *dive = table->dives[i];
		struct dive *merged;

		/* only try to merge overlapping dives - or if one of the dives has
		 * zero duration (that might be a gps marker from the webservice) */
		if (prev->duration.seconds && dive->duration.seconds &&
		    dive_endtime(prev) < dive->when)
			continue;

		merged = try_to_merge(prev, dive, false);
		if (!merged)
			continue;

		/* Overwrite the first of the two dives and remove the second */
		free_dive(prev);
		table->dives[i - 1] = merged;
		delete_dive_from_table(table, i);

		/* Redo the new 'i'th dive */
		i--;
	}
}

static void insert_dive(struct dive_table *table, struct dive *d)
{
	int idx = dive_table_get_insertion_index(table, d);
	add_to_dive_table(table, idx, d);
}

/*
 * Clear a dive_table and a trip_table. Think about generating these with macros.
 */
void clear_table(struct dive_table *table)
{
	for (int i = 0; i < table->nr; i++)
		free_dive(table->dives[i]);
	table->nr = 0;
}

static void clear_trip_table(struct trip_table *table)
{
	for (int i = 0; i < table->nr; i++)
		free_trip(table->trips[i]);
	table->nr = 0;
}

/*
 * Try to merge a new dive into the dive at position idx. Return
 * true on success. On success, the old dive will be added to the
 * dives_to_remove table and the merged dive to the dives_to_add
 * table. On failure everything stays unchanged.
 * If "prefer_imported" is true, use data of the new dive.
 */
static bool try_to_merge_into(struct dive *dive_to_add, int idx, struct dive_table *table, bool prefer_imported,
			      /* output parameters: */
			      struct dive_table *dives_to_add, struct dive_table *dives_to_remove)
{
	struct dive *old_dive = table->dives[idx];
	struct dive *merged = try_to_merge(old_dive, dive_to_add, prefer_imported);
	if (!merged)
		return false;

	merged->divetrip = old_dive->divetrip;
	insert_dive(dives_to_remove, old_dive);
	insert_dive(dives_to_add, merged);

	return true;
}

/* Check if two trips overlap time-wise. */
static bool trips_overlap(const struct dive_trip *t1, const struct dive_trip *t2)
{
	/* First, handle the empty-trip cases. */
	if (t1->dives.nr == 0 || t2->dives.nr == 0)
		return 0;

	if (trip_date(t1) < trip_date(t2))
		return trip_enddate(t1) >= trip_date(t2);
	else
		return trip_enddate(t2) >= trip_date(t1);
}

/* Check if a dive is ranked after the last dive of the global dive list */
static bool dive_is_after_last(struct dive *d)
{
	if (dive_table.nr == 0)
		return true;
	return dive_less_than(dive_table.dives[dive_table.nr - 1], d);
}

/* Merge dives from "dives_from" into "dives_to". Overlapping dives will be merged,
 * non-overlapping dives will be moved. The results will be added to the "dives_to_add"
 * table. Dives that were merged are added to the "dives_to_remove" table.
 * Any newly added (not merged) dive will be assigned to the trip of the "trip"
 * paremeter. If "delete_from" is non-null dives will be removed from this table.
 * This function supposes that all input tables are sorted.
 * Returns true if any dive was added (not merged) that is not past the
 * last dive of the global dive list (i.e. the sequence will change).
 * The integer pointed to by "num_merged" will be increased for every
 * merged dive that is added to "dives_to_add" */
static bool merge_dive_tables(struct dive_table *dives_from, struct dive_table *delete_from,
			      struct dive_table *dives_to,
			      bool prefer_imported, struct dive_trip *trip,
			      /* output parameters: */
			      struct dive_table *dives_to_add, struct dive_table *dives_to_remove,
			      int *num_merged)
{
	int i, j;
	int last_merged_into = -1;
	bool sequence_changed = false;

	/* Merge newly imported dives into the dive table.
	 * Since both lists (old and new) are sorted, we can step
	 * through them concurrently and locate the insertions points.
	 * Once found, check if the new dive can be merged in the
	 * previous or next dive.
	 * Note that this doesn't consider pathological cases such as:
	 *  - New dive "connects" two old dives (turn three into one).
	 *  - New dive can not be merged into adjacent but some further dive.
	 */
	j = 0; /* Index in dives_to */
	for (i = 0; i < dives_from->nr; i++) {
		struct dive *dive_to_add = dives_from->dives[i];

		if (delete_from)
			remove_dive(delete_from, dive_to_add);

		/* Find insertion point. */
		while (j < dives_to->nr && dive_less_than(dives_to->dives[j], dive_to_add))
			j++;

		/* Try to merge into previous dive.
		 * We are extra-careful to not merge into the same dive twice, as that
		 * would put the merged-into dive twice onto the dives-to-delete list.
		 * In principle that shouldn't happen as all dives that compare equal
		 * by is_same_dive() were already merged, and is_same_dive() should be
		 * transitive. But let's just go *completely* sure for the odd corner-case. */
		if (j > 0 && j - 1 > last_merged_into &&
		    dive_endtime(dives_to->dives[j - 1]) > dive_to_add->when) {
			if (try_to_merge_into(dive_to_add, j - 1, dives_to, prefer_imported,
					      dives_to_add, dives_to_remove)) {
				free_dive(dive_to_add);
				last_merged_into = j - 1;
				(*num_merged)++;
				continue;
			}
		}

		/* That didn't merge into the previous dive.
		 * Try to merge into next dive. */
		if (j < dives_to->nr && j > last_merged_into &&
		    dive_endtime(dive_to_add) > dives_to->dives[j]->when) {
			if (try_to_merge_into(dive_to_add, j, dives_to, prefer_imported,
					      dives_to_add, dives_to_remove)) {
				free_dive(dive_to_add);
				last_merged_into = j;
				(*num_merged)++;
				continue;
			}
		}

		/* We couldnt merge dives, simply add to list of dives to-be-added. */
		insert_dive(dives_to_add, dive_to_add);
		sequence_changed |= !dive_is_after_last(dive_to_add);
		dive_to_add->divetrip = trip;
	}

	/* we took care of all dives, clean up the import table */
	dives_from->nr = 0;

	return sequence_changed;
}

/* Merge the dives of the trip "from" and the dive_table "dives_from" into the trip "to"
 * and dive_table "dives_to". If "prefer_imported" is true, dive data of "from" takes
 * precedence */
void add_imported_dives(struct dive_table *import_table, struct trip_table *import_trip_table, int flags)
{
	int i, idx;
	struct dive_table dives_to_add = { 0 };
	struct dive_table dives_to_remove = { 0 };
	struct trip_table trips_to_add = { 0 };

	/* Process imported dives and generate lists of dives
	 * to-be-added and to-be-removed */
	process_imported_dives(import_table, import_trip_table, flags,
			       &dives_to_add, &dives_to_remove, &trips_to_add);

	/* Add new dives to trip, so that trips don't get deleted
	 * on deletion of old dives */
	for (i = 0; i < dives_to_add.nr; i++) {
		struct dive *d = dives_to_add.dives[i];
		struct dive_trip *trip = d->divetrip;
		if (!trip)
			continue;
		d->divetrip = NULL;
		add_dive_to_trip(d, trip);
	}

	/* Remove old dives */
	for (i = 0; i < dives_to_remove.nr; i++) {
		idx = get_divenr(dives_to_remove.dives[i]);
		delete_single_dive(idx);
	}
	dives_to_remove.nr = 0;

	/* Add new dives */
	for (i = 0; i < dives_to_add.nr; i++) {
		idx = dive_table_get_insertion_index(&dive_table, dives_to_add.dives[i]);
		add_single_dive(idx, dives_to_add.dives[i]);
	}
	dives_to_add.nr = 0;

	/* Add new trips */
	for (i = 0; i < trips_to_add.nr; i++)
		insert_trip(trips_to_add.trips[i], &trip_table);
	trips_to_add.nr = 0;

	/* We might have deleted the old selected dive.
	 * Choose the newest dive as selected (if any) */
	current_dive = dive_table.nr > 0 ? dive_table.dives[dive_table.nr - 1] : NULL;
	mark_divelist_changed(true);
}

/* Helper function for process_imported_dives():
 * Try to merge a trip into one of the existing trips.
 * The bool pointed to by "sequence_changed" is set to true, if the sequence of
 * the existing dives changes.
 * The int pointed to by "start_renumbering_at" keeps track of the first dive
 * to be renumbered in the dives_to_add table.
 * For other parameters see process_imported_dives()
 * Returns true if trip was merged. In this case, the trip will be
 * freed.
 */
bool try_to_merge_trip(struct dive_trip *trip_import, struct dive_table *import_table, bool prefer_imported,
		       /* output parameters: */
		       struct dive_table *dives_to_add, struct dive_table *dives_to_remove,
		       bool *sequence_changed, int *start_renumbering_at)
{
	int i;
	struct dive_trip *trip_old;

	for (i = 0; i < trip_table.nr; i++) {
		trip_old = trip_table.trips[i];
		if (trips_overlap(trip_import, trip_old)) {
			*sequence_changed |= merge_dive_tables(&trip_import->dives, import_table, &trip_old->dives,
							       prefer_imported, trip_old,
							       dives_to_add, dives_to_remove,
							       start_renumbering_at);
			free_trip(trip_import); /* All dives in trip have been consumed -> free */
			return true;
		}
	}

	return false;
}

/* Process imported dives: take a table of dives to be imported and
 * generate three lists:
 *	1) Dives to be added
 *	2) Dives to be removed
 *	3) Trips to be added
 * The dives to be added are owning (i.e. the caller is responsible
 * for freeing them).
 * The dives and trips in "import_table" and "import_trip_table" are
 * consumed. On return, both tables have size 0.
 * "import_trip_table" may be NULL if all dives are not associated
 * with a trip.
 * The output parameters should be empty - if not, their content
 * will be cleared!
 *
 * Note: The new dives will have their divetrip-field set, but will
 * *not* be part of the trip. The caller has to add them to the trip.
 *
 * The lists are generated by merging dives if possible. This is
 * performed trip-wise. Finer control on merging is provided by
 * the "flags" parameter:
 * - If IMPORT_PREFER_IMPORTED is set, data of the new dives are
 *   prioritized on merging.
 * - If IMPORT_MERGE_ALL_TRIPS is set, all overlapping trips will
 *   be merged, not only non-autogenerated trips.
 * - If IMPORT_IS_DOWNLOADED is true, only the divecomputer of the
 *   first dive will be considered, as it is assumed that all dives
 *   come from the same computer.
 * - If IMPORT_ADD_TO_NEW_TRIP is true, dives that are not assigned
 *   to a trip will be added to a newly generated trip.
 */
void process_imported_dives(struct dive_table *import_table, struct trip_table *import_trip_table, int flags,
			    /* output parameters: */
			    struct dive_table *dives_to_add, struct dive_table *dives_to_remove,
			    struct trip_table *trips_to_add)
{
	int i, nr, start_renumbering_at = 0;
	struct dive_trip *trip_import, *new_trip;
	int preexisting;
	bool sequence_changed = false;
	bool new_dive_has_number = false;

	/* If the caller didn't pass an import_trip_table because all
	 * dives are tripless, provide a local table. This may be
	 * necessary if the trips are autogrouped */
	struct trip_table local_trip_table = { 0 };
	if (!import_trip_table)
		import_trip_table = &local_trip_table;

	/* Make sure that output parameters don't contain garbage */
	clear_table(dives_to_add);
	clear_table(dives_to_remove);
	clear_trip_table(trips_to_add);

	/* Check if any of the new dives has a number. This will be
	 * important later to decide if we want to renumber the added
	 * dives */
	for (int i = 0; i < import_table->nr; i++) {
		if (import_table->dives[i]->number > 0) {
			new_dive_has_number = true;
			break;
		}
	}

	/* If no dives were imported, don't bother doing anything */
	if (!import_table->nr)
		return;

	/* check if we need a nickname for the divecomputer for newly downloaded dives;
	 * since we know they all came from the same divecomputer we just check for the
	 * first one */
	if (flags & IMPORT_IS_DOWNLOADED)
		set_dc_nickname(import_table->dives[0]);
	else
		/* they aren't downloaded, so record / check all new ones */
		for (i = 0; i < import_table->nr; i++)
			set_dc_nickname(import_table->dives[i]);

	/* Sort the table of dives to be imported and combine mergable dives */
	sort_dive_table(import_table);
	merge_imported_dives(import_table);

	/* Autogroup tripless dives if desired by user. But don't autogroup
	 * if tripless dives should be added to a new trip. */
	if (!(flags & IMPORT_ADD_TO_NEW_TRIP))
		autogroup_dives(import_table, import_trip_table);

	preexisting = dive_table.nr; /* Remember old size for renumbering */

	/* Merge overlapping trips. Since both trip tables are sorted, we
	 * could be smarter here, but realistically not a whole lot of trips
	 * will be imported so do a simple n*m loop until someone complains.
	 */
	for (i = 0; i < import_trip_table->nr; i++) {
		trip_import = import_trip_table->trips[i];
		if ((flags & IMPORT_MERGE_ALL_TRIPS) || trip_import->autogen) {
			if (try_to_merge_trip(trip_import, import_table, flags & IMPORT_PREFER_IMPORTED, dives_to_add, dives_to_remove,
					      &sequence_changed, &start_renumbering_at))
				continue;
		}

		/* If no trip to merge-into was found, add trip as-is.
		 * First, add dives to list of dives to add */
		for (i = 0; i < trip_import->dives.nr; i++) {
			struct dive *d = trip_import->dives.dives[i];

			/* Add dive to list of dives to-be-added. */
			insert_dive(dives_to_add, d);
			sequence_changed |= !dive_is_after_last(d);

			remove_dive(import_table, d);
		}

		/* Then, add trip to list of trips to add */
		insert_trip(trip_import, trips_to_add);
		trip_import->dives.nr = 0; /* Caller is responsible for adding dives to trip */
	}
	import_trip_table->nr = 0; /* All trips were consumed */

	if ((flags & IMPORT_ADD_TO_NEW_TRIP) && import_table->nr > 0) {
		/* Create a new trip for unassigned dives, if desired. */
		new_trip = create_trip_from_dive(import_table->dives[0]);
		insert_trip(new_trip, trips_to_add);

		/* Add all remaining dives to this trip */
		for (i = 0; i < import_table->nr; i++) {
			struct dive *d = import_table->dives[i];
			d->divetrip = new_trip;
			insert_dive(dives_to_add, d);
			sequence_changed |= !dive_is_after_last(d);
		}

		import_table->nr = 0; /* All dives were consumed */
	} else if (import_table->nr > 0) {
		/* The remaining dives in import_table are those that don't belong to
		 * a trip and the caller does not want them to be associated to a
		 * new trip. Merge them into the global table. */
		sequence_changed |= merge_dive_tables(import_table, NULL, &dive_table, flags & IMPORT_PREFER_IMPORTED, NULL,
						      dives_to_add, dives_to_remove, &start_renumbering_at);
	}

	/* If new dives were only added at the end, renumber the added dives.
	 * But only if
	 *	- The last dive in the old dive table had a number itself.
	 *	- None of the new dives has a number.
	 */
	nr = dive_table.nr > 0 ? dive_table.dives[dive_table.nr - 1]->number : 0;
	/* We counted the number of merged dives that were added to dives_to_add.
	 * Skip those. Since sequence_changed is false all added dives are *after*
	 * all merged dives. */
	if (!sequence_changed && nr >= preexisting && !new_dive_has_number) {
		for (i = start_renumbering_at; i < dives_to_add->nr; i++)
			dives_to_add->dives[i]->number = ++nr;
	}
}

/* return the number a dive gets when inserted at the given index.
 * this function is supposed to be called *before* a dive was added.
 * this returns:
 * 	- 1 for an empty log
 * 	- last_nr+1 for addition at end of log (if last dive had a number)
 * 	- 0 for all other cases
 */
int get_dive_nr_at_idx(int idx)
{
	if (dive_table.nr == 0)
		return 1;
	if (idx >= dive_table.nr) {
		struct dive *last_dive = get_dive(dive_table.nr - 1);
		return last_dive->number ? last_dive->number + 1 : 0;
	}
	return 0;
}

void set_dive_nr_for_current_dive()
{
	int selected_dive = get_divenr(current_dive);
	if (dive_table.nr == 1)
		current_dive->number = 1;
	else if (selected_dive == dive_table.nr - 1 && get_dive(dive_table.nr - 2)->number)
		current_dive->number = get_dive(dive_table.nr - 2)->number + 1;
}

static int min_datafile_version;

int get_min_datafile_version()
{
	return min_datafile_version;
}

void reset_min_datafile_version()
{
	min_datafile_version = 0;
}

void report_datafile_version(int version)
{
	if (min_datafile_version == 0 || min_datafile_version > version)
		min_datafile_version = version;
}

int get_dive_id_closest_to(timestamp_t when)
{
	int i;
	int nr = dive_table.nr;

	// deal with pathological cases
	if (nr == 0)
		return 0;
	else if (nr == 1)
		return dive_table.dives[0]->id;

	for (i = 0; i < nr && dive_table.dives[i]->when <= when; i++)
		; // nothing

	// again, capture the two edge cases first
	if (i == nr)
		return dive_table.dives[i - 1]->id;
	else if (i == 0)
		return dive_table.dives[0]->id;

	if (when - dive_table.dives[i - 1]->when < dive_table.dives[i]->when - when)
		return dive_table.dives[i - 1]->id;
	else
		return dive_table.dives[i]->id;
}

void clear_dive_file_data()
{
	while (dive_table.nr)
		delete_single_dive(0);
	while (dive_site_table.nr)
		delete_dive_site(get_dive_site(0));
	if (trip_table.nr != 0) {
		fprintf(stderr, "Warning: trip table not empty in clear_dive_file_data()!\n");
		trip_table.nr = 0;
	}

	clear_dive(&displayed_dive);

	reset_min_datafile_version();
	saved_git_id = "";
}

bool dive_less_than(const struct dive *a, const struct dive *b)
{
	return comp_dives(a, b) < 0;
}

bool trip_less_than(const struct dive_trip *a, const struct dive_trip *b)
{
	return comp_trips(a, b) < 0;
}

/* When comparing a dive to a trip, use the first dive of the trip. */
static int comp_dive_to_trip(struct dive *a, struct dive_trip *b)
{
	/* This should never happen, nevertheless don't crash on trips
	 * with no (or worse a negative number of) dives. */
	if (b->dives.nr <= 0)
		return -1;
	return comp_dives(a, b->dives.dives[0]);
}

static int comp_dive_or_trip(struct dive_or_trip a, struct dive_or_trip b)
{
	if (a.dive && b.dive)
		return comp_dives(a.dive, b.dive);
	if (a.trip && b.trip)
		return comp_trips(a.trip, b.trip);
	if (a.dive)
		return comp_dive_to_trip(a.dive, b.trip);
	else
		return -comp_dive_to_trip(b.dive, a.trip);
}

bool dive_or_trip_less_than(struct dive_or_trip a, struct dive_or_trip b)
{
	return comp_dive_or_trip(a, b) < 0;
}

/*
 * Calculate surface interval for dive starting at "when". Currently, we
 * might display dives which are not yet in the divelist, therefore the
 * input parameter is a timestamp.
 * If the given dive starts during a different dive, the surface interval
 * is 0. If we can't determine a surface interval (first dive), <0 is
 * returned. This does *not* consider pathological cases such as dives
 * that happened inside other dives. The interval will always be calculated
 * with respect to the dive that started previously.
 */
timestamp_t get_surface_interval(timestamp_t when)
{
	int i;
	timestamp_t prev_end;

	/* find previous dive. might want to use a binary search. */
	for (i = dive_table.nr - 1; i >= 0; --i) {
		if (dive_table.dives[i]->when < when)
			break;
	}
	if (i < 0)
		return -1;

	prev_end = dive_endtime(dive_table.dives[i]);
	if (prev_end > when)
		return 0;
	return when - prev_end;
}

/* Find visible dive close to given date. First search towards older,
 * then newer dives. */
struct dive *find_next_visible_dive(timestamp_t when)
{
	int i, j;

	if (!dive_table.nr)
		return NULL;

	/* we might want to use binary search here */
	for (i = 0; i < dive_table.nr; i++) {
		if (when <= get_dive(i)->when)
			break;
	}

	for (j = i - 1; j > 0; j--) {
		if (!get_dive(j)->hidden_by_filter)
			return get_dive(j);
	}

	for (j = i; j < dive_table.nr; j++) {
		if (!get_dive(j)->hidden_by_filter)
			return get_dive(j);
	}

	return NULL;
}

static bool is_same_day(timestamp_t trip_when, timestamp_t dive_when)
{
	static timestamp_t twhen = (timestamp_t) 0;
	static struct tm tmt;
	struct tm tmd;

	utc_mkdate(dive_when, &tmd);

	if (twhen != trip_when) {
		twhen = trip_when;
		utc_mkdate(twhen, &tmt);
	}

	return (tmd.tm_mday == tmt.tm_mday) && (tmd.tm_mon == tmt.tm_mon) && (tmd.tm_year == tmt.tm_year);
}

bool trip_is_single_day(const struct dive_trip *trip)
{
	if (trip->dives.nr <= 1)
		return true;
	return is_same_day(trip->dives.dives[0]->when,
			   trip->dives.dives[trip->dives.nr - 1]->when);
}

int trip_shown_dives(const struct dive_trip *trip)
{
	int res = 0;
	for (int i = 0; i < trip->dives.nr; ++i) {
		if (!trip->dives.dives[i]->hidden_by_filter)
			res++;
	}
	return res;
}
