/* Copyright (c) 2015, 2016 Nicira, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at:
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <config.h>

#include "lib/sset.h"
#include "lport.h"
#include "hash.h"
#include "openvswitch/vlog.h"
#include "ovn/lib/ovn-sb-idl.h"
VLOG_DEFINE_THIS_MODULE(lport);

static struct ldatapath *ldatapath_lookup_by_key__(
    const struct ldatapath_index *, uint32_t dp_key);

void
ldatapath_index_init(struct ldatapath_index *ldatapaths,
                     struct ovsdb_idl *ovnsb_idl)
{
    hmap_init(&ldatapaths->by_key);

    const struct sbrec_port_binding *pb;
    SBREC_PORT_BINDING_FOR_EACH (pb, ovnsb_idl) {
        if (!pb->datapath) {
            continue;
        }
        uint32_t dp_key = pb->datapath->tunnel_key;
        struct ldatapath *ld = ldatapath_lookup_by_key__(ldatapaths, dp_key);
        if (!ld) {
            ld = xzalloc(sizeof *ld);
            hmap_insert(&ldatapaths->by_key, &ld->by_key_node, dp_key);
            ld->db = pb->datapath;
        }

        if (ld->n_lports >= ld->allocated_lports) {
            ld->lports = x2nrealloc(ld->lports, &ld->allocated_lports,
                                    sizeof *ld->lports);
        }
        ld->lports[ld->n_lports++] = pb;
    }
}

void
ldatapath_index_destroy(struct ldatapath_index *ldatapaths)
{
    if (!ldatapaths) {
        return;
    }

    struct ldatapath *ld, *ld_next;
    HMAP_FOR_EACH_SAFE (ld, ld_next, by_key_node, &ldatapaths->by_key) {
        hmap_remove(&ldatapaths->by_key, &ld->by_key_node);
        free(ld->lports);
        free(ld);
    }
    hmap_destroy(&ldatapaths->by_key);
}

static struct ldatapath *ldatapath_lookup_by_key__(
    const struct ldatapath_index *ldatapaths, uint32_t dp_key)
{
    struct ldatapath *ld;
    HMAP_FOR_EACH_WITH_HASH (ld, by_key_node, dp_key, &ldatapaths->by_key) {
        return ld;
    }
    return NULL;
}

const struct ldatapath *ldatapath_lookup_by_key(
    const struct ldatapath_index *ldatapaths, uint32_t dp_key)
{
    return ldatapath_lookup_by_key__(ldatapaths, dp_key);
}

/* A logical port. */
struct lport {
    struct hmap_node name_node; /* Index by name. */
    struct hmap_node key_node;  /* Index by (dp_key, port_key). */
    const struct sbrec_port_binding *pb;
};

void
lport_index_init(struct lport_index *lports, struct ovsdb_idl *ovnsb_idl)
{
    hmap_init(&lports->by_name);
    hmap_init(&lports->by_key);

    const struct sbrec_port_binding *pb;
    SBREC_PORT_BINDING_FOR_EACH (pb, ovnsb_idl) {
        if (!pb->datapath) {
            continue;
        }

        if (lport_lookup_by_name(lports, pb->logical_port)) {
            static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(1, 1);
            VLOG_WARN_RL(&rl, "duplicate logical port name '%s'",
                         pb->logical_port);
            continue;
        }
        if (lport_lookup_by_key(lports, pb->datapath->tunnel_key,
                                pb->tunnel_key)) {
            static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(1, 1);
            VLOG_WARN_RL(&rl, "duplicate logical port %"PRId64" in logical "
                         "datapath %"PRId64,
                         pb->tunnel_key, pb->datapath->tunnel_key);
            continue;
        }

        struct lport *p = xmalloc(sizeof *p);
        hmap_insert(&lports->by_name, &p->name_node,
                    hash_string(pb->logical_port, 0));
        hmap_insert(&lports->by_key, &p->key_node,
                    hash_int(pb->tunnel_key, pb->datapath->tunnel_key));
        p->pb = pb;
    }
}

void
lport_index_destroy(struct lport_index *lports)
{
    if (!lports) {
        return;
    }

    /* Destroy all of the "struct lport"s.
     *
     * We don't have to remove the node from both indexes. */
    struct lport *port, *next;
    HMAP_FOR_EACH_SAFE (port, next, name_node, &lports->by_name) {
        hmap_remove(&lports->by_name, &port->name_node);
        free(port);
    }

    hmap_destroy(&lports->by_name);
    hmap_destroy(&lports->by_key);
}

/* Finds and returns the lport with the given 'name', or NULL if no such lport
 * exists. */
const struct sbrec_port_binding *
lport_lookup_by_name(const struct lport_index *lports, const char *name)
{
    const struct lport *lport;
    HMAP_FOR_EACH_WITH_HASH (lport, name_node, hash_string(name, 0),
                             &lports->by_name) {
        if (!strcmp(lport->pb->logical_port, name)) {
            return lport->pb;
        }
    }
    return NULL;
}

const struct sbrec_port_binding *
lport_lookup_by_key(const struct lport_index *lports,
                    uint32_t dp_key, uint16_t port_key)
{
    const struct lport *lport;
    HMAP_FOR_EACH_WITH_HASH (lport, key_node, hash_int(port_key, dp_key),
                             &lports->by_key) {
        if (port_key == lport->pb->tunnel_key
            && dp_key == lport->pb->datapath->tunnel_key) {
            return lport->pb;
        }
    }
    return NULL;
}

struct mcgroup {
    struct hmap_node dp_name_node; /* Index by (logical datapath, name). */
    const struct sbrec_multicast_group *mg;
};

void
mcgroup_index_init(struct mcgroup_index *mcgroups, struct ovsdb_idl *ovnsb_idl)
{
    hmap_init(&mcgroups->by_dp_name);

    const struct sbrec_multicast_group *mg;
    SBREC_MULTICAST_GROUP_FOR_EACH (mg, ovnsb_idl) {
        const struct uuid *dp_uuid = &mg->datapath->header_.uuid;
        if (mcgroup_lookup_by_dp_name(mcgroups, mg->datapath, mg->name)) {
            static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(1, 1);
            VLOG_WARN_RL(&rl, "datapath "UUID_FMT" contains duplicate "
                         "multicast group '%s'", UUID_ARGS(dp_uuid), mg->name);
            continue;
        }

        struct mcgroup *m = xmalloc(sizeof *m);
        hmap_insert(&mcgroups->by_dp_name, &m->dp_name_node,
                    hash_string(mg->name, uuid_hash(dp_uuid)));
        m->mg = mg;
    }
}

void
mcgroup_index_destroy(struct mcgroup_index *mcgroups)
{
    if (!mcgroups) {
        return;
    }

    struct mcgroup *mcgroup, *next;
    HMAP_FOR_EACH_SAFE (mcgroup, next, dp_name_node, &mcgroups->by_dp_name) {
        hmap_remove(&mcgroups->by_dp_name, &mcgroup->dp_name_node);
        free(mcgroup);
    }

    hmap_destroy(&mcgroups->by_dp_name);
}

const struct sbrec_multicast_group *
mcgroup_lookup_by_dp_name(const struct mcgroup_index *mcgroups,
                          const struct sbrec_datapath_binding *dp,
                          const char *name)
{
    const struct uuid *dp_uuid = &dp->header_.uuid;
    const struct mcgroup *mcgroup;
    HMAP_FOR_EACH_WITH_HASH (mcgroup, dp_name_node,
                             hash_string(name, uuid_hash(dp_uuid)),
                             &mcgroups->by_dp_name) {
        if (uuid_equals(&mcgroup->mg->datapath->header_.uuid, dp_uuid)
            && !strcmp(mcgroup->mg->name, name)) {
            return mcgroup->mg;
        }
    }
    return NULL;
}


/* redirect-chassis option parsing
 */
static int
compare_chassis_prio_(const void *a_, const void *b_)
{
    const struct redirect_chassis *chassis_a = a_;
    const struct redirect_chassis *chassis_b = b_;
    int prio_diff = chassis_b->prio - chassis_a->prio;
    if (!prio_diff) {
        return strcmp(chassis_a->chassis_id, chassis_b->chassis_id);
    }
    return prio_diff;
}

struct ovs_list*
parse_redirect_chassis(const struct sbrec_port_binding *binding)
{

    const char *redir_chassis_const;
    char *redir_chassis_str;
    char *save_ptr1 = NULL;
    char *chassis_prio;

    struct redirect_chassis *redirect_chassis =
        xmalloc(sizeof *redirect_chassis);

    int n=0;

    redir_chassis_const = smap_get(&binding->options, "redirect-chassis");

    if (!redir_chassis_const) {
        free(redirect_chassis);
        return NULL;
    }

    redir_chassis_str = strdup(redir_chassis_const);

    for (chassis_prio = strtok_r(redir_chassis_str, ", ", &save_ptr1);
         chassis_prio; chassis_prio = strtok_r(NULL, ", ", &save_ptr1)) {

        char *save_ptr2 = NULL;
        char *chassis_name = strtok_r(chassis_prio, ":", &save_ptr2);
        char *prio = strtok_r(NULL, ":", &save_ptr2);

        if (strlen(chassis_name) > UUID_LEN) {
            static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(1, 1);
            VLOG_WARN_RL(&rl, "chassis name (%s) in redirect-chassis option "
                              "of logical port %s is too long, ignoring.",
                              chassis_name, binding->logical_port);
            continue;
        }

        strcpy(redirect_chassis[n].chassis_id, chassis_name);

        /* chassis with no priority get lowest priority: 0 */
        redirect_chassis[n].prio = prio ? atoi(prio):0;

	    redirect_chassis = xrealloc(redirect_chassis,
				                    sizeof *redirect_chassis * (++n + 1));
    }

    free(redir_chassis_str);

    qsort(redirect_chassis, n, sizeof *redirect_chassis,
          compare_chassis_prio_);

    struct ovs_list *list = NULL;
    if (n) {
        list = xmalloc(sizeof *list);
        ovs_list_init(list);

        int i;
        for (i=0; i<n; i++) {
            ovs_list_push_back(list, &redirect_chassis[i].node);
        }
    }

    return list;
}

bool
redirect_chassis_contains(struct ovs_list *redirect_chassis,
                          const struct sbrec_chassis *chassis) {
    struct redirect_chassis *chassis_item;
    if (redirect_chassis) {
        LIST_FOR_EACH(chassis_item, node, redirect_chassis) {
            if (!strcmp(chassis_item->chassis_id, chassis->name)) {
                return true;
            }
        }
    }
    return false;
}

void
redirect_chassis_destroy(struct ovs_list *list)
{
    if (!list) {
        return;
    }
    free(ovs_list_front(list));
    free(list);
}

bool
pb_redirect_chassis_contains(const struct sbrec_port_binding *binding,
                             const struct sbrec_chassis *chassis)
{
    bool contained;
    struct ovs_list *redirect_chassis = parse_redirect_chassis(binding);
    contained = redirect_chassis_contains(redirect_chassis, chassis);
    redirect_chassis_destroy(redirect_chassis);
    return contained;
}

bool
redirect_chassis_is_active(const struct ovs_list *redirect_chassis,
                           const struct sbrec_chassis *local_chassis,
                           const struct sset *active_tunnels)
{
    struct redirect_chassis *rc;

    /* if there's only one chassis, it's not HA and it's the equivalent
     * of being active */
    if(ovs_list_is_short(redirect_chassis)) {
        return true;
    }

    /* if there are no other tunnels active, we assume that the
     * connection providing tunneling is down, hence we're down */
    if(sset_is_empty(active_tunnels)) {
        return false;
    }

    /* redirect_chassis is an ordered list, by priority, of chassis
     * hosting the redirect of the port */
    LIST_FOR_EACH(rc, node, redirect_chassis) {
        /* if we found the chassis on the list, and we didn't exit before
         * on the active_tunnels check for other higher priority chassis
         * being active, then this chassis is master.
         * This will also be true for single-chassis (non-HA) redirects */
        if(!strcmp(rc->chassis_id, local_chassis->name)) {
            return true;
        }
        /* if we find this specific chassis on the list to have an active
         * tunnel, then 'local_chassis' is not master */
        if(sset_contains(active_tunnels, rc->chassis_id)) {
            return false;
        }
    }
    return false;
}
