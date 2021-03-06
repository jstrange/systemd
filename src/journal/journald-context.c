/***
  This file is part of systemd.

  Copyright 2017 Lennart Poettering

  systemd is free software; you can redistribute it and/or modify it
  under the terms of the GNU Lesser General Public License as published by
  the Free Software Foundation; either version 2.1 of the License, or
  (at your option) any later version.

  systemd is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with systemd; If not, see <http://www.gnu.org/licenses/>.
***/

#if HAVE_SELINUX
#include <selinux/selinux.h>
#endif

#include "alloc-util.h"
#include "audit-util.h"
#include "cgroup-util.h"
#include "journald-context.h"
#include "process-util.h"
#include "string-util.h"
#include "user-util.h"

/* This implements a metadata cache for clients, which are identified by their PID. Requesting metadata through /proc
 * is expensive, hence let's cache the data if we can. Note that this means the metadata might be out-of-date when we
 * store it, but it might already be anyway, as we request the data asynchronously from /proc at a different time the
 * log entry was originally created. We hence just increase the "window of inaccuracy" a bit.
 *
 * The cache is indexed by the PID. Entries may be "pinned" in the cache, in which case the entries are not removed
 * until they are unpinned. Unpinned entries are kept around until cache pressure is seen. Cache entries older than 5s
 * are never used (a sad attempt to deal with the UNIX weakness of PIDs reuse), cache entries older than 1s are
 * refreshed in an incremental way (meaning: data is reread from /proc, but any old data we can't refresh is not
 * flushed out). Data newer than 1s is used immediately without refresh.
 *
 * Log stream clients (i.e. all clients using the AF_UNIX/SOCK_STREAM stdout/stderr transport) will pin a cache entry
 * as long as their socket is connected. Note that cache entries are shared between different transports. That means a
 * cache entry pinned for the stream connection logic may be reused for the syslog or native protocols.
 *
 * Caching metadata like this has two major benefits:
 *
 * 1. Reading metadata is expensive, and we can thus substantially speed up log processing under flood.
 *
 * 2. Because metadata caching is shared between stream and datagram transports and stream connections pin a cache
 *    entry there's a good chance we can properly map a substantial set of datagram log messages to their originating
 *    service, as all services (unless explicitly configured otherwise) will have their stdout/stderr connected to a
 *    stream connection. This should improve cases where a service process logs immediately before exiting and we
 *    previously had trouble associating the log message with the service.
 *
 * NB: With and without the metadata cache: the implicitly added entry metadata in the journal (with the exception of
 *     UID/PID/GID and SELinux label) must be understood as possibly slightly out of sync (i.e. sometimes slighly older
 *     and sometimes slightly newer than what was current at the log event).
 */

/* We refresh every 1s */
#define REFRESH_USEC (1*USEC_PER_SEC)

/* Data older than 5s we flush out */
#define MAX_USEC (5*USEC_PER_SEC)

/* Keep at most 16K entries in the cache. (Note though that this limit may be violated if enough streams pin entries in
 * the cache, in which case we *do* permit this limit to be breached. That's safe however, as the number of stream
 * clients itself is limited.) */
#define CACHE_MAX (16*1024)

static int client_context_compare(const void *a, const void *b) {
        const ClientContext *x = a, *y = b;

        if (x->timestamp < y->timestamp)
                return -1;
        if (x->timestamp > y->timestamp)
                return 1;

        if (x->pid < y->pid)
                return -1;
        if (x->pid > y->pid)
                return 1;

        return 0;
}

static int client_context_new(Server *s, pid_t pid, ClientContext **ret) {
        ClientContext *c;
        int r;

        assert(s);
        assert(pid_is_valid(pid));
        assert(ret);

        r = hashmap_ensure_allocated(&s->client_contexts, NULL);
        if (r < 0)
                return r;

        r = prioq_ensure_allocated(&s->client_contexts_lru, client_context_compare);
        if (r < 0)
                return r;

        c = new0(ClientContext, 1);
        if (!c)
                return -ENOMEM;

        c->pid = pid;

        c->uid = UID_INVALID;
        c->gid = GID_INVALID;
        c->auditid = AUDIT_SESSION_INVALID;
        c->loginuid = UID_INVALID;
        c->owner_uid = UID_INVALID;
        c->lru_index = PRIOQ_IDX_NULL;
        c->timestamp = USEC_INFINITY;

        r = hashmap_put(s->client_contexts, PID_TO_PTR(pid), c);
        if (r < 0) {
                free(c);
                return r;
        }

        *ret = c;
        return 0;
}

static void client_context_reset(ClientContext *c) {
        assert(c);

        c->timestamp = USEC_INFINITY;

        c->uid = UID_INVALID;
        c->gid = GID_INVALID;

        c->comm = mfree(c->comm);
        c->exe = mfree(c->exe);
        c->cmdline = mfree(c->cmdline);
        c->capeff = mfree(c->capeff);

        c->auditid = AUDIT_SESSION_INVALID;
        c->loginuid = UID_INVALID;

        c->cgroup = mfree(c->cgroup);
        c->session = mfree(c->session);
        c->owner_uid = UID_INVALID;
        c->unit = mfree(c->unit);
        c->user_unit = mfree(c->user_unit);
        c->slice = mfree(c->slice);
        c->user_slice = mfree(c->user_slice);

        c->invocation_id = SD_ID128_NULL;

        c->label = mfree(c->label);
        c->label_size = 0;
}

static ClientContext* client_context_free(Server *s, ClientContext *c) {
        assert(s);

        if (!c)
                return NULL;

        assert_se(hashmap_remove(s->client_contexts, PID_TO_PTR(c->pid)) == c);

        if (c->in_lru)
                assert_se(prioq_remove(s->client_contexts_lru, c, &c->lru_index) >= 0);

        client_context_reset(c);

        return mfree(c);
}

static void client_context_read_uid_gid(ClientContext *c, const struct ucred *ucred) {
        assert(c);
        assert(pid_is_valid(c->pid));

        /* The ucred data passed in is always the most current and accurate, if we have any. Use it. */
        if (ucred && uid_is_valid(ucred->uid))
                c->uid = ucred->uid;
        else
                (void) get_process_uid(c->pid, &c->uid);

        if (ucred && gid_is_valid(ucred->gid))
                c->gid = ucred->gid;
        else
                (void) get_process_gid(c->pid, &c->gid);
}

static void client_context_read_basic(ClientContext *c) {
        char *t;

        assert(c);
        assert(pid_is_valid(c->pid));

        if (get_process_comm(c->pid, &t) >= 0)
                free_and_replace(c->comm, t);

        if (get_process_exe(c->pid, &t) >= 0)
                free_and_replace(c->exe, t);

        if (get_process_cmdline(c->pid, 0, false, &t) >= 0)
                free_and_replace(c->cmdline, t);

        if (get_process_capeff(c->pid, &t) >= 0)
                free_and_replace(c->capeff, t);
}

static int client_context_read_label(
                ClientContext *c,
                const char *label, size_t label_size) {

        assert(c);
        assert(pid_is_valid(c->pid));
        assert(label_size == 0 || label);

        if (label_size > 0) {
                char *l;

                /* If we got an SELinux label passed in it counts. */

                l = newdup_suffix0(char, label, label_size);
                if (!l)
                        return -ENOMEM;

                free_and_replace(c->label, l);
                c->label_size = label_size;
        }
#if HAVE_SELINUX
        else {
                char *con;

                /* If we got no SELinux label passed in, let's try to acquire one */

                if (getpidcon(c->pid, &con) >= 0) {
                        free_and_replace(c->label, con);
                        c->label_size = strlen(c->label);
                }
        }
#endif

        return 0;
}

static int client_context_read_cgroup(Server *s, ClientContext *c, const char *unit_id) {
        char *t = NULL;
        int r;

        assert(c);

        /* Try to acquire the current cgroup path */
        r = cg_pid_get_path_shifted(c->pid, s->cgroup_root, &t);
        if (r < 0) {

                /* If that didn't work, we use the unit ID passed in as fallback, if we have nothing cached yet */
                if (unit_id && !c->unit) {
                        c->unit = strdup(unit_id);
                        if (c->unit)
                                return 0;
                }

                return r;
        }

        /* Let's shortcut this if the cgroup path didn't change */
        if (streq_ptr(c->cgroup, t)) {
                free(t);
                return 0;
        }

        free_and_replace(c->cgroup, t);

        (void) cg_path_get_session(c->cgroup, &t);
        free_and_replace(c->session, t);

        if (cg_path_get_owner_uid(c->cgroup, &c->owner_uid) < 0)
                c->owner_uid = UID_INVALID;

        (void) cg_path_get_unit(c->cgroup, &t);
        free_and_replace(c->unit, t);

        (void) cg_path_get_user_unit(c->cgroup, &t);
        free_and_replace(c->user_unit, t);

        (void) cg_path_get_slice(c->cgroup, &t);
        free_and_replace(c->slice, t);

        (void) cg_path_get_user_slice(c->cgroup, &t);
        free_and_replace(c->user_slice, t);

        return 0;
}

static int client_context_read_invocation_id(
                Server *s,
                ClientContext *c) {

        _cleanup_free_ char *escaped = NULL, *slice_path = NULL;
        char ids[SD_ID128_STRING_MAX];
        const char *p;
        int r;

        assert(s);
        assert(c);

        /* Read the invocation ID of a unit off a unit. It's stored in the "trusted.invocation_id" extended attribute
         * on the cgroup path. */

        if (!c->unit || !c->slice)
                return 0;

        r = cg_slice_to_path(c->slice, &slice_path);
        if (r < 0)
                return r;

        escaped = cg_escape(c->unit);
        if (!escaped)
                return -ENOMEM;

        p = strjoina(s->cgroup_root, "/", slice_path, "/", escaped);
        if (!p)
                return -ENOMEM;

        r = cg_get_xattr(SYSTEMD_CGROUP_CONTROLLER, p, "trusted.invocation_id", ids, 32);
        if (r < 0)
                return r;
        if (r != 32)
                return -EINVAL;
        ids[32] = 0;

        return sd_id128_from_string(ids, &c->invocation_id);
}

static void client_context_really_refresh(
                Server *s,
                ClientContext *c,
                const struct ucred *ucred,
                const char *label, size_t label_size,
                const char *unit_id,
                usec_t timestamp) {

        assert(s);
        assert(c);
        assert(pid_is_valid(c->pid));

        if (timestamp == USEC_INFINITY)
                timestamp = now(CLOCK_MONOTONIC);

        client_context_read_uid_gid(c, ucred);
        client_context_read_basic(c);
        (void) client_context_read_label(c, label, label_size);

        (void) audit_session_from_pid(c->pid, &c->auditid);
        (void) audit_loginuid_from_pid(c->pid, &c->loginuid);

        (void) client_context_read_cgroup(s, c, unit_id);
        (void) client_context_read_invocation_id(s, c);

        c->timestamp = timestamp;

        if (c->in_lru) {
                assert(c->n_ref == 0);
                assert_se(prioq_reshuffle(s->client_contexts_lru, c, &c->lru_index) >= 0);
        }
}

void client_context_maybe_refresh(
                Server *s,
                ClientContext *c,
                const struct ucred *ucred,
                const char *label, size_t label_size,
                const char *unit_id,
                usec_t timestamp) {

        assert(s);
        assert(c);

        if (timestamp == USEC_INFINITY)
                timestamp = now(CLOCK_MONOTONIC);

        /* No cached data so far? Let's fill it up */
        if (c->timestamp == USEC_INFINITY)
                goto refresh;

        /* If the data isn't pinned and if the cashed data is older than the upper limit, we flush it out
         * entirely. This follows the logic that as long as an entry is pinned the PID reuse is unlikely. */
        if (c->n_ref == 0 && c->timestamp + MAX_USEC < timestamp) {
                client_context_reset(c);
                goto refresh;
        }

        /* If the data is older than the lower limit, we refresh, but keep the old data for all we can't update */
        if (c->timestamp + REFRESH_USEC < timestamp)
                goto refresh;

        /* If the data passed along doesn't match the cached data we also do a refresh */
        if (ucred && uid_is_valid(ucred->uid) && c->uid != ucred->uid)
                goto refresh;

        if (ucred && gid_is_valid(ucred->gid) && c->gid != ucred->gid)
                goto refresh;

        if (label_size > 0 && (label_size != c->label_size || memcmp(label, c->label, label_size) != 0))
                goto refresh;

        return;

refresh:
        client_context_really_refresh(s, c, ucred, label, label_size, unit_id, timestamp);
}

static void client_context_try_shrink_to(Server *s, size_t limit) {
        assert(s);

        /* Bring the number of cache entries below the indicated limit, so that we can create a new entry without
         * breaching the limit. Note that we only flush out entries that aren't pinned here. This means the number of
         * cache entries may very well grow beyond the limit, if all entries stored remain pinned. */

        while (hashmap_size(s->client_contexts) > limit) {
                ClientContext *c;

                c = prioq_pop(s->client_contexts_lru);
                if (!c)
                        break; /* All remaining entries are pinned, give up */

                assert(c->in_lru);
                assert(c->n_ref == 0);

                c->in_lru = false;

                client_context_free(s, c);
        }
}

void client_context_flush_all(Server *s) {
        assert(s);

        /* Flush out all remaining entries. This assumes all references are already dropped. */

        s->my_context = client_context_release(s, s->my_context);
        s->pid1_context = client_context_release(s, s->pid1_context);

        client_context_try_shrink_to(s, 0);

        assert(prioq_size(s->client_contexts_lru) == 0);
        assert(hashmap_size(s->client_contexts) == 0);

        s->client_contexts_lru = prioq_free(s->client_contexts_lru);
        s->client_contexts = hashmap_free(s->client_contexts);
}

static int client_context_get_internal(
                Server *s,
                pid_t pid,
                const struct ucred *ucred,
                const char *label, size_t label_len,
                const char *unit_id,
                bool add_ref,
                ClientContext **ret) {

        ClientContext *c;
        int r;

        assert(s);
        assert(ret);

        if (!pid_is_valid(pid))
                return -EINVAL;

        c = hashmap_get(s->client_contexts, PID_TO_PTR(pid));
        if (c) {

                if (add_ref) {
                        if (c->in_lru) {
                                /* The entry wasn't pinned so far, let's remove it from the LRU list then */
                                assert(c->n_ref == 0);
                                assert_se(prioq_remove(s->client_contexts_lru, c, &c->lru_index) >= 0);
                                c->in_lru = false;
                        }

                        c->n_ref++;
                }

                client_context_maybe_refresh(s, c, ucred, label, label_len, unit_id, USEC_INFINITY);

                *ret = c;
                return 0;
        }

        client_context_try_shrink_to(s, CACHE_MAX-1);

        r = client_context_new(s, pid, &c);
        if (r < 0)
                return r;

        if (add_ref)
                c->n_ref++;
        else {
                r = prioq_put(s->client_contexts_lru, c, &c->lru_index);
                if (r < 0) {
                        client_context_free(s, c);
                        return r;
                }

                c->in_lru = true;
        }

        client_context_really_refresh(s, c, ucred, label, label_len, unit_id, USEC_INFINITY);

        *ret = c;
        return 0;
}

int client_context_get(
                Server *s,
                pid_t pid,
                const struct ucred *ucred,
                const char *label, size_t label_len,
                const char *unit_id,
                ClientContext **ret) {

        return client_context_get_internal(s, pid, ucred, label, label_len, unit_id, false, ret);
}

int client_context_acquire(
                Server *s,
                pid_t pid,
                const struct ucred *ucred,
                const char *label, size_t label_len,
                const char *unit_id,
                ClientContext **ret) {

        return client_context_get_internal(s, pid, ucred, label, label_len, unit_id, true, ret);
};

ClientContext *client_context_release(Server *s, ClientContext *c) {
        assert(s);

        if (!c)
                return NULL;

        assert(c->n_ref > 0);
        assert(!c->in_lru);

        c->n_ref--;
        if (c->n_ref > 0)
                return NULL;

        /* The entry is not pinned anymore, let's add it to the LRU prioq if we can. If we can't we'll drop it
         * right-away */

        if (prioq_put(s->client_contexts_lru, c, &c->lru_index) < 0)
                client_context_free(s, c);
        else
                c->in_lru = true;

        return NULL;
}

void client_context_acquire_default(Server *s) {
        int r;

        assert(s);

        /* Ensure that our own and PID1's contexts are always pinned. Our own context is particularly useful to
         * generate driver messages. */

        if (!s->my_context) {
                struct ucred ucred = {
                        .pid = getpid_cached(),
                        .uid = getuid(),
                        .gid = getgid(),
                };

                r = client_context_acquire(s, ucred.pid, &ucred, NULL, 0, NULL, &s->my_context);
                if (r < 0)
                        log_warning_errno(r, "Failed to acquire our own context, ignoring: %m");
        }

        if (!s->pid1_context) {

                r = client_context_acquire(s, 1, NULL, NULL, 0, NULL, &s->pid1_context);
                if (r < 0)
                        log_warning_errno(r, "Failed to acquire PID1's context, ignoring: %m");

        }
}
