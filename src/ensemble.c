#include "system.h"
#include "sccs.h"
#include "logging.h"
#include "ensemble.h"
#include "range.h"

typedef struct {
	char	*rootkey;		/* rootkey of the repo */
	char	*deltakey;		/* deltakey of repo as of rev */
	char	*path;			/* path to component or null */
	u32	new:1;			/* if set, the undo will remove this */
	u32	present:1;		/* if set, the repo is actually here */
} repo;

private int	repo_sort(const void *a, const void *b);
private	void	setgca(sccs *s, u32 bit, u32 tmp);

private	char	*ensemble_version = "1.0";

/*
 * Return the list of repos for this product.
 * Optionally include the product.
 *
 * Callers of this are undo, [r]clone, pull, push and they tend to want
 * different things:
 *
 * undo wants a list of components, which is the subset that will change in
 * the undo operation, and delta keys which are the desired tips after undo.
 * We get the list of repos from the range of csets passed in in revs
 * and then we do another pass to get the tips from "rev".
 *
 * [r]clone wants a list of all components in a particular rev and their
 * tip keys.  It passes in the rev and not the list.
 *
 * pull/push want a list components, which is the subset that changed in
 * the range of csets being moved in the product, and delta keys which are
 * the tips in each component.  "rev" is not passed in, "revs" is the list.
 * The first tip we see for each component is what we want, that's newest.
 *
 * The path field is always set to the current path of the component, not
 * the path as of the rev[s].  This is the final pass through the ChangeSet
 * file.
 *
 * The present field is set if the component is in the filesystem.
 *
 */
repos *
ensemble_list(eopts opts)
{
	repo	*e = 0;
	repos	*r = 0;
	char	**list = 0;	/* really repos **list */
	delta	*d, *top;
	char	*t;
	int	had_top = 0, close = 0;
	int	i;
	MDBM	*idDB = 0;
	kvpair	kv;
	char	buf[MAXKEY];

	assert (!opts.rev ^ !opts.revs); /* logical xor: one or the other, not both */
	unless (proj_isProduct(0)) {
		fprintf(stderr, "ensemble_list called in a non-product.\n");
		return (0);
	}
	unless (opts.rev || opts.revs) {
		fprintf(stderr, "ensemble_list called with no revs?\n");
		return (0);
	}
	unless (opts.sc) {
		concat_path(buf, proj_root(proj_product(0)), CHANGESET);
		opts.sc = sccs_init(buf, INIT_NOCKSUM|INIT_NOSTAT);
		close = 1;
	}
	assert(CSET(opts.sc) && proj_isProduct(opts.sc->proj));

	r = new(repos);
	top = sccs_top(opts.sc);
	if (opts.revs) {
		unless (close) sccs_clearbits(opts.sc, D_SET|D_RED|D_BLUE);
		EACH(opts.revs) {
			unless (d = sccs_findrev(opts.sc, opts.revs[i])) {
				fprintf(stderr,
				    "ensemble: bad rev %s\n",opts.revs[i]);
err:				if (close) sccs_free(opts.sc);
				return (0);
			}
			d->flags |= (D_SET|D_BLUE);
			if (d == top) had_top = 1;
		}
		if (sccs_cat(opts.sc, GET_HASHONLY, 0)) goto err;
		EACH_KV(opts.sc->mdbm) {
			unless (componentKey(kv.val.dptr)) continue;
			e = new(repo);
			e->rootkey  = strdup(kv.key.dptr);
			e->deltakey = strdup(kv.val.dptr);
			e->path     = key2path(e->deltakey, 0);
			e->new	    = 1;	// cleared below if not true
			e->present  = 1;	// ditto
			csetChomp(e->path);
			list = addLine(list, (void*)e);
		}
		/* Mark the set gca of the range we marked in opts.revs */
		setgca(opts.sc, D_SET, D_RED);
		if (sccs_cat(opts.sc, GET_HASHONLY, 0)) goto err;
		EACH(list) {
			e = (repo*)list[i];
			unless (
			    t = mdbm_fetch_str(opts.sc->mdbm, e->rootkey)) {
				continue;
			}
			e->new = 0;
			/*
			 * undo wants the keys as of opts.rev,
			 * which is older.  push wants the key it
			 * already has, the newest.
			 */
			if (opts.undo) {
				assert(!streq(e->deltakey, t));
				free(e->deltakey);
				e->deltakey = strdup(t);
			}
		}
	} else {
		if (sccs_get(opts.sc,
		    opts.rev, 0, 0, 0, SILENT|GET_HASHONLY, 0)) {
			goto err;
		}
		EACH_KV(opts.sc->mdbm) {
			unless (componentKey(kv.val.dptr)) continue;
			e = new(repo);
			e->rootkey  = strdup(kv.key.dptr);
			e->deltakey = strdup(kv.val.dptr);
			e->path     = key2path(e->deltakey, 0);
			e->new	    = 1;	// cleared below if not true
			e->present  = 1;	// ditto
			csetChomp(e->path);
			list = addLine(list, (void*)e);
		}
	}

	/*
	 * Filter the list through the modules requested, if any.
	 */
	if (opts.modules) {
		EACH(list) {
			e = (repo*)list[i];
			unless (hash_fetchStr(opts.modules, e->rootkey)) {
				free(e->path);
				free(e->rootkey);
				free(e->deltakey);
				removeLineN(list, i, free);
				i--;
			}
		}
	}

	/*
	 * Now see if we have the TIP, otherwise we need to overwrite
	 * the path field with whatever path is in the TIP delta
	 * keys.  The TIP of the changeset file contains the current
	 * locations (unless someone did a mv on a repo).
	 */
	if ((opts.revs && !had_top) ||
	    (top != sccs_findrev(opts.sc, opts.rev))) {
		/* Fetch TIP and see if any paths have changed */
		sccs_get(opts.sc, top->rev, 0, 0, 0, SILENT|GET_HASHONLY, 0);
		EACH(list) {
			e = (repo*)list[i];
			unless (
			    t = mdbm_fetch_str(opts.sc->mdbm, e->rootkey)) {
				continue;
			}
			t = key2path(t, 0);
			csetChomp(t);
			if (streq(e->path, t)) {
				free(t);
			} else {
				free(e->path);
				e->path = t;
			}
		}
	}

	/*
	 * Mark entries that are not present
	 */
	idDB = loadDB(IDCACHE, 0, DB_IDCACHE);
	EACH(list) {
		e = (repo*)list[i];
		if (isComponent(e->path)) continue;
		if ((t = mdbm_fetch_str(idDB, e->rootkey)) && isComponent(t)){
			free(e->path);
			e->path = strdup(t);
		} else {
			e->present = 0;
		}
	}
	mdbm_close(idDB);

	sortLines(list, repo_sort);

	if (opts.product) {
		if (opts.rev) {			/* undo / [r]clone/ push */
			d = sccs_findrev(opts.sc, opts.rev);
		} else if (had_top && !opts.undo) {
			d = top;
		} else {			/* push/pull */
			u32	flag;
			/*
			 * Use the latest one, it matches what we do
			 * in the weave. The newest key will be
			 * colored D_BLUE while the oldest key will be
			 * colored D_SET.
			 */
			flag = D_BLUE;
			if (opts.undo) flag = D_SET;
			for (d = opts.sc->table; d; d = d->next) {
				if (!TAG(d) && (d->flags & flag)) break;
			}
		}
		assert(d);
		e = new(repo);
		e->rootkey = strdup(proj_rootkey(opts.sc->proj));
		sccs_sdelta(opts.sc, d, buf);
		e->deltakey = strdup(buf);
		e->path = strdup(".");
		e->present = 1;
		if (opts.product_first) {
			char	**l = 0;

			l = addLine(0, (void*)e);
			EACH(list) l = addLine(l, list[i]);
			freeLines(list, 0);
			list = l;
		} else {
			list = addLine(list, (void*)e);
		}
	}
	if (close) {
		sccs_free(opts.sc);
	} else {
		if (opts.revs) sccs_clearbits(opts.sc, D_SET|D_RED|D_BLUE);
	}
	r->repos = (void**)list;
	return (r);
}

/*
 * Mark the set gca of a range
 * Input: some region, like pull -r, is colored.
 * bit - the color of the set region
 * tmp - the color used as a tmp. Assumes all off and leaves it all off
 */
private void
setgca(sccs *s, u32 bit, u32 tmp)
{
	delta	*d, *p;

	for (d = s->table; d; d = d->next) {
		if (TAG(d)) continue;
		if (d->flags & bit) {
			d->flags &= ~bit;
			if ((p = d->parent) && !(p->flags & bit)) {
				p->flags |= tmp;
			}
			if (d->merge && (p = sfind(s, d->merge)) &&
			    !(p->flags & bit)) {
				p->flags |= tmp;
			}
			continue;
		}
		unless (d->flags & tmp) continue;
		d->flags &= ~tmp;
		d->flags |= bit;
		if (p = d->parent) {
			p->flags |= tmp;
		}
		if (d->merge && (p = sfind(s, d->merge))) {
			p->flags |= tmp;
		}
	}
}

#define	L_ROOT		1
#define	L_DELTA		2
#define	L_PATH		4
#define	L_NEW		8
#define	L_PRESENT	0x10

int
ensemble_list_main(int ac, char **av)
{
	int	c;
	int	want = 0, serialize = 0;
	int	input = 0, output = 0;
	char	*p;
	repos	*r = 0;
	eopts	opts;
	char	**modules = 0;
	char	buf[MAXKEY];

	unless (proj_isProduct(0)) {
		fprintf(stderr,
		    "%s: needs to be called in a Product.\n", av[0]);
		return (1);
	}
	bzero(&opts, sizeof(opts));
	opts.product = 1;

	while ((c = getopt(ac, av, "1il;M;opr;u")) != -1) {
		switch (c) {
		    case '1':
			opts.product_first = 1;
			break;
		    case 'i': input = 1; break;
		    case 'l':
			for (p = optarg; *p; p++) {
				switch (*p) {
				    case 'd': want |= L_DELTA; break;
				    case 'h': want |= L_PRESENT; break;
				    case 'n': want |= L_NEW; break;
				    case 'p': want |= L_PATH; break;
				    case 'r': want |= L_ROOT; break;
			    	}
			}
			break;
		    case 'M':
			modules = addLine(modules, optarg);
			break;
		    case 'o': output = 1; break;
		    case 'p':
			opts.product = 0;
			break;
		    case 'r':
			opts.rev = optarg;
			break;
		    case 's':
			serialize = 1;
			break;
		    case 'u':
			opts.undo = 1;
			break;
		    default:
			system("bk help ensemble_list");
			exit(1);
		}
	}
	if (av[optind] && streq(av[optind], "-")) {
		while (fnext(buf, stdin)) {
			chomp(buf);
			opts.revs = addLine(opts.revs, strdup(buf));
		}
	}
	if (modules) opts.modules = module_list(modules, 0);

	unless (want) want = L_PATH;
	if (input) {
		r = ensemble_fromStream(r, stdin);
	} else {
		unless (r = ensemble_list(opts)) exit(0);
	}
	if (output) {
		ensemble_toStream(r, stdout);
		goto out;
	}
	EACH_REPO(r) {
		if (opts.undo && r->new && !(want & L_NEW)) continue;
		p = "";
		if (want & L_PATH) {
			printf("%s", r->path);
			p = "|";
		}
		if (want & L_DELTA) {
			printf("%s%s", p, r->deltakey);
			p = "|";
		}
		if (want & L_ROOT) {
			printf("%s%s", p, r->rootkey);
			p = "|";
		}
		if ((want & L_NEW) && r->new)  {
			printf("%s(new)", p);
			p = "|";
		}
		if ((want & L_PRESENT) && r->present)  {
			printf("%s(present)", p);
			p = "|";
		}
		printf("\n");
	}
out:	ensemble_free(r);
	if (modules) freeLines(modules, 0);
	if (opts.revs) freeLines(opts.revs, free);
	exit(0);
}

private int
repo_sort(const void *a, const void *b)
{
	repo	*l, *r;

	l = *(repo**)a;
	r = *(repo**)b;
	return (strcmp(l->path, r->path));
}

repos *
ensemble_first(repos *list)
{
	assert(list);
	list->index = -1;
	return (ensemble_next(list));
}

repos *
ensemble_next(repos *list)
{
	repo	*r;

	assert(list);
	if (list->index == -1) list->index = 0;
	list->index++;
	/* This next if is EACH_INDEX() unrolled */
	if (list->repos &&
	    (list->index < LSIZ(list->repos)) &&
	    (r = list->repos[list->index])) {
		list->rootkey = r->rootkey;
		list->deltakey = r->deltakey;
		list->path = r->path;
		list->new = r->new;
		list->present = r->present;
	} else {
		list->index = 0;
		list->new = list->present = 0;
		list->rootkey = list->deltakey = list->path = 0;
	}
	return (list);
}

/* lm3di */
int
ensemble_find(repos *list, char *rootkey)
{
	assert(list);
	EACH_REPO(list) {
		if (streq(list->rootkey, rootkey)) return (1);
	}
	return (0);
}

void
ensemble_free(repos *list)
{
	unless (list) return;
	EACH_REPO(list) {
		/* goofy but it should work */
		free(list->rootkey);
		free(list->deltakey);
		free(list->path);
		free((char*)list->repos[list->index]);
	}
	free(list->repos);
	free(list);
}

int
ensemble_toStream(repos *repos, FILE *f)
{
	fprintf(f, "@ensemble_list %s@\n", ensemble_version);
	EACH_REPO(repos) {
		fprintf(f, "rk:%s\n", repos->rootkey);
		fprintf(f, "dk:%s\n", repos->deltakey);
		fprintf(f, "pt:%s\n", repos->path);
		fprintf(f, "nw:%d\n", repos->new);
		fprintf(f, "pr:%d\n", repos->present);
	}
	fprintf(f, "@ensemble_list end@\n");
	return (0);
}

repos *
ensemble_fromStream(repos *r, FILE *f)
{
	char	*buf;
	char	*rk, *dk, *pt;
	int	nw, pr;
	repo	*e;
	char	**list = 0;

	buf = fgetline(f);
	unless (strneq(buf, "@ensemble_list ", 15)) return (0);
	while (buf = fgetline(f)) {
		if (strneq(buf, "@ensemble_list end@", 19)) break;
		while (!strneq(buf, "rk:", 3)) continue;
		rk = strdup(buf+3);
		buf = fgetline(f);
		assert(strneq(buf, "dk:", 3));
		dk = strdup(buf+3);
		buf = fgetline(f);
		assert(strneq(buf, "pt:", 3));
		pt = strdup(buf+3);
		buf = fgetline(f);
		assert(strneq(buf, "nw:", 3));
		nw = atoi(buf+3);
		buf = fgetline(f);
		assert(strneq(buf, "pr:", 3));
		pr = atoi(buf+3);
		/* now add it */
		e = new(repo);
		e->rootkey = rk;
		e->deltakey = dk;
		e->path = pt;
		e->new = nw;
		e->present = pr;
		list = addLine(list, (void*)e);
	}
	unless (r) r = new(repos);
	r->repos = (void **)list;
	return (r);
}

/*
 * Run a command in each repository of the ensemble, including the
 * product.
 */
int
ensemble_each(int quiet, int ac, char **av)
{
	repos	*list;
	project	*p = proj_product(0);
	int	c;
	int	errors = 0;
	int	status;
	char	**modules = 0;
	eopts	opts;

	unless (p) {
		fprintf(stderr, "Not in an ensemble.\n");
		return (1);
	}
	chdir(proj_root(p));
	bzero(&opts, sizeof(opts));
	opts.product = 1;
	opts.product_first = 1;
	opts.rev = "+";
	getoptReset();
	// has to track bk.c's getopt string
	while ((c = getopt(ac, av, "@|1aAB;cCdDgGhjL|lM;npqr|RuUxz;")) != -1) {
		unless (c == 'M') continue;
		if (optarg[0] == '|') {
			opts.rev = &optarg[1];
		} else if (streq("!.", optarg)) {
			opts.product = 0;
		} else {
			modules = addLine(modules, optarg);
		}
	}
	if (modules) opts.modules = module_list(modules, 0);

	unless (list = ensemble_list(opts)) {
		fprintf(stderr, "No ensemble list?\n");
		return (1);
	}
	EACH_REPO(list) {
		unless (quiet) {
			printf("===== %s =====\n", list->path);
			fflush(stdout);
		}
		chdir(proj_root(p));
		if (chdir(list->path)) continue;
		status = spawnvp(_P_WAIT, "bk", av);
		if (WIFEXITED(status)) {
			errors |= WEXITSTATUS(status);
		} else {
			errors |= 1;
		}
	}
	ensemble_free(list);
	return (errors);
}