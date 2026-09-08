#include "hhsdr/manet/route_table.h"
#include <string.h>

void hh_route_publisher_init(hh_route_publisher_t *p)
{
    if (!p) return;
    memset(p, 0, sizeof *p);
    p->buffers[0].version = 1;
    atomic_store(&p->current, &p->buffers[0]);
    p->next_buffer = 1;
}

hh_route_snapshot_t *hh_route_publisher_begin(hh_route_publisher_t *p)
{
    const hh_route_snapshot_t *cur;
    hh_route_snapshot_t *draft;

    if (!p) return NULL;
    cur   = atomic_load(&p->current);
    draft = &p->buffers[p->next_buffer];
    /* Start from the current contents so a commit is a delta, not a rebuild. */
    memcpy(draft, cur, sizeof *draft);
    draft->version = cur->version + 1;
    return draft;
}

void hh_route_publisher_commit(hh_route_publisher_t *p, hh_route_snapshot_t *draft)
{
    if (!p || !draft) return;
    /* The entire publish is one atomic pointer store: the fast path never
     * observes a partially-built table and never blocks. */
    atomic_store(&p->current, draft);
    p->next_buffer ^= 1u;
}

const hh_route_snapshot_t *hh_route_publisher_current(const hh_route_publisher_t *p)
{
    if (!p) return NULL;
    return atomic_load(&p->current);
}

const hh_route_entry_t *hh_route_lookup(const hh_route_snapshot_t *s, hh_node_id_t dst)
{
    if (!s) return NULL;
    /* Flat scan of the next-hop table: no graph walk on the forwarding path. */
    for (size_t i = 0; i < s->count; i++) {
        const hh_route_entry_t *e = &s->entries[i];
        if (e->destination == dst && e->valid) return e;
    }
    return NULL;
}
