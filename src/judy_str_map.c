
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <xxhash.h>

#include <judy_str_map.h>

#define MAX_NUM_RETRIES 16

struct jsm_item{
    uint64_t id;
    uint64_t length;
    char value[0];
} __attribute__((packed));

static uint64_t jsm_insert_small(struct judy_str_map *jsm,
                                 const char *buf,
                                 uint64_t length)
{
    char key[8];
    Word_t *ptr;

    key[0] = length;
    memcpy(&key[1], buf, length);

    JLI(ptr, jsm->small_map, *(Word_t*)key);
    if (!*ptr)
        *ptr = ++jsm->num_keys;
    return *ptr;
}

static uint64_t jsm_insert_large(struct judy_str_map *jsm,
                                 const char *buf,
                                 uint64_t length,
                                 int num_retries)
{
    Word_t *ptr;
    Word_t key;

    XXH64_reset(&jsm->hash_state, num_retries + 1);
    XXH64_update(&jsm->hash_state, buf, length);
    key = XXH64_digest(&jsm->hash_state);

    JLI(ptr, jsm->large_map, key);
    if (*ptr){
        const struct jsm_item *item_ro =
            (const struct jsm_item*)&jsm->buffer[*ptr];

        if (item_ro->length == length && !memcmp(item_ro->value, buf, length))
            return item_ro->id;
        else{
            if (++num_retries < MAX_NUM_RETRIES)
                return jsm_insert_large(jsm, buf, length, num_retries);
            else{
                fprintf(stderr, "All hash lookups failed for a key of size %"
                                PRIu64". Very strange!\n", length);
                return 0;
            }
        }
    }else{
        struct jsm_item item;

        if (jsm->buffer_offset + length + sizeof(item) > jsm->buffer_size){
            while (jsm->buffer_offset + length + sizeof(item) >
                   jsm->buffer_size)
                jsm->buffer_size *= 2;
            if (!(jsm->buffer = realloc(jsm->buffer, jsm->buffer_size)))
                return 0;
        }

        *ptr = jsm->buffer_offset;
        item.id = ++jsm->num_keys;
        item.length = length;
        memcpy(&jsm->buffer[jsm->buffer_offset], &item, sizeof(item));
        jsm->buffer_offset += sizeof(item);
        memcpy(&jsm->buffer[jsm->buffer_offset], buf, length);
        jsm->buffer_offset += length;
        return item.id;
    }
}

void *jsm_fold(const struct judy_str_map *jsm,
               judy_str_fold_fn fun,
               void *state)
{
    char buf[8];
    Word_t *ptr;
    Word_t key = 0;

    uint64_t offset = 0;

    JLF(ptr, jsm->small_map, key);
    while (ptr){
        memcpy(buf, &key, 8);
        state = fun(*ptr, &buf[1], buf[0], state);
        JLN(ptr, jsm->small_map, key);
    }

    while (offset < jsm->buffer_offset){
        const struct jsm_item *item =
            (const struct jsm_item*)&jsm->buffer[offset];

        state = fun(item->id, item->value, item->length, state);
        offset += item->length + sizeof(struct jsm_item);
    }

    return state;
}

uint64_t jsm_insert(struct judy_str_map *jsm, const char *buf, uint64_t length)
{
    if (length == 0)
        return 0;
    else if (length > 7)
        return jsm_insert_large(jsm, buf, length, 0);
    else
        return jsm_insert_small(jsm, buf, length);
}

int jsm_init(struct judy_str_map *jsm)
{
    memset(jsm, 0, sizeof(struct judy_str_map));
    jsm->buffer_size = BUFFER_INITIAL_SIZE;
    if (!(jsm->buffer = malloc(jsm->buffer_size)))
        return 1;
    return 0;
}

void jsm_free(struct judy_str_map *jsm)
{
    Word_t tmp;

    JLFA(tmp, jsm->small_map);
    JLFA(tmp, jsm->large_map);

    free(jsm->buffer);
}

#if 0
int main(int argc, char **argv)
{
    FILE *in = fopen(argv[1], "r");
    char *line = NULL;
    size_t len = 0;
    struct judy_str_map jsm;
    ssize_t read;

    jsm_init(&jsm);

   while ((read = getline(&line, &len, in)) != -1) {
        uint64_t x = jsm_insert(&jsm, line, read);
   }
}
#endif
