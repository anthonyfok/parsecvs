/*
 *  Copyright © 2006 Keith Packard <keithp@keithp.com>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or (at
 *  your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful, but
 *  WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  59 Temple Place, Suite 330, Boston, MA 02111-1307 USA.
 */

#include <limits.h>
#include "cvs.h"

static int mark;

void
export_init(void)
{
    mark = 0;
}

void 
export_blob(Node *node, void *buf, unsigned long len)
{
    node->file->mark = ++mark;

    printf("blob\nmark :%d\ndata %zd\n", 
	   node->file->mark, len);
    fwrite(buf, len, sizeof(char), stdout);
    putchar('\n');
}

static char *
export_filename (rev_file *file, int strip)
{
    static char name[PATH_MAX];
    char    *attic;
    int	    l;
    int	    len;
    
    if (strlen (file->name) - strip >= MAXPATHLEN)
    {
	fprintf(stderr, "parsecvs: file name %s\n too long\n", file->name);
	exit(1);
    }
    strcpy (name, file->name + strip);
    while ((attic = strstr (name, "Attic/")) &&
	   (attic == name || attic[-1] == '/'))
    {
	l = strlen (attic);
	memmove (attic, attic + 6, l - 5);
    }
    len = strlen (name);
    if (len > 2 && !strcmp (name + len - 2, ",v"))
	name[len-2] = '\0';

    if (strcmp(name, ".cvsignore") == 0)
    {
	name[1] = 'g';
	name[2] = 'i';
	name[3] = 't';
    }

    return name;
}

static const char *utc_offset_timestamp(const time_t *timep, const char *tz)
{
    static char outbuf[BUFSIZ];

    struct tm *tm;
    char tzbuf[BUFSIZ];
    /* coverity[tainted_data] */
    char *oldtz = getenv("TZ");

    // make a copy in case original is clobbered
    if (oldtz != NULL)
	strncpy(tzbuf, oldtz, sizeof(tzbuf)-1);

    setenv("TZ", tz, 1);
    tzset();  // just in case ...

    tm = localtime(timep);
    strftime(outbuf, sizeof(outbuf), "%s %z", tm);

    if (oldtz != NULL)
	setenv("TZ", tzbuf, 1);
    else
	unsetenv("TZ");
    tzset();

    return outbuf;
}

static int export_total_commits;
static int export_current_commit;
static char *export_current_head;

#define STATUS	stderr
#define PROGRESS_LEN	20

static void
export_status (void)
{
    int	spot = export_current_commit * PROGRESS_LEN / export_total_commits;
    int	s;

    fprintf (STATUS, "Save: %35.35s ", export_current_head);
    for (s = 0; s < PROGRESS_LEN + 1; s++)
	putc (s == spot ? '*' : '.', STATUS);
    fprintf (STATUS, " %5d of %5d\n", export_current_commit, export_total_commits);
    fflush (STATUS);
}

static void
export_commit(rev_commit *commit, char *branch, int strip)
{
    cvs_author *author;
    char *full;
    char *email;
    char *timezone;
    char *revpairs = NULL;
    size_t revpairsize = 0;
    const char *ts;
    time_t ct;
    rev_file	*f, *f2;
    int		i, j, i2, j2;

    author = fullname(commit->author);
    if (!author) {
	full = commit->author;
	email = commit->author;
	timezone = "UTC";
    } else {
	full = author->full;
	email = author->email;
	timezone = author->timezone ? author->timezone : "UTC";
    }

    printf("commit refs/heads/%s\n", branch);
    printf("mark :%d\n", ++mark);
    commit->mark = mark;
    ct = force_dates ? mark * commit_time_window * 2 : commit->date;
    ts = utc_offset_timestamp(&ct, timezone);
    printf("author %s <%s> %s\n", full, email, ts);
    printf("committer %s <%s> %s\n", full, email, ts);
    printf("data %zd\n%s\n", strlen(commit->log), commit->log);
    if (commit->parent)
	printf("from :%d\n", commit->parent->mark);

    if (reposurgeon)
    {
	revpairs = xmalloc((revpairsize = 1024));
	revpairs[0] = '\0';
    }

    for (i = 0; i < commit->ndirs; i++) {
	rev_dir	*dir = commit->dirs[i];
	
	for (j = 0; j < dir->nfiles; j++) {
	    char *stripped;
	    bool present, changed;
	    f = dir->files[j];
	    stripped = export_filename(f, strip);
	    present = false;
	    changed = false;
	    if (commit->parent) {
		for (i2 = 0; i2 < commit->parent->ndirs; i2++) {
		    rev_dir	*dir2 = commit->parent->dirs[i2];
		    for (j2 = 0; j2 < dir2->nfiles; j2++) {
			f2 = dir2->files[j2];
			if (strcmp(f->name, f2->name) == 0) {
			    present = true;
			    changed = (f->mark != f2->mark);
			}
		    }
		}
	    }
	    if (!present || changed) {
		printf("M 100%o :%d %s\n", 
		       (f->mode & 0777) | 0200, 
		       f->mark, stripped);
		if (revision_map || reposurgeon) {
		    char *fr = stringify_revision(stripped, " ", &f->number);
		    if (revision_map)
			fprintf(revision_map, "%s :%d\n", fr, f->mark);
		    if (reposurgeon)
		    {
			if (strlen(revpairs) + strlen(fr) + 2 > revpairsize)
			{
			    revpairsize += strlen(fr) + 2;
			    revpairs = xrealloc(revpairs, revpairsize);
			}
			strcat(revpairs, fr);
			strcat(revpairs, "\n");
		    }
		}
	    }
	}
    }

    if (commit->parent)
    {
	for (i = 0; i < commit->parent->ndirs; i++) {
	    rev_dir	*dir = commit->parent->dirs[i];

	    for (j = 0; j < dir->nfiles; j++) {
		bool present;
		f = dir->files[j];
		present = false;
		for (i2 = 0; i2 < commit->ndirs; i2++) {
		    rev_dir	*dir2 = commit->dirs[i2];
		    for (j2 = 0; j2 < dir2->nfiles; j2++) {
			f2 = dir2->files[j2];
			if (strcmp(f->name, f2->name) == 0) {
			    present = true;
			}
		    }
		}
		if (!present)
		    printf("D %s\n", export_filename(f, strip));
	    }
	}
    }

    if (reposurgeon) 
    {
	printf("property cvs-revision %zd %s", strlen(revpairs), revpairs);
	free(revpairs);
    }

    printf ("\n");

}

static int
export_commit_recurse (rev_ref *head, rev_commit *commit, int strip)
{
    Tag *t;
    
    if (commit->parent && !commit->tail)
	    if (!export_commit_recurse (head, commit->parent, strip))
		return 0;
    ++export_current_commit;
    export_status ();
    export_commit (commit, head->name, strip);
    for (t = all_tags; t; t = t->next)
	if (t->commit == commit)
	    printf("reset refs/tags/%s\nfrom :%d\n\n", t->name, commit->mark);
    return 1;
}

static int
export_ncommit (rev_list *rl)
{
    rev_ref	*h;
    rev_commit	*c;
    int		n = 0;
    
    for (h = rl->heads; h; h = h->next) {
	if (h->tail)
	    continue;
	for (c = h->commit; c; c = c->parent) {
	    n++;
	    if (c->tail)
		break;
	}
    }
    return n;
}

bool
export_commits (rev_list *rl, int strip)
{
    rev_ref *h;

    export_total_commits = export_ncommit (rl);
    export_current_commit = 0;
    for (h = rl->heads; h; h = h->next) 
    {
	export_current_head = h->name;
	if (!h->tail)
	    if (!export_commit_recurse (h, h->commit, strip))
		return false;
	printf("reset refs/heads/%s\nfrom :%d\n\n", h->name, h->commit->mark);
    }
    fprintf (STATUS, "\n");
    return true;
}

#define PROGRESS_LEN	20

void load_status (char *name)
{
    int	spot = load_current_file * PROGRESS_LEN / load_total_files;
    int	    s;
    int	    l;

    l = strlen (name);
    if (l > 35) name += l - 35;

    fprintf (STATUS, "Load: %35.35s ", name);
    for (s = 0; s < PROGRESS_LEN + 1; s++)
	putc (s == spot ? '*' : '.', STATUS);
    fprintf (STATUS, " %5d of %5d\n", load_current_file, load_total_files);
    fflush (STATUS);
}

void load_status_next (void)
{
    fprintf (STATUS, "\n");
    fflush (STATUS);
}

/* end */
