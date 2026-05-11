/*
 * Copyright (c) 2023, Red Hat, Inc.
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

#include "openvswitch/vlog.h"

#include "en-meters.h"
#include "hash.h"

VLOG_DEFINE_THIS_MODULE(en_meters);

static void build_meter_groups(struct shash *meter_group,
                               const struct nbrec_meter_table *);
static void sync_meters(struct ovsdb_idl_txn *ovnsb_txn,
                        const struct nbrec_meter_table *,
                        const struct nbrec_acl_table *,
                        const struct sbrec_meter_table *,
                        struct shash *meter_groups);

void
*en_sync_meters_init(struct engine_node *node OVS_UNUSED,
                     struct engine_arg *arg OVS_UNUSED)
{
    struct sync_meters_data *data = xmalloc(sizeof *data);

    *data = (struct sync_meters_data) {
        .meter_groups = SHASH_INITIALIZER(&data->meter_groups),
    };
    return data;
}

void
en_sync_meters_cleanup(void *data_)
{
    struct sync_meters_data *data = data_;

    shash_destroy(&data->meter_groups);
}

enum engine_node_state
en_sync_meters_run(struct engine_node *node, void *data_)
{
    struct sync_meters_data *data = data_;

    const struct nbrec_acl_table *acl_table =
        EN_OVSDB_GET(engine_get_input("NB_acl", node));

    const struct nbrec_meter_table *nb_meter_table =
        EN_OVSDB_GET(engine_get_input("NB_meter", node));

    const struct sbrec_meter_table *sb_meter_table =
        EN_OVSDB_GET(engine_get_input("SB_meter", node));

    const struct engine_context *eng_ctx = engine_get_context();

    build_meter_groups(&data->meter_groups, nb_meter_table);

    sync_meters(eng_ctx->ovnsb_idl_txn, nb_meter_table, acl_table,
                sb_meter_table, &data->meter_groups);
    return EN_UPDATED;
}

enum engine_input_handler_result
sync_meters_nb_acl_handler(struct engine_node *node, void *data OVS_UNUSED)
{
    const struct nbrec_acl_table *acl_table =
        EN_OVSDB_GET(engine_get_input("NB_acl", node));

    const struct nbrec_acl *nb_acl;
    NBREC_ACL_TABLE_FOR_EACH_TRACKED (nb_acl, acl_table) {
        /* New or deleted ACL with meter needs to be recomputed. */
        if ((nbrec_acl_is_new(nb_acl) || nbrec_acl_is_deleted(nb_acl)) &&
            (nb_acl->log || nb_acl->meter)) {
            return EN_UNHANDLED;
        }

        /* Addition or removal of meter requires recompute. */
        if (nbrec_acl_is_updated(nb_acl, NBREC_ACL_COL_LOG) ||
            nbrec_acl_is_updated(nb_acl, NBREC_ACL_COL_METER)) {
            return EN_UNHANDLED;
        }
    }

    return EN_HANDLED_UNCHANGED;
}

const struct nbrec_meter*
fair_meter_lookup_by_name(const struct shash *meter_groups,
                          const char *meter_name)
{
    const struct nbrec_meter *nb_meter =
        meter_name ? shash_find_data(meter_groups, meter_name) : NULL;
    if (nb_meter) {
        return (nb_meter->fair && *nb_meter->fair) ? nb_meter : NULL;
    }
    return NULL;
}

struct band_entry {
    int64_t rate;
    int64_t burst_size;
    const char *action;
};

struct band_info {
    struct hmap_node node;
    int rate;
    int burst_size;
    const char *action;
};

static struct band_info *
band_info_find(struct hmap *bands, uint32_t rate, uint32_t burst_size,
               const char *action, uint32_t hash)
{
    struct band_info *b;
    HMAP_FOR_EACH_WITH_HASH (b, node, hash, bands) {
        if (b->rate == rate && b->burst_size == burst_size &&
            !strcmp(b->action, action)) {
            return b;
        }
    }
    return NULL;
}

static bool
bands_need_update(const struct nbrec_meter *nb_meter,
                  const struct sbrec_meter *sb_meter)
{
    if (nb_meter->n_bands != sb_meter->n_bands) {
        return true;
    }
    struct hmap band_hmap = HMAP_INITIALIZER(&band_hmap);

    struct band_info *sb_bands;
    sb_bands = xmalloc(sizeof *sb_bands * sb_meter->n_bands);
    for (size_t i = 0; i < sb_meter->n_bands; i++) {
        struct sbrec_meter_band *sb_band = sb_meter->bands[i];
        sb_band[i].rate = sb_band->rate;
        sb_band[i].burst_size = sb_band->burst_size;
        sb_band[i].action = sb_band->action;

        uint32_t basis = hash_int(sb_band->rate, 0);
        basis = hash_int(sb_band->burst_size, basis);
        basis = hash_string(sb_band->action, basis);
        hmap_insert(&band_hmap, &sb_bands[i].node, basis);
    }

    /* Place the Northbound entries in sorted order. */
    bool need_update = false;
    for (size_t i = 0; i < nb_meter->n_bands; i++) {
        struct nbrec_meter_band *nb_band = nb_meter->bands[i];

        uint32_t basis = hash_int(nb_band->rate, 0);
        basis = hash_int(nb_band->burst_size, basis);
        basis = hash_string(nb_band->action, basis);

        struct band_info *band =
            band_info_find(&band_hmap, nb_band->rate, nb_band->burst_size,
                           nb_band->action, basis);
        if (band) {
            hmap_remove(&band_hmap, &band->node);
        } else {
            need_update = true;
            break;
        }
    }

    free(sb_bands);

    return need_update;
}
static void
sync_meters_iterate_nb_meter(struct ovsdb_idl_txn *ovnsb_txn,
                             const char *meter_name,
                             const struct nbrec_meter *nb_meter,
                             struct shash *sb_meters,
                             struct sset *used_sb_meters)
{
    const struct sbrec_meter *sb_meter;
    bool new_sb_meter = false;

    sb_meter = shash_find_data(sb_meters, meter_name);
    if (!sb_meter) {
        sb_meter = sbrec_meter_insert(ovnsb_txn);
        sbrec_meter_set_name(sb_meter, meter_name);
        shash_add(sb_meters, sb_meter->name, sb_meter);
        new_sb_meter = true;
    }
    sset_add(used_sb_meters, meter_name);

    if (new_sb_meter || bands_need_update(nb_meter, sb_meter)) {
        struct sbrec_meter_band **sb_bands;
        sb_bands = xcalloc(nb_meter->n_bands, sizeof *sb_bands);
        for (size_t i = 0; i < nb_meter->n_bands; i++) {
            const struct nbrec_meter_band *nb_band = nb_meter->bands[i];

            sb_bands[i] = sbrec_meter_band_insert(ovnsb_txn);

            sbrec_meter_band_set_action(sb_bands[i], nb_band->action);
            sbrec_meter_band_set_rate(sb_bands[i], nb_band->rate);
            sbrec_meter_band_set_burst_size(sb_bands[i],
                                            nb_band->burst_size);
        }
        sbrec_meter_set_bands(sb_meter, sb_bands, nb_meter->n_bands);
        free(sb_bands);
    }

    sbrec_meter_set_unit(sb_meter, nb_meter->unit);
}

static void
sync_acl_fair_meter(struct ovsdb_idl_txn *ovnsb_txn,
                    struct shash *meter_groups,
                    const struct nbrec_acl *acl, struct shash *sb_meters,
                    struct sset *used_sb_meters)
{
    const struct nbrec_meter *nb_meter;

    if (!acl->log || !acl->meter) {
        return;
    }

    nb_meter = fair_meter_lookup_by_name(meter_groups, acl->meter);
    if (!nb_meter) {
        return;
    }

    char *meter_name = alloc_acl_log_unique_meter_name(acl);
    sync_meters_iterate_nb_meter(ovnsb_txn, meter_name, nb_meter, sb_meters,
                                 used_sb_meters);
    free(meter_name);
}

static void
build_meter_groups(struct shash *meter_groups,
                   const struct nbrec_meter_table *nb_meter_table)
{
    const struct nbrec_meter *nb_meter;

    shash_clear(meter_groups);
    NBREC_METER_TABLE_FOR_EACH (nb_meter, nb_meter_table) {
        shash_add(meter_groups, nb_meter->name, nb_meter);
    }
}

/* Each entry in the Meter and Meter_Band tables in OVN_Northbound have
 * a corresponding entries in the Meter and Meter_Band tables in
 * OVN_Southbound. Additionally, ACL logs that use fair meters have
 * a private copy of its meter in the SB table.
 */
static void
sync_meters(struct ovsdb_idl_txn *ovnsb_txn,
            const struct nbrec_meter_table *nbrec_meter_table,
            const struct nbrec_acl_table *nbrec_acl_table,
            const struct sbrec_meter_table *sbrec_meter_table,
            struct shash *meter_groups)
{
    struct shash sb_meters = SHASH_INITIALIZER(&sb_meters);
    struct sset used_sb_meters = SSET_INITIALIZER(&used_sb_meters);

    const struct sbrec_meter *sb_meter;
    SBREC_METER_TABLE_FOR_EACH (sb_meter, sbrec_meter_table) {
        shash_add(&sb_meters, sb_meter->name, sb_meter);
    }

    const struct nbrec_meter *nb_meter;
    NBREC_METER_TABLE_FOR_EACH (nb_meter, nbrec_meter_table) {
        sync_meters_iterate_nb_meter(ovnsb_txn, nb_meter->name, nb_meter,
                                     &sb_meters, &used_sb_meters);
    }

    /*
     * In addition to creating Meters in the SB from the block above, check
     * and see if additional rows are needed to get ACLs logs individually
     * rate-limited.
     */
    const struct nbrec_acl *acl;
    NBREC_ACL_TABLE_FOR_EACH (acl, nbrec_acl_table) {
        sync_acl_fair_meter(ovnsb_txn, meter_groups, acl,
                            &sb_meters, &used_sb_meters);
    }

    const char *used_meter;
    SSET_FOR_EACH_SAFE (used_meter, &used_sb_meters) {
        shash_find_and_delete(&sb_meters, used_meter);
        sset_delete(&used_sb_meters, SSET_NODE_FROM_NAME(used_meter));
    }
    sset_destroy(&used_sb_meters);

    struct shash_node *node;
    SHASH_FOR_EACH_SAFE (node, &sb_meters) {
        sbrec_meter_delete(node->data);
        shash_delete(&sb_meters, node);
    }
    shash_destroy(&sb_meters);
}
