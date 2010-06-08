/*-
 * Copyright (c) 2006 Verdens Gang AS
 * Copyright (c) 2006-2010 Redpill Linpro AS
 * All rights reserved.
 *
 * Author: Poul-Henning Kamp <phk@phk.freebsd.dk>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include "config.h"

#include "svnid.h"
SVNID("$Id$")

#include <sys/types.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "vas.h"
#include "shmlog.h"
#include "vre.h"
#include "vbm.h"
#include "vqueue.h"
#include "miniobj.h"
#include "varnishapi.h"

#include "vslapi.h"

#ifndef MAP_HASSEMAPHORE
#define MAP_HASSEMAPHORE 0 /* XXX Linux */
#endif

/*--------------------------------------------------------------------*/

struct VSL_data *
VSL_New(void)
{
	struct VSL_data *vd;

	ALLOC_OBJ(vd, VSL_MAGIC);
	AN(vd);

	vd->diag = (vsl_diag_f*)fprintf;
	vd->priv = stderr;

	vd->vsl_fd = -1;

	vd->regflags = 0;

	/* XXX: Allocate only if log access */
	vd->vbm_client = vbit_init(4096);
	vd->vbm_backend = vbit_init(4096);
	vd->vbm_supress = vbit_init(256);
	vd->vbm_select = vbit_init(256);

	vd->r_fd = -1;
	/* XXX: Allocate only if -r option given ? */
	vd->rbuflen = 256;	/* XXX ?? */
	vd->rbuf = malloc(vd->rbuflen * 4);
	assert(vd->rbuf != NULL);

	VTAILQ_INIT(&vd->sf_list);

	CHECK_OBJ_NOTNULL(vd, VSL_MAGIC);
	return (vd);
}

/*--------------------------------------------------------------------*/

void
VSL_Diag(struct VSL_data *vd, vsl_diag_f *func, void *priv)
{

	CHECK_OBJ_NOTNULL(vd, VSL_MAGIC);
	if (func == NULL)
		vd->diag = (vsl_diag_f*)getpid;
	else
		vd->diag = func;
	vd->priv = priv;
}

/*--------------------------------------------------------------------*/

int
VSL_n_Arg(struct VSL_data *vd, const char *opt)
{

	CHECK_OBJ_NOTNULL(vd, VSL_MAGIC);
	REPLACE(vd->n_opt, opt);
	AN(vd->n_opt);
	if (vin_n_arg(vd->n_opt, NULL, NULL, &vd->fname)) {
		vd->diag(vd->priv, "Invalid instance name: %s\n",
		    strerror(errno));
		return (-1);
	}
	return (1);
}

/*--------------------------------------------------------------------*/

const char *
VSL_Name(const struct VSL_data *vd)
{

	CHECK_OBJ_NOTNULL(vd, VSL_MAGIC);
	return (vd->n_opt);
}

/*--------------------------------------------------------------------*/

void
VSL_Delete(struct VSL_data *vd)
{
	struct vsl_sf *sf;

	CHECK_OBJ_NOTNULL(vd, VSL_MAGIC);
	VSL_Close(vd);
	vbit_destroy(vd->vbm_client);
	vbit_destroy(vd->vbm_backend);
	vbit_destroy(vd->vbm_supress);
	vbit_destroy(vd->vbm_select);
	free(vd->n_opt);
	free(vd->rbuf);
	free(vd->fname);

	while(!VTAILQ_EMPTY(&vd->sf_list)) {
		sf = VTAILQ_FIRST(&vd->sf_list);
		VTAILQ_REMOVE(&vd->sf_list, sf, next);
		free(sf->class);
		free(sf->ident);
		free(sf->name);
		free(sf);
	}

	free(vd);
}

/*--------------------------------------------------------------------*/

static int
vsl_open(struct VSL_data *vd, int diag)
{
	int i;
	struct vsm_head slh;

	if (vd->vsl_lh != NULL)
		return (0);

	vd->vsl_fd = open(vd->fname, O_RDONLY);
	if (vd->vsl_fd < 0) {
		if (diag)
			vd->diag(vd->priv, "Cannot open %s: %s\n",
			    vd->fname, strerror(errno));
		return (1);
	}

	assert(fstat(vd->vsl_fd, &vd->fstat) == 0);
	if (!S_ISREG(vd->fstat.st_mode)) {
		if (diag)
			vd->diag(vd->priv, "%s is not a regular file\n",
			    vd->fname);
		return (1);
	}

	i = read(vd->vsl_fd, &slh, sizeof slh);
	if (i != sizeof slh) {
		if (diag)
			vd->diag(vd->priv, "Cannot read %s: %s\n",
			    vd->fname, strerror(errno));
		return (1);
	}
	if (slh.magic != VSM_HEAD_MAGIC) {
		if (diag)
			vd->diag(vd->priv, "Wrong magic number in file %s\n",
			    vd->fname);
		return (1);
	}

	vd->vsl_lh = (void *)mmap(NULL, slh.shm_size,
	    PROT_READ, MAP_SHARED|MAP_HASSEMAPHORE, vd->vsl_fd, 0);
	if (vd->vsl_lh == MAP_FAILED) {
		if (diag)
			vd->diag(vd->priv, "Cannot mmap %s: %s\n",
			    vd->fname, strerror(errno));
		return (1);
	}
	vd->vsl_end = (uint8_t *)vd->vsl_lh + slh.shm_size;

	while(slh.alloc_seq == 0)
		usleep(50000);			/* XXX limit total sleep */
	vd->alloc_seq = slh.alloc_seq;
	return (0);
}

/*--------------------------------------------------------------------*/

int
VSL_Open(struct VSL_data *vd, int diag)

{

	return (vsl_open(vd, diag));
}

/*--------------------------------------------------------------------*/

void
VSL_Close(struct VSL_data *vd)
{
	if (vd->vsl_lh == NULL)
		return;
	assert(0 == munmap((void*)vd->vsl_lh, vd->vsl_lh->shm_size));
	vd->vsl_lh = NULL;
	assert(vd->vsl_fd >= 0);
	assert(0 == close(vd->vsl_fd));
	vd->vsl_fd = -1;
}

/*--------------------------------------------------------------------*/

int
VSL_ReOpen(struct VSL_data *vd, int diag)
{
	struct stat st;
	int i;

	AN(vd->vsl_lh);

	if (stat(vd->fname, &st))
		return (0);

	if (st.st_dev == vd->fstat.st_dev && st.st_ino == vd->fstat.st_ino)
		return (0);

	VSL_Close(vd);
	for (i = 0; i < 5; i++) {		/* XXX param */
		if (!vsl_open(vd, 0))
			return (1);
	}
	if (vsl_open(vd, diag))
		return (-1);
	return (1);
}

/*--------------------------------------------------------------------*/

struct vsm_chunk *
vsl_iter0(const struct VSL_data *vd)
{

	CHECK_OBJ_NOTNULL(vd, VSL_MAGIC);
	if (vd->alloc_seq != vd->vsl_lh->alloc_seq)
		return(NULL);
	CHECK_OBJ_NOTNULL(&vd->vsl_lh->head, VSM_CHUNK_MAGIC);
	return (&vd->vsl_lh->head);
}

void
vsl_itern(const struct VSL_data *vd, struct vsm_chunk **pp)
{

	CHECK_OBJ_NOTNULL(vd, VSL_MAGIC);
	if (vd->alloc_seq != vd->vsl_lh->alloc_seq) {
		*pp = NULL;
		return;
	}
	CHECK_OBJ_NOTNULL(*pp, VSM_CHUNK_MAGIC);
	*pp = VSM_NEXT(*pp);
	if ((void*)(*pp) >= vd->vsl_end) {
		*pp = NULL;
		return;
	}
	CHECK_OBJ_NOTNULL(*pp, VSM_CHUNK_MAGIC);
}

/*--------------------------------------------------------------------*/

struct vsm_chunk *
vsl_find_alloc(const struct VSL_data *vd, const char *class, const char *type, const char *ident)
{
	struct vsm_chunk *sha;

	assert (vd->vsl_lh != NULL);
	VSL_FOREACH(sha, vd) {
		CHECK_OBJ_NOTNULL(sha, VSM_CHUNK_MAGIC);
		if (strcmp(sha->class, class))
			continue;
		if (type != NULL && strcmp(sha->type, type))
			continue;
		if (ident != NULL && strcmp(sha->ident, ident))
			continue;
		return (sha);
	}
	return (NULL);
}

/*--------------------------------------------------------------------*/

void *
VSL_Find_Alloc(struct VSL_data *vd, const char *class, const char *type, const char *ident,
    unsigned *lenp)
{
	struct vsm_chunk *sha;

	CHECK_OBJ_NOTNULL(vd, VSL_MAGIC);
	sha = vsl_find_alloc(vd, class, type, ident);
	if (sha == NULL)
		return (NULL);
	if (lenp != NULL)
		*lenp = sha->len - sizeof *sha;
	return (VSM_PTR(sha));
}
