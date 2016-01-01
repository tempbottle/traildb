
#include <fcntl.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>

#include "tdb_internal.h"
#include "tdb_encode_model.h"
#include "tdb_huffman.h"
#include "util.h"

#define DSFMT_MEXP 521
#include "dsfmt/dSFMT.h"

#define SAMPLE_SIZE (0.1 * RAND_MAX)
#define RANDOM_SEED 238713
#define UNIGRAM_SUPPORT 0.00001
#define NUM_EVENTS_SAMPLING_THRESHOLD 1000000
#define INITIAL_GRAM_BUF_LEN 256

#define MIN(a,b) ((a)>(b)?(b):(a))

/* event op handles one *event* (not one trail) */
typedef void *(*event_op)(const tdb_item *encoded,
                          uint64_t n,
                          const tdb_event *ev,
                          void *state);

struct ngram_state{
    Pvoid_t candidates;
    struct judy_128_map ngram_freqs;
    Pvoid_t final_freqs;
    __uint128_t *grams;
    struct gram_bufs gbufs;
};

static double get_sample_size(void)
{
    /* TODO remove this env var */
    double d = 0.1;
    if (getenv("TDB_SAMPLE_SIZE")){
        char *endptr;
        d = strtod(getenv("TDB_SAMPLE_SIZE"), &endptr);
        if (*endptr || d < 0.01 || d > 1.0)
            DIE("Invalid TDB_SAMPLE_SIZE");
    }
    return d;
}

static void *event_fold(event_op op,
                        FILE *grouped,
                        uint64_t num_events,
                        const tdb_item *items,
                        uint64_t num_fields,
                        void *state)
{
    tdb_item *prev_items = NULL;
    tdb_item *encoded = NULL;
    uint64_t encoded_size = 0;
    uint64_t i = 1;
    double sample_size = 1.0;
    tdb_event ev;

    dsfmt_t rand_state;
    dsfmt_init_gen_rand(&rand_state, RANDOM_SEED);

    if (num_events == 0)
        return state;

    /* enable sampling only if there is a large number of events */
    if (num_events > NUM_EVENTS_SAMPLING_THRESHOLD)
        sample_size = get_sample_size();

    if (!(prev_items = malloc(num_fields * sizeof(tdb_item))))
        DIE("Couldn't allocate %"PRIu64" items", num_fields);

    rewind(grouped);
    SAFE_FREAD(grouped, "grouped", &ev, sizeof(tdb_event));

    /* this function scans through *all* unencoded data, takes a sample
       of trails, edge-encodes events for a trail, and calls the
       given function (op) for each event */

    while (i <= num_events){
        /* NB: We sample trails, not events, below.
           We can't encode *and* sample events efficiently at the same time.

           If data is very unevenly distributed over trails, sampling trails
           will produce suboptimal results. We could compensate for this by
           always include all very long trails in the sample.
        */
        uint64_t trail_id = ev.trail_id;

        /* Always include the first trail so we don't end up empty */
        if (i == 1 || dsfmt_genrand_close_open(&rand_state) < sample_size){
            memset(prev_items, 0, num_fields * sizeof(tdb_item));

            while (ev.trail_id == trail_id){
                uint64_t n = edge_encode_items(items,
                                               &encoded,
                                               &encoded_size,
                                               prev_items,
                                               &ev);

                state = op(encoded, n, &ev, state);

                if (i++ < num_events){
                    SAFE_FREAD(grouped, "grouped", &ev, sizeof(tdb_event));
                }else
                    break;
            }
        }else{
            /* given that we are sampling trails, we need to skip all events
               related to a trail not included in the sample */
            for (;i < num_events && ev.trail_id == trail_id; i++)
                SAFE_FREAD(grouped, "grouped", &ev, sizeof(tdb_event));
        }
    }

    free(encoded);
    free(prev_items);

    return state;
}

static void alloc_gram_bufs(struct gram_bufs *b)
{
    if (!(b->chosen = malloc(b->buf_len * b->buf_len * 16)))
        DIE("Could not allocate bigram gram_buf (%"PRIu64" bytes)",
            b->buf_len * b->buf_len * 16);

    if (!(b->scores = malloc(b->buf_len * b->buf_len * 8)))
        DIE("Could not allocate scores gram_buf (%"PRIu64" bytes)",
            b->buf_len * b->buf_len * 8);
}

void init_gram_bufs(struct gram_bufs *b, uint64_t num_fields)
{
    if (!(b->covered = malloc(num_fields)))
        DIE("Could not allocate covered gram_buf (%"PRIu64" fields)",
            num_fields);

    b->buf_len = MIN(num_fields, INITIAL_GRAM_BUF_LEN);
    b->num_fields = num_fields;
    alloc_gram_bufs(b);
}

void free_gram_bufs(struct gram_bufs *b)
{
    free(b->chosen);
    free(b->scores);
    free(b->covered);
}

/* given a set of edge-encoded values (encoded), choose a set of unigrams
   and bigrams that cover the original set. In essence, this tries to
   solve Weigted Exact Cover Problem for the universe of 'encoded'. */
uint64_t choose_grams_one_event(const tdb_item *encoded,
                                uint64_t num_encoded,
                                const struct judy_128_map *gram_freqs,
                                struct gram_bufs *g,
                                __uint128_t *grams,
                                const tdb_event *ev)
{
    uint64_t i, j, k, n = 0;
    Word_t *ptr;
    uint64_t unigram1 = ev->timestamp;

    /*
    in the worst case we need O(num_fields^2) of memory but typically
    either num_fields is small or events are sparse, i.e.
    num_encoded << num_fields, so in practice these shouldn't take a
    huge amount of space
    */
    if (g->buf_len < num_encoded * num_encoded){
        free(g->scores);
        free(g->chosen);
        g->buf_len = num_encoded * num_encoded;
        alloc_gram_bufs(g);
    }

    memset(g->covered, 0, g->num_fields);

    /* First, produce all candidate bigrams for this set. */
    for (k = 0, i = 0; i < num_encoded; i++){
        if (i > 0){
            unigram1 = encoded[i];
            j = i + 1;
        }else
            j = 0;

        for (;j < num_encoded; j++){
            __uint128_t bigram = unigram1;
            bigram |= ((__uint128_t)encoded[j]) << 64;
            ptr = j128m_get(gram_freqs, bigram);
            if (ptr){
                g->chosen[k] = bigram;
                g->scores[k++] = *ptr;
            }
        }
    }

    /* timestamp *must* be the first item in the list, add unigram as
       a placeholder - this may get replaced by a bigram below */
    grams[n++] = ev->timestamp;

    /* Pick non-overlapping histograms, in the order of descending score.
       As we go, mark fields covered (consumed) in the set. */
    while (1){
        uint64_t max_idx = 0;
        uint64_t max_score = 0;

        for (i = 0; i < k; i++)
            /* consider only bigrams whose both unigrams are non-covered */
            if (!(g->covered[tdb_item_field(HUFF_BIGRAM_TO_ITEM(g->chosen[i]))] ||
                  g->covered[tdb_item_field(HUFF_BIGRAM_OTHER_ITEM(g->chosen[i]))]) &&
                  g->scores[i] > max_score){

                max_score = g->scores[i];
                max_idx = i;
            }

        if (max_score){
            /* mark both unigrams of this bigram covered */
            __uint128_t chosen = g->chosen[max_idx];
            g->covered[tdb_item_field(HUFF_BIGRAM_TO_ITEM(chosen))] = 1;
            g->covered[tdb_item_field(HUFF_BIGRAM_OTHER_ITEM(chosen))] = 1;
            if (tdb_item_field(HUFF_BIGRAM_TO_ITEM(chosen)))
                grams[n++] = chosen;
            else
                /*
                make sure timestamp stays as the first item.
                This is safe since grams[0] was reserved above for
                the timestamp. */
                grams[0] = chosen;
        }else
            /* all bigrams used */
            break;
    }

    /* Finally, add all remaining unigrams to the result set which have not
       been covered by any bigrams */
    for (i = 0; i < num_encoded; i++)
        if (!g->covered[tdb_item_field(encoded[i])])
            grams[n++] = encoded[i];

    return n;
}

static void *choose_grams(const tdb_item *encoded,
                          uint64_t num_encoded,
                          const tdb_event *ev,
                          void *state){

    struct ngram_state *g = (struct ngram_state *)state;
    uint64_t n = choose_grams_one_event(encoded,
                                        num_encoded,
                                        &g->ngram_freqs,
                                        &g->gbufs,
                                        g->grams,
                                        ev);

    while (n--){
        Word_t *ptr = j128m_insert(g->final_freqs, g->grams[n]);
        ++*ptr;
    }

    return state;
}


static Pvoid_t find_candidates(const Pvoid_t unigram_freqs)
{
    Pvoid_t candidates = NULL;
    Word_t idx = 0;
    Word_t *ptr;
    uint64_t num_values = 0;
    uint64_t support;

    /* find all unigrams whose probability of occurrence is greater than
       UNIGRAM_SUPPORT */

    JLF(ptr, unigram_freqs, idx);
    while (ptr){
        num_values += *ptr;
        JLN(ptr, unigram_freqs, idx);
    }

    support = num_values / (uint64_t)(1.0 / UNIGRAM_SUPPORT);
    idx = 0;

    JLF(ptr, unigram_freqs, idx);
    while (ptr){
        int tmp;
        if (*ptr > support)
            J1S(tmp, candidates, idx);
        JLN(ptr, unigram_freqs, idx);
    }

    return candidates;
}

static void *all_bigrams(const tdb_item *encoded,
                         uint64_t n,
                         const tdb_event *ev,
                         void *state){

    struct ngram_state *g = (struct ngram_state *)state;
    Word_t *ptr;
    int set;
    uint64_t i, j;
    uint64_t unigram1 = ev->timestamp;

    for (i = 0; i < n; i++){
        if (i > 0){
            unigram1 = encoded[i];
            j = i + 1;
        }else
            j = 0;

        J1T(set, g->candidates, unigram1);
        if (set){
            for (; j < n; j++){
                uint64_t unigram2 = encoded[j];
                J1T(set, g->candidates, unigram2);
                if (set){
                    __uint128_t bigram = unigram1;
                    bigram |= ((__uint128_t)unigram2) << 64;
                    ptr = j128m_insert(&g->ngram_freqs, bigram);
                    ++*ptr;
                }
            }
        }
    }

    return state;
}

void make_grams(FILE *grouped,
                uint64_t num_events,
                const tdb_item *items,
                uint64_t num_fields,
                const Pvoid_t unigram_freqs,
                struct judy_128_map *final_freqs)
{
    struct ngram_state g = {.final_freqs = final_freqs};
    Word_t tmp;

    j128m_init(&g.ngram_freqs);
    init_gram_bufs(&g.gbufs, num_fields);

    if (!(g.grams = malloc(num_fields * 16)))
        DIE("Could not allocate grams buf (%"PRIu64" fields)", num_fields);

    /* below is a very simple version of the Apriori algorithm
       for finding frequent sets (bigrams) */

    /* find unigrams that are sufficiently frequent */
    g.candidates = find_candidates(unigram_freqs);

    /* collect frequencies of *all* occurring bigrams of candidate unigrams */
    event_fold(all_bigrams, grouped, num_events, items, num_fields, &g);
    /* collect frequencies of non-overlapping bigrams and unigrams
       (exact covering set for each event), store in final_freqs */
    event_fold(choose_grams, grouped, num_events, items, num_fields, (void *)&g);

    J1FA(tmp, g.candidates);
    j128m_free(&g.ngram_freqs);
    free_gram_bufs(&g.gbufs);
    free(g.grams);
}

static void *all_freqs(const tdb_item *encoded,
                       uint64_t n,
                       const tdb_event *ev,
                       void *state){

    Pvoid_t freqs = (Pvoid_t)state;
    Word_t *ptr;

    while (n--){
        JLI(ptr, freqs, encoded[n]);
        ++*ptr;
    }

    /* include frequencies for timestamp deltas */
    JLI(ptr, freqs, ev->timestamp);
    ++*ptr;
    return state;
}

Pvoid_t collect_unigrams(FILE *grouped,
                         uint64_t num_events,
                         const tdb_item *items,
                         uint64_t num_fields)
{
    /* calculate frequencies of all items */
    return event_fold(all_freqs, grouped, num_events, items, num_fields, NULL);
}

