/* Replays reference engine inputs through sc-timing.c and compares every decision. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "sc-timing.h"

static int run(const char *path)
{
	FILE *f = fopen(path, "r");
	if (!f) {
		printf("cannot open %s\n", path);
		return 1;
	}
	struct sct_engine e;
	sct_init(&e, 4);
	e.fill_only = getenv("SCT_FILL_ONLY") != NULL;
	long long t, a, b;
	int nw, conf, cut, kind, n = 0, bad = 0, interp = 0;
	double gx, gy, al;
	while (fscanf(f, "%lld,%d,%lf,%lf,%d,%d,%d,%lld,%lld,%lf", &t, &nw, &gx, &gy, &conf, &cut, &kind, &a, &b, &al) ==
	       10) {
		sct_push(&e, t, nw != 0, gx, gy, conf != 0, cut != 0);
		struct sct_decision d = {0};
		if (t - 4 >= 0)
			d = sct_decide(&e, t - 4, t);
		bool same = (int)d.kind == kind && (kind == 0 || d.a == a) &&
			    (kind != 2 || (d.b == b && fabs(d.alpha - al) < 1e-9));
		if (!same) {
			if (bad < 5)
				printf("  tick %lld: C (%d,%lld,%lld,%.6f) vs ref (%d,%lld,%lld,%.6f)\n", t, d.kind,
				       (long long)d.a, (long long)d.b, d.alpha, kind, a, b, al);
			bad++;
		}
		interp += kind == 2;
		n++;
	}
	fclose(f);
	printf("%-28s %4d ticks, %3d interpolated, %s\n", path, n, interp, bad ? "MISMATCH" : "identical");
	return bad != 0;
}

int main(int argc, char **argv)
{
	int fails = 0;
	for (int i = 1; i < argc; i++)
		fails += run(argv[i]);
	printf("%s\n", fails ? "FAILED" : "ALL IDENTICAL");
	return fails;
}
