/* radare - LGPL - Copyright 2021 - RHL120, pancake */

#include "r_config.h"
#include "r_core.h"
#include "types.h"
#include <rvc.h>
#include <r_util.h>
#include <sdb.h>
#define FIRST_BRANCH "branches.master"
#define NOT_SPECIAL(c) IS_DIGIT (c) || IS_LOWER (c) || c == '_'
#define COMMIT_BLOB_SEP "----"
#define DBNAME "branches.sdb"
#define CURRENTB "current_branch"
#define IGNORE_NAME ".rvc_ignore"
#define MAX_MESSAGE_LEN 80
#define NULLVAL "-"

//copies src to dst and creates the parent dirs if they do not exist.
static bool file_copyp(const char *src, const char *dst) {
	if (r_file_is_directory (dst)) {
		return r_file_copy (src, dst);
	}
	const char *d = r_str_rchr (dst, dst + r_str_len_utf8 (dst) - 1, *R_SYS_DIR);
	if (!d) {
		return false;
	}
	char *dir = r_str_ndup (dst, d - dst);
	if (!dir) {
		return false;
	}
	if (!r_file_is_directory (dir)) {
		if (!r_sys_mkdirp (dir)) {
			free (dir);
			return false;
		}
	}
	bool res = r_file_copy (src, dst);
	free (dir);
	return res;
}

//should I move to file.c?
bool file_copyrf(const char *src, const char *dst) {
	if (r_file_exists (src)) {
		return file_copyp (src, dst);
	}
	RList *fl = r_file_lsrf (src);
	if (!fl) {
		return false;
	}
	RListIter *iter;
	const char *path;
	bool ret = true;
	r_list_foreach (fl, iter, path) {
		//strlen(src) should always be less than strlen(path) so
		//I think this is ok??
		char *dstp = r_file_new (dst, path + strlen (src), NULL);
		if (dstp) {
			if (r_file_is_directory (path)) {
				r_sys_mkdirp (dstp);
			} else {
				if (!file_copyp (path, dstp)) {
				eprintf ("Failed to copy the file: %s to %s\n",
						path, dstp);
				ret = false;
				//continue copying files don't break
				}
			}
			free (dstp);
		} else {
			ret = false;
			eprintf ("Failed to copy the file: %s\n", path);
		}
	}
	return ret;
}

static char *strip_sys_dir(const char *path) {
	char *res = strdup (path);
	char *ptr = res;
	while (*ptr) {
		if (*ptr == *R_SYS_DIR) {
			if (ptr[1] == *R_SYS_DIR) {
				char *ptr2 = ptr + 1;
				while (*ptr2 == *R_SYS_DIR) {
					ptr2++;
				}
				memmove (ptr + 1, ptr2, strlen (ptr2) + 1);
			}
		}
		ptr++;
	}
	return res;
}

static Sdb *vcdb_open(const char *rp) {
	char *frp = r_file_new (rp, ".rvc", DBNAME, NULL);
	if (!frp) {
		return NULL;
	}
	Sdb *db = sdb_new0 ();
	if (!db) {
		free (frp);
		return NULL;
	}
	if (sdb_open (db, frp) < 0) {
		free (frp);
		sdb_free (db);
		return NULL;
	}
	free (frp);
	return db;
}

static bool repo_exists(const char *path) {
	char *rp = r_file_new (path, ".rvc", NULL);
	if (!rp) {
		return false;
	}
	if (!r_file_is_directory (rp)) {
		free (rp);
		return false;
	}
	bool r = true;
	char *files[3] = {r_file_new (rp, DBNAME, NULL),
		r_file_new (rp, "commits", NULL),
		r_file_new (rp, "blobs", NULL)
	};
	free (rp);
	size_t i;
	for (i = 0; i < 3; i++) {
		if (!files[i]) {
			r = false;
			break;
		}
		if (!r_file_is_directory (files[i]) && !r_file_exists (files[i])) {
			eprintf ("Error: Corrupt repo: %s doesn't exist\n",
					files[i]);
			r = false;
			break;
		}

	}
	free (files[0]);
	free (files[1]);
	free (files[2]);
	return r;
}

static bool is_valid_branch_name(const char *name) {
	if (r_str_len_utf8 (name) >= 16) {
		return false;
	}
	for (; *name; name++) {
		if (NOT_SPECIAL (*name)) {
			continue;
		}
		return false;
	}
	return true;
}

static char *find_sha256(const ut8 *block, int len) {
	RHash *ctx = r_hash_new (true, R_HASH_SHA256);
	if (!ctx) {
		return NULL;
	}
	const ut8 *c = r_hash_do_sha256 (ctx, block, len);
	char *ret = r_hex_bin2strdup (c, R_HASH_SIZE_SHA256);
	r_hash_free (ctx);
	return ret;
}

static inline char *sha256_file(const char *fname) {
	size_t content_length = 0;
	char *res = NULL;
	char *content = r_file_slurp (fname, &content_length);
	if (content) {
		res = find_sha256 ((const ut8 *)content, content_length);
		free (content);
	}
	return res;
}

static void free_blobs(RList *blobs) {
	RListIter *iter;
	RvcBlob *blob;
	r_list_foreach (blobs, iter, blob) {
		free (blob->fhash);
		free (blob->fname);
	}
	r_list_free (blobs);
}

static char *absp2rp(const char *rp, const char *absp) {
	char *p;
	char *arp = r_file_abspath (rp);
	if (!arp) {
		return NULL;
	}
	if (r_str_len_utf8 (arp) < r_str_len_utf8 (rp)) {
		free (arp);
		return NULL;
	}
	p = r_str_new (absp + r_str_len_utf8 (arp));
	free (arp);
	if (!p) {
		return NULL;
	}
	char *ret = strip_sys_dir (p);
	free (p);
	return ret;
}

char *rp2absp(const char *rp, const char *path) {
	char *arp = r_file_abspath (rp);
	if (!arp) {
		return NULL;
	}
	char *appended = r_file_new (arp, path, NULL);
	free (arp);
	if (!appended) {
		return NULL;
	}
	char *stripped = strip_sys_dir (appended);
	free (appended);
	return stripped;
}

//TODO:Make the tree related functions abit cleaner & more efficient

static RList *get_commits(const char *rp, const size_t max_num) {
	char *i;
	RList *ret = r_list_new ();
	if (!ret) {
		return NULL;
	}
	Sdb *db = vcdb_open(rp);
	i = sdb_get (db, sdb_const_get (db, CURRENTB, 0), 0);
	if (!i) {
		r_list_free (ret);
		sdb_unlink (db);
		sdb_free (db);
		return NULL;
	}
	if (!strcmp (i, NULLVAL)) {
		sdb_unlink (db);
		sdb_free (db);
		return ret;
	}
	while (true) {
		if (!r_list_prepend (ret, i)) {
			r_list_free (ret);
			break;
		}
		i = sdb_get (db, i, 0);
		if (!i) {
			r_list_free (ret);
			ret = NULL;
			break;
		}
		if (!strcmp (i, NULLVAL) || (max_num && ret->length >= max_num)) {
			break;
		}
	}
	sdb_unlink (db);
	sdb_free (db);
	return ret;
}


// check if rpf is in the ignore file. if ignore is NULL it just returns false
static bool in_rvc_ignore(const RList *ignore, const char *rpf) {
	RListIter *iter;
	char *p;
	bool ret = false;
	if (!ignore) {
		return false;
	}
	r_list_foreach (ignore, iter, p) {
		char *stripped = strip_sys_dir (p);
		if (stripped) {
			if (!strcmp (stripped, rpf)) {
				free (stripped);
				ret = true;
				break;
			}
			free (stripped);
		}

	}
	return ret;
}

static bool update_blobs(const RList *ignore, RList *blobs, const RList *nh) {
	RListIter *iter;
	RvcBlob *blob;
	if (in_rvc_ignore (ignore, nh->head->data)) {
		return true;
	}
	r_list_foreach (blobs, iter, blob) {
		if (strcmp (nh->head->data, blob->fname)) {
			continue;
		}
		blob->fhash = r_str_new (nh->tail->data);
		return blob->fhash;
	}
	blob = R_NEW (RvcBlob);
	if (!blob) {
		return false;
	}
	blob->fhash = r_str_new (nh->tail->data);
	blob->fname = r_str_new (nh->head->data);
	if (!blob->fhash || !blob->fname) {
		free (blob->fhash);
		free (blob->fname);
		free (blob);
		return false;
	}
	if (!r_list_append (blobs, blob)) {
		free (blob->fhash);
		free (blob->fname);
		free (blob);
		return false;
	}
	return true;
}

static int branch_exists(const char *rp, const char *bname) {
	RList *branches = r_vc_get_branches (rp);
	if (!branches) {
		return -1;
	}
	RListIter *iter;
	char *branch;
	bool ret = 0;
	r_list_foreach (branches, iter, branch) {
		branch = branch + r_str_len_utf8 (BPREFIX);
		if (!strcmp (branch, bname)) {
			ret = 1;
			break;
		}
	}
	r_list_free (branches);
	return ret;
}

static RList *get_blobs(const char *rp, RList *ignore) {
	RList *commits = get_commits (rp, 0);
	if (!commits) {
		return NULL;
	}
	RList *ret = r_list_new ();
	if (!ret) {
		r_list_free (commits);
	}
	RListIter *i;
	char *hash;
	r_list_foreach (commits, i, hash) {
		char *commit_path = r_file_new (rp, ".rvc", "commits",
				hash, NULL);
		if (!commit_path) {
			free_blobs (ret);
			ret = NULL;
			break;
		}
		char *content = r_file_slurp (commit_path, 0);
		free (commit_path);
		if (!content) {
			free_blobs (ret);
			ret = NULL;
			break;
		}
		RList *lines = r_str_split_duplist (content, "\n", true);
		free (content);
		if (!lines) {
			free_blobs (ret);
			ret = NULL;
			break;
		}
		RListIter *j;
		char *ln;
		bool found = false;
		r_list_foreach (lines, j, ln) {
			if (!found) {
				found = !r_str_cmp (ln, COMMIT_BLOB_SEP, r_str_len_utf8 (COMMIT_BLOB_SEP));
				continue;
			}
			RvcBlob *blob = R_NEW (RvcBlob);
			if (!blob) {
				free_blobs (ret);
				ret = NULL;
				break;
			}
			RList *kv = r_str_split_list (ln, "=", 2);
			if (!kv) {
				free_blobs (ret);
				ret = NULL;
				break;
			}
			if (!update_blobs (ignore, ret, kv)) {
				free_blobs (ret);
				ret = NULL;
				free (kv);
				break;
			}
		}
		r_list_free (lines);
	}
	r_list_free (commits);
	return ret;
}

static bool rm_empty_dir(const char *rp) {
	char *rvc = r_file_new (rp, ".rvc", NULL);
	if (!rvc) {
		return false;
	}
	RList *files = r_file_lsrf (rp);
	RListIter *iter;
	const char *f;
	r_list_foreach (files, iter, f) {
		if (r_str_cmp (f, rvc, r_str_len_utf8 (rvc))) {
			rmdir (f);
		}
	}
	free (rvc);
	r_list_free (files);
	return true;
}

static bool traverse_files(RList *dst, const char *dir) {
	char *name;
	RListIter *iter;
	bool ret = true;
	RList *files = r_sys_dir (dir);
	if (!r_list_empty (dst)) {
		char *vcp = r_file_new (dir, ".rvc", NULL);
		if (!vcp) {
			r_list_free (files);
			return false;
		}
		if (r_file_is_directory (vcp)) {
			r_list_free (files);
			free (vcp);
			return true;
		}
		free (vcp);
	}
	if (!files) {
		r_list_free (files);
		return false;
	}
	r_list_foreach (files, iter, name) {
		char *path;
		if (!strcmp (name, "..") || !strcmp (name, ".")) {
			continue;
		}
		if (!strcmp (name, ".rvc")) {
			continue;
		}
		path = r_file_new (dir, name, NULL);
		if (!path) {
			ret = false;
			break;
		}
		if (r_file_is_directory (path)) {
			if (!traverse_files (dst, path)) {
				ret = false;
				break;
			}
			free (path);
			continue;
		}
		if (!r_list_append (dst, path)) {
			ret = false;
			free (path);
			break;
		}
	}
	r_list_free (files);
	return ret;
}

static RList *repo_files(const char *dir) {
	RList *ret = r_list_newf (free);
	if (ret) {
		if (!traverse_files (ret, dir)) {
			r_list_free (ret);
			ret = NULL;
		}
	}
	return ret;
}

static RList *load_rvc_ignore(const char *rp) {
	RList *ignore = NULL;
	char *path = r_file_new (rp, IGNORE_NAME, NULL);
	if (!path) {
		return false;
	}
	char *c = r_file_slurp (path, 0);
	// skip if contnet is not readable
	if (c) {
		ignore = r_str_split_duplist (c, "\n", true);
		free (c);
	}
	free (path);
	return ignore;
}

//shit function:
R_API RList *r_vc_get_uncommitted(const char *rp) {
	RList *ignore = load_rvc_ignore (rp);
	if (!repo_exists (rp)) {
		eprintf ("No valid repo in %s\n", rp);
		return false;
	}
	RList *blobs = get_blobs(rp, ignore);
	if (!blobs) {
		return NULL;
	}
	RList *files = repo_files (rp);
	if (!files) {
		free_blobs (blobs);
		return NULL;
	}
	RList *ret = r_list_new ();
	if (!ret) {
		free_blobs (blobs);
		r_list_free (files);
		return NULL;
	}
	RListIter *iter;
	RvcBlob *blob;
	r_list_foreach (blobs, iter, blob) {
		char *blob_absp = rp2absp (rp, blob->fname);
		if (!blob_absp) {
			goto fail_ret;
		}
		char *file;
		RListIter *j, *tmp;
		bool found = false;
		r_list_foreach_safe (files, j, tmp, file) {
			if (strcmp (blob_absp, file)) {
				continue;
			}
			found = true;
			r_list_delete (files, j);
			char *file_hash = sha256_file (blob_absp);
			if (!file_hash) {
				free (blob_absp);
				goto fail_ret;
			}
			if (!strcmp (file_hash, blob->fhash)) {
				free (file_hash);
				break;
			}
			free (file_hash);
			char *append = r_str_new (blob_absp);
			if (!append) {
				free (blob_absp);
				goto fail_ret;
			}
			if (!r_list_append (ret, blob_absp)) {
				free (blob_absp);
				goto fail_ret;
			}
		}
		if (!found) {
			if (!strcmp (NULLVAL, blob->fhash)) {
				free (blob_absp);
				continue;
			}
			if (!r_list_append (ret, blob_absp)) {
				free (blob_absp);
				goto fail_ret;
			}
		}
	}
	char *file;
	free_blobs (blobs);
	blobs = NULL;
	r_list_foreach (files, iter, file) {
		char *rfp = absp2rp (rp, file);
		if (!rfp) {
			goto fail_ret;
		}
		if (in_rvc_ignore (ignore, rfp)) {
			free (rfp);
			continue;
		}
		free (rfp);
		char *append = r_str_new (file);
		if (!append) {
			goto fail_ret;
		}
		if (!r_list_append (ret, append)) {
			free (append);
			goto fail_ret;
		}
	}
	r_list_free (files);
	return ret;
fail_ret:
	r_list_free (files);
	r_list_free (ret);
	free_blobs (blobs);
	return NULL;
}

static char *find_blob_hash(const char *rp, const char *fname) {
	RList *blobs = get_blobs (rp, load_rvc_ignore(rp));
	if (blobs) {
		RListIter *i;
		RvcBlob *b;
		r_list_foreach_prev (blobs, i, b) {
			if (!strcmp (b->fname, fname)) {
				char *bhash = r_str_new (b->fhash);
				free_blobs (blobs);
				return bhash;
			}
		}
	}
	return NULL;
}

static char *write_commit(const char *rp, const char *message, const char *author, RList *blobs) {
	RvcBlob *blob;
	RListIter *iter;
	char *content = r_str_newf ("message=%s\nauthor=%s\ntime=%" PFMT64x "\n"
			COMMIT_BLOB_SEP, message, author, (ut64) r_time_now ());
	if (!content) {
		return false;
	}
	r_list_foreach (blobs, iter, blob) {
		content = r_str_appendf (content, "\n%s=%s", blob->fname,
				blob->fhash);
		if (!content) {
			return false;
		}
	}
	char *commit_hash = find_sha256 ((unsigned char *)
			content, r_str_len_utf8 (content));
	if (!commit_hash) {
		free (content);
		return false;
	}
	char *commit_path = r_file_new (rp, ".rvc","commits", commit_hash, NULL);
	if (!commit_path || !r_file_dump (commit_path, (const ut8*)content, -1, false)) {
		free (content);
		free (commit_hash);
		return false;
	}
	free (content);
	return commit_hash;
}

static RvcBlob *bfadd(const char *rp, const char *fname) {
	RvcBlob *ret = R_NEW (RvcBlob);
	if (!ret) {
		return NULL;
	}
	char *absp = r_file_abspath (fname);
	if (!absp) {
		free (ret);
		return NULL;
	}
	ret->fname = absp2rp (rp, absp);
	if (!ret->fname) {
		free (ret);
		free (absp);
		return NULL;
	}
	if (!r_file_exists (absp)) {
		ret->fhash = r_str_new (NULLVAL);
		free (absp);
		if (!ret->fhash) {
			goto fail_ret;
		}
		return ret;
	}
	ret->fhash = sha256_file (absp);
	if (!ret->fhash) {
		free (absp);
		goto fail_ret;
	}
	char *bpath = r_file_new (rp, ".rvc", "blobs", ret->fhash, NULL);
	if (!bpath) {
		goto fail_ret;
	}
	if (!r_file_copy (absp, bpath)) {
		free (ret->fhash);
		free (ret->fname);
		free (ret);
		ret = NULL;
	}
	free (absp);
	free (bpath);
	return ret;

fail_ret:
	free (ret->fhash);
	free (ret->fname);
	free (ret);
	return NULL;
}

static RList *blobs_add(const char *rp, const RList *files) {
	RList *ret = r_list_new ();
	if (!ret) {
		return NULL;
	}
	RList *uncommitted = r_vc_get_uncommitted (rp);
	if (!uncommitted) {
		free (ret);
		return NULL;
	}
	RListIter *i;
	char *path;
	r_list_foreach (files, i, path) {
		char *absp = r_file_abspath (path);
		if (!absp) {
			break;
		}
		RListIter *j;
		char *ucp;
		bool found = false;
		RListIter *tmp;
		//problamatic iterates even after finding the file but needed for directires.
		r_list_foreach_safe (uncommitted, j, tmp, ucp) {
			if (r_str_cmp (ucp, absp, r_str_len_utf8 (absp))) {
				continue;
			}
			found = true;
			RvcBlob *b = bfadd (rp, ucp);
			if (!b) {
				free (absp);
				goto fail_ret;
			}
			if (!r_list_append (ret, b)) {
				free (absp);
				free (b->fhash);
				free (b->fname);
				free (b);
				goto fail_ret;
			}
			r_list_delete (uncommitted, j);
		}
		if (!found) {
			eprintf ("File %s is already committed\n", path);
			free (absp);
		}
	}
	return ret;
fail_ret:
	r_list_free (uncommitted);
	free (ret);
	return NULL;
}

R_API bool r_vc_commit(const char *rp, const char *message, const char *author, const RList *files) {
	char *commit_hash;
	if (!repo_exists (rp)) {
		eprintf ("No valid repo in %s\n", rp);
		return false;
	}
	if (R_STR_ISEMPTY (message)) {
		char *path = NULL;
		(void)r_file_mkstemp ("rvc", &path);
		if (path) {
			free (r_cons_editor (path, NULL));
			message = r_file_slurp (path, NULL);
			if (!message) {
				free (path);
				return false;
			}
		} else {
			return false;
		}
	}
	if (message && r_str_len_utf8 (message) > MAX_MESSAGE_LEN) {
		eprintf ("Commit message is too long\n");
		return false;
	}
	const char *m;
	for (m = message; *m; m++) {
		if (*m < ' ') {
			eprintf ("commit messages must not contain unprintable charecters\n");
			return false;
		}
	}
	RList *blobs = blobs_add (rp, files);
	if (!blobs) {
		return false;
	}
	if (r_list_empty (blobs)) {
		r_list_free (blobs);
		eprintf ("Nothing to commit\n");
		return false;
	}
	commit_hash = write_commit (rp, message, author, blobs);
	if (!commit_hash) {
		free_blobs (blobs);
		return false;
	}
	{
		const char *current_branch;
		Sdb *db = vcdb_open (rp) ;
		current_branch = sdb_const_get (db, CURRENTB, 0);
		if (sdb_set (db, commit_hash, sdb_const_get (db, current_branch, 0), 0) < 0) {
			sdb_unlink (db);
			sdb_free (db);
			free_blobs (blobs);
			free (commit_hash);
			return false;
		}
		if (sdb_set (db, current_branch, commit_hash, 0) < 0) {
			sdb_unlink (db);
			sdb_free (db);
			free_blobs (blobs);
			free (commit_hash);
			return false;
		}
		sdb_sync (db);
		sdb_unlink (db);
		sdb_free (db);
	}
	free (commit_hash);
	free_blobs (blobs);
	return true;
}

R_API RList *r_vc_get_branches(const char *rp) {
	if (!repo_exists (rp)) {
		eprintf ("No valid repo in %s\n", rp);
		return NULL;
	}
	Sdb *db = vcdb_open (rp);
	RList *ret = r_list_new ();
	if (!ret) {
		sdb_unlink (db);
		sdb_free (db);
		return NULL;
	}
	SdbList *keys = sdb_foreach_list (db, false);
	if (!keys) {
		sdb_unlink (db);
		sdb_free (db);
		r_list_free (ret);
		return NULL;
	}
	SdbListIter *i;
	SdbKv *kv;
	ls_foreach (keys, i, kv) {
		if (r_str_cmp ((char *)kv->base.key,
					BPREFIX, r_str_len_utf8 (BPREFIX))) {
			continue;
		}
		if (!r_list_append (ret, r_str_new (kv->base.key))
				&& !ret->head->data) {
			r_list_free (ret);
			ret = NULL;
			break;
		}
	}
	ls_free (keys);
	sdb_unlink (db);
	sdb_free (db);
	return ret;
}

R_API bool r_vc_branch(const char *rp, const char *bname) {
	const char *current_branch;
	const char *commits;
	if (!repo_exists (rp)) {
		eprintf ("No valid repo in %s\n", rp);
		return false;
	}
	if (!is_valid_branch_name (bname)) {
		eprintf ("The branch name %s is invalid\n", bname);
		return false;
	}
	{
		int ret = branch_exists (rp, bname);
		if (ret < 0) {
			return false;
		} else if (ret) {
			eprintf ("The branch %s already exists\n", bname);
			return false;
		}
	}
	Sdb *db = vcdb_open (rp);
	current_branch = sdb_const_get (db, CURRENTB, 0);
	if (!current_branch) {
		sdb_unlink (db);
		sdb_free (db);
		return false;
	}
	commits = sdb_const_get (db, current_branch, 0);
	char *nbn = r_str_newf (BPREFIX "%s", bname);
	if (!nbn) {
		sdb_unlink (db);
		sdb_free (db);
		return false;
	}
	sdb_set (db, nbn, commits, 0);
	free (nbn);
	sdb_sync (db);
	sdb_unlink (db);
	sdb_free (db);
	return true;
}

R_API bool r_vc_new(const char *path) {
	Sdb *db;
	char *commitp, *blobsp;
	char *vcp = r_file_new (path, ".rvc", NULL);
	if (r_file_is_directory (vcp)) {
		eprintf ("A repository already exists in %s\n", path);
		free (vcp);
		return false;

	}
	if (!vcp) {
		return false;
	}
	commitp = r_file_new (vcp, "commits", NULL);
	blobsp = r_file_new (vcp, "blobs", NULL);
	if (!commitp || !blobsp) {
		free (commitp);
		free (blobsp);
		free (vcp);
		return false;
	}
	if (!r_sys_mkdirp (commitp) || !r_sys_mkdir (blobsp)) {
		eprintf ("Can't create The RVC repo directory");
		free (commitp);
		free (vcp);
		free (blobsp);
		return false;
	}
	free (commitp);
	free (blobsp);
	db = sdb_new (vcp, DBNAME, 0);
	free (vcp);
	if (!db) {
		eprintf ("Can't create The RVC branches database");
		return false;
	}
	if (!sdb_set (db, FIRST_BRANCH, NULLVAL, 0)) {
		sdb_unlink (db);
		sdb_free (db);
		return false;
	}
	if (!sdb_set (db, CURRENTB, FIRST_BRANCH, 0)) {
		sdb_unlink (db);
		sdb_free (db);
		return false;
	}
	sdb_sync (db);
	sdb_unlink (db);
	sdb_free (db);
	return true;
}

R_API bool r_vc_checkout(const char *rp, const char *bname) {
	if (!repo_exists (rp)) {
		eprintf ("No valid repo in %s\n", rp);
		return false;
	}
	{
		int ret = branch_exists (rp, bname);
		if (ret < 0) {
			return false;
		}
		if (ret == 0) {
			eprintf ("The branch %s doesn't exist.\n", bname);
			return false;
		}
	}
	RList *uncommitted = r_vc_get_uncommitted (rp);
	RListIter *i;
	char *file;
	if (!uncommitted) {
		return false;
	}
	if (!r_list_empty (uncommitted)) {
		eprintf ("The following files:\n");
		r_list_foreach (uncommitted, i, file) {
			eprintf ("%s\n", file);
		}
		eprintf ("Are uncommitted.\nCommit them before checkout\n");
		r_list_free (uncommitted);
		return false;
	}
	r_list_free (uncommitted);
	//Must set to NULL to avoid double r_list_free on fail_ret
	uncommitted = NULL;
	Sdb *db = vcdb_open (rp) ;
	if (!db) {
		return false;
	}
	const char *oldb;
	{
		char *fbname = r_str_newf (BPREFIX "%s", bname);
		if (!fbname) {
			sdb_unlink (db);
			sdb_free (db);
			return false;
		}
		oldb = sdb_const_get (db, CURRENTB, 0);
		sdb_set (db, CURRENTB, fbname, 0);
		free (fbname);
		if (!sdb_sync (db)) {
			sdb_unlink (db);
			sdb_free (db);
			return false;
		}
	}
	if (!r_vc_reset (rp)) {
		goto fail_ret;
	}
	if (!rm_empty_dir (rp)) {
		goto fail_ret;
	}
	sdb_sync (db);
	sdb_unlink (db);
	sdb_free (db);
	return true;
fail_ret:
	r_list_free (uncommitted);
	sdb_set (db, CURRENTB, oldb, 0);
	sdb_sync (db);
	sdb_unlink (db);
	sdb_free (db);
	return false;
}

R_API RList *r_vc_log(const char *rp) {
	if (!repo_exists (rp)) {
		eprintf ("No valid repo in %s\n", rp);
		return false;
	}
	RList *commits = get_commits (rp, 0);
	if (!commits) {
		return NULL;
	}
	RListIter *iter;
	char *ch;
	r_list_foreach_prev (commits, iter, ch) {
		char *cp = r_file_new (rp, ".rvc", "commits", ch, NULL);
		if (!cp) {
			goto fail_ret;
		}
		char *contnet = r_file_slurp (cp, 0);
		free (cp);
		if (!contnet) {
			goto fail_ret;
		}
		iter->data = r_str_newf ("hash=%s", (char *) iter->data);
		if (!iter->data) {
			free (contnet);
			goto fail_ret;
		}
		free (ch);
		iter->data = r_str_appendf (iter->data, "\n%s", contnet);
		free (contnet);
		if (!iter->data) {
			goto fail_ret;
		}
	}
	return commits;
fail_ret:
	r_list_free (commits);
	return NULL;
}

R_API char *r_vc_current_branch(const char *rp) {
	if (!repo_exists (rp)) {
		eprintf ("No valid repo in %s\n", rp);
		return false;
	}
	Sdb *db = vcdb_open (rp);
	if (!db) {
		return NULL;
	}
	//TODO: return consistently either BPREFIX.bname or bname
	char *ret = r_str_new (sdb_const_get (db, CURRENTB, 0) + r_str_len_utf8 (BPREFIX));
	sdb_unlink (db);
	sdb_close (db);
	sdb_free (db);
	return ret;
}

R_API bool r_vc_clone(const char *src, const char *dst) {
	printf ("src, dst: %s, %s\n", src, dst);
	char *drp = r_file_new (dst, ".rvc", NULL);
	bool ret = false;
	if (drp) {
		char *srp = r_file_new (src, ".rvc", NULL);
		if (srp) {
			if (file_copyrf (srp, drp)) {
				if (r_vc_reset (dst)) {
					ret = true;
				} else {
					eprintf("Failed to reset\n");
				}
			} else {
				eprintf ("Failed to copy files\n");
			}
			free (srp);
		}
		free (drp);
	}
	return ret;
}

// GIT commands as APIs

R_API bool r_vc_git_init(const char *path) {
	char *escpath = r_str_escape (path);
	int ret = r_sys_cmdf ("git init \"%s\"", escpath);
	free (escpath);
	return !ret;
}

R_API bool r_vc_git_branch(const char *path, const char *name) {
	char *escpath = r_str_escape (path);
	if (!escpath) {
		return false;
	}
	char *escname = r_str_escape (name);
	if (!escname) {
		free (escpath);
		return false;
	}
	int ret = r_sys_cmdf ("git -C \"%s\" branch \"%s\"", escpath, escname);
	free (escpath);
	free (escname);
	return !ret;
}

R_API bool r_vc_git_checkout(const char *path, const char *name) {
	char *escpath = r_str_escape (path);
	char *escname = r_str_escape (name);
	int ret = r_sys_cmdf ("git -C \"%s\" checkout \"%s\"", escpath, escname);
	free (escname);
	free (escpath);
	return !ret;
}

R_API bool r_vc_git_add(const char *path, const char *fname) {
	char *cwd = r_sys_getdir ();
	if (!cwd) {
		return false;
	}
	if (!r_sys_chdir (path)) {
		free (cwd);
		return false;
	}
	char *escfname = r_str_escape (fname);
	int ret = r_sys_cmdf ("git add \"%s\"", escfname);
	free (escfname);
	if (!r_sys_chdir (cwd)) {
		free (cwd);
		return false;
	}
	free (cwd);
	return ret == 0;
}

R_API bool r_vc_git_commit(const char *path, const char *message) {
	if (R_STR_ISEMPTY (message)) {
		char *epath = r_str_escape (path);
		int res = r_sys_cmdf ("git -C \"%s\" commit", epath);
		free (epath);
		return res == 0;
	}
	char *epath = r_str_escape (path);
	char *emsg = r_str_escape (message);
	int res = r_sys_cmdf ("git -C %s commit -m %s", epath, emsg);
	free (epath);
	free (emsg);
	return res == 0;
}

R_API bool r_vc_reset(const char *rp) {
	if (!repo_exists (rp)) {
		return false;
	}
	bool ret = true;
	RList *uncommitted = r_vc_get_uncommitted (rp);
	if (!uncommitted) {
		return false;
	}
	RListIter *iter;
	const char *fp;
	r_list_foreach (uncommitted, iter, fp) {
		char *blobp;
		{
			char *p = absp2rp (rp, fp);
			if (!p) {
				ret = false;
				break;
			}
			char *b = find_blob_hash (rp, p);
			if (!b || !strcmp (b, "-")) {
				free (p);
				if (!r_file_rm (fp)) {
					ret = false;
					break;
				}
				continue;

			}
			blobp = r_file_new (rp, ".rvc", "blobs", b, NULL);
			free (b);
		}
		if (!blobp) {
			ret = false;
			break;
		}
		if (!file_copyp (blobp, fp)) {
			free (blobp);
			ret = false;
			break;
		}
	}
	r_list_free (uncommitted);
	return ret;
}

//Access both git and rvc functionality from one set of functions

R_API bool rvc_git_init(const RCore *core, const char *rp) {
	if (!strcmp (r_config_get (core->config, "prj.vc.type"), "git")) {
		return r_vc_git_init (rp);
	}
	printf ("rvc is just for testing please don't use it\n");
	return r_vc_new (rp);
}

R_API bool rvc_git_commit(RCore *core, const char *rp, const char *message, const char *author, const RList *files) {
	const char *m = r_config_get (core->config, "prj.vc.message");
	if (!*m) {
		if (!r_cons_is_interactive ()) {
			r_config_set (core->config, "prj.vc.message", "test");
			m = r_config_get (core->config, "prj.vc.message");
		}
	}
	message = R_STR_ISEMPTY (message)? m : message;
	if (!strcmp (r_config_get (core->config, "prj.vc.type"), "rvc")) {
		author = author? author : r_config_get (core->config, "cfg.user");
		eprintf ("rvc is just for testing please don't use it\n");
		return r_vc_commit (rp, message, author, files);
	}
	char *path;
	RListIter *iter;
	r_list_foreach (files, iter, path) {
		if (!r_vc_git_add (rp, path)) {
			return false;
		}
	}
	return r_vc_git_commit (rp, message);
}

R_API bool rvc_git_branch(const RCore *core, const char *rp, const char *bname) {
	if (!strcmp (r_config_get (core->config, "prj.vc.type"), "rvc")) {
		eprintf ("rvc is just for testing please don't use it\n");
		return r_vc_branch (rp, bname);
	}
	return !r_vc_git_branch (rp, bname);
}

R_API bool rvc_git_checkout(const RCore *core, const char *rp, const char *bname) {
	if (!strcmp (r_config_get (core->config, "prj.vc.type"), "rvc")) {
		eprintf ("rvc is just for testing please don't use it\n");
		return r_vc_checkout (rp, bname);
	}
	return r_vc_git_checkout (rp, bname);
}

R_API bool rvc_git_repo_exists(const RCore *core, const char *rp) {
	char *frp = !strcmp (r_config_get (core->config, "prj.vc.type"), "rvc")?
		r_file_new (rp, ".rvc", NULL):
		r_file_new (rp, ".git", NULL);
	if (frp) {
		bool ret = r_file_is_directory (frp);
		free (frp);
		return ret;
	}
	return false;
}
