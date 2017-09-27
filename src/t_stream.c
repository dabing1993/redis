/*
 * Copyright (c) 2017, Salvatore Sanfilippo <antirez at gmail dot com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "server.h"
#include "endianconv.h"
#include "stream.h"

#define STREAM_BYTES_PER_LISTPACK 4096

/* Every stream item inside the listpack, has a flags field that is used to
 * mark the entry as deleted, or having the same field as the "master"
 * entry at the start of the listpack> */
#define STREAM_ITEM_FLAG_NONE 0             /* No special flags. */
#define STREAM_ITEM_FLAG_DELETED (1<<0)     /* Entry is delted. Skip it. */
#define STREAM_ITEM_FLAG_SAMEFIELDS (1<<1)  /* Same fields as master entry. */

/* -----------------------------------------------------------------------
 * Low level stream encoding: a radix tree of listpacks.
 * ----------------------------------------------------------------------- */

/* Create a new stream data structure. */
stream *streamNew(void) {
    stream *s = zmalloc(sizeof(*s));
    s->rax = raxNew();
    s->length = 0;
    s->last_id.ms = 0;
    s->last_id.seq = 0;
    return s;
}

/* Free a stream, including the listpacks stored inside the radix tree. */
void freeStream(stream *s) {
    raxFreeWithCallback(s->rax,(void(*)(void*))lpFree);
    zfree(s);
}

/* Generate the next stream item ID given the previous one. If the current
 * milliseconds Unix time is greater than the previous one, just use this
 * as time part and start with sequence part of zero. Otherwise we use the
 * previous time (and never go backward) and increment the sequence. */
void streamNextID(streamID *last_id, streamID *new_id) {
    uint64_t ms = mstime();
    if (ms > last_id->ms) {
        new_id->ms = ms;
        new_id->seq = 0;
    } else {
        new_id->ms = last_id->ms;
        new_id->seq = last_id->seq+1;
    }
}

/* This is just a wrapper for lpAppend() to directly use a 64 bit integer
 * instead of a string. */
unsigned char *lpAppendInteger(unsigned char *lp, int64_t value) {
    char buf[LONG_STR_SIZE];
    int slen = ll2string(buf,sizeof(buf),value);
    return lpAppend(lp,(unsigned char*)buf,slen);
}

/* This is a wrapper function for lpGet() to directly get an integer value
 * from the listpack (that may store numbers as a string), converting
 * the string if needed. */
int64_t lpGetInteger(unsigned char *ele) {
    int64_t v;
    unsigned char *e = lpGet(ele,&v,NULL);
    if (e == NULL) return v;
    /* The following code path should never be used for how listpacks work:
     * they should always be able to store an int64_t value in integer
     * encoded form. However the implementation may change. */
    int retval = string2ll((char*)e,v,&v);
    serverAssert(retval != 0);
    return v;
}

/* Debugging function to log the full content of a listpack. Useful
 * for development and debugging. */
void streamLogListpackContent(unsigned char *lp) {
    unsigned char *p = lpFirst(lp);
    while(p) {
        unsigned char buf[LP_INTBUF_SIZE];
        int64_t v;
        unsigned char *ele = lpGet(p,&v,buf);
        serverLog(LL_WARNING,"- [%d] '%.*s'", (int)v, (int)v, ele);
        p = lpNext(lp,p);
    }
}

/* Convert the specified stream entry ID as a 128 bit big endian number, so
 * that the IDs can be sorted lexicographically. */
void streamEncodeID(void *buf, streamID *id) {
    uint64_t e[2];
    e[0] = htonu64(id->ms);
    e[1] = htonu64(id->seq);
    memcpy(buf,e,sizeof(e));
}

/* This is the reverse of streamEncodeID(): the decoded ID will be stored
 * in the 'id' structure passed by reference. The buffer 'buf' must point
 * to a 128 bit big-endian encoded ID. */
void streamDecodeID(void *buf, streamID *id) {
    uint64_t e[2];
    memcpy(e,buf,sizeof(e));
    id->ms = ntohu64(e[0]);
    id->seq = ntohu64(e[1]);
}

/* Adds a new item into the stream 's' having the specified number of
 * field-value pairs as specified in 'numfields' and stored into 'argv'.
 * Returns the new entry ID populating the 'added_id' structure.
 *
 * If 'use_id' is not NULL, the ID is not auto-generated by the function,
 * but instead the passed ID is uesd to add the new entry. In this case
 * adding the entry may fail as specified later in this comment.
 *
 * The function returns C_OK if the item was added, this is always true
 * if the ID was generated by the function. However the function may return
 * C_ERR if an ID was given via 'use_id', but adding it failed since the
 * current top ID is greater or equal. */
int streamAppendItem(stream *s, robj **argv, int numfields, streamID *added_id, streamID *use_id) {
    /* If an ID was given, check that it's greater than the last entry ID
     * or return an error. */
    if (use_id && (use_id->ms < s->last_id.ms ||
                   (use_id->ms == s->last_id.ms &&
                    use_id->seq <= s->last_id.seq))) return C_ERR;

    /* Add the new entry. */
    raxIterator ri;
    raxStart(&ri,s->rax);
    raxSeek(&ri,"$",NULL,0);

    size_t lp_bytes = 0;        /* Total bytes in the tail listpack. */
    unsigned char *lp = NULL;   /* Tail listpack pointer. */

    /* Get a reference to the tail node listpack. */
    if (raxNext(&ri)) {
        lp = ri.data;
        lp_bytes = lpBytes(lp);
    }
    raxStop(&ri);

    /* Generate the new entry ID. */
    streamID id;
    if (use_id)
        id = *use_id;
    else
        streamNextID(&s->last_id,&id);

    /* We have to add the key into the radix tree in lexicographic order,
     * to do so we consider the ID as a single 128 bit number written in
     * big endian, so that the most significant bytes are the first ones. */
    uint64_t rax_key[2];    /* Key in the radix tree containing the listpack.*/
    streamID master_id;     /* ID of the master entry in the listpack. */

    /* Create a new listpack and radix tree node if needed. */
    int flags = STREAM_ITEM_FLAG_NONE;
    if (lp == NULL || lp_bytes > STREAM_BYTES_PER_LISTPACK) {
        master_id = id;
        streamEncodeID(rax_key,&id);
        /* Create the listpack having the master entry ID and fields. */
        lp = lpNew();
        lp = lpAppend(lp,(unsigned char*)rax_key,sizeof(rax_key));
        lp = lpAppendInteger(lp,numfields);
        for (int i = 0; i < numfields; i++) {
            sds field = argv[i*2]->ptr;
            lp = lpAppend(lp,(unsigned char*)field,sdslen(field));
        }
        raxInsert(s->rax,(unsigned char*)&rax_key,sizeof(rax_key),lp,NULL);
        /* The first entry we insert, has obviously the same fields of the
         * master entry. */
        flags |= STREAM_ITEM_FLAG_SAMEFIELDS;
    } else {
        serverAssert(ri.key_len == sizeof(rax_key));
        memcpy(rax_key,ri.key,sizeof(rax_key));

        /* Read the master entry ID. */
        int64_t e_len;
        unsigned char *lp_ele = lpFirst(lp);
        unsigned char buf[LP_INTBUF_SIZE];
        unsigned char *e = lpGet(lp_ele,&e_len,buf);
        serverAssert(e_len == sizeof(streamID));
        streamDecodeID(e,&master_id);
        lp_ele = lpNext(lp,lp_ele);

        /* Check if the entry we are adding, have the same fields
         * as the master entry. */
        int master_fields_count = lpGetInteger(lp_ele);
        lp_ele = lpNext(lp,lp_ele);
        if (numfields == master_fields_count) {
            int i;
            for (i = 0; i < master_fields_count; i++) {
                sds field = argv[i*2]->ptr;
                unsigned char *e = lpGet(lp_ele,&e_len,buf);
                /* Stop if there is a mismatch. */
                if (sdslen(field) != (size_t)e_len ||
                    memcmp(e,field,e_len) != 0) break;
                lp_ele = lpNext(lp,lp_ele);
            }
            if (i == master_fields_count) flags |= STREAM_ITEM_FLAG_SAMEFIELDS;
        }
    }

    /* Populate the listpack with the new entry. We use the following
     * encoding:
     *
     * +-----+--------+----------+-------+-------+-/-+-------+-------+
     * |flags|entry-id|num-fields|field-1|value-1|...|field-N|value-N|
     * +-----+--------+----------+-------+-------+-/-+-------+-------+
     *
     * However if the SAMEFIELD flag is set, we have just to populate
     * the entry with the values, so it becomes:
     *
     * +-----+--------+-------+-/-+-------+
     * |flags|entry-id|value-1|...|value-N|
     * +-----+--------+-------+-/-+-------+
     *
     * The entry-id field is actually two separated fields: the ms
     * and seq difference compared to the master entry.
     */
    lp = lpAppendInteger(lp,flags);
    lp = lpAppendInteger(lp,id.ms - master_id.ms);
    lp = lpAppendInteger(lp,id.seq - master_id.seq);
    if (!(flags & STREAM_ITEM_FLAG_SAMEFIELDS))
        lp = lpAppendInteger(lp,numfields);
    for (int i = 0; i < numfields; i++) {
        sds field = argv[i*2]->ptr, value = argv[i*2+1]->ptr;
        if (!(flags & STREAM_ITEM_FLAG_SAMEFIELDS))
            lp = lpAppend(lp,(unsigned char*)field,sdslen(field));
        lp = lpAppend(lp,(unsigned char*)value,sdslen(value));
    }

    /* Insert back into the tree in order to update the listpack pointer. */
    raxInsert(s->rax,(unsigned char*)&rax_key,sizeof(rax_key),lp,NULL);
    s->length++;
    s->last_id = id;
    if (added_id) *added_id = id;
    return C_OK;
}

/* Initialize the stream iterator, so that we can call iterating functions
 * to get the next items. This requires a corresponding streamIteratorStop()
 * at the end.
 *
 * Once the iterator is initalized, we iterate like this:
 *
 *  streamIterator myiterator;
 *  streamIteratorStart(&myiterator,...);
 *  int64_t numfields;
 *  while(streamIteratorGetID(&myitereator,&ID,&numfields)) {
 *      while(numfields--) {
 *          unsigned char *key, *value;
 *          size_t key_len, value_len;
 *          streamIteratorGetField(&myiterator,&key,&value,&key_len,&value_len);
 *
 *          ... do what you want with key and value ...
 *      }
 *  }
 *  streamIteratorStop(&myiterator); */
void streamIteratorStart(streamIterator *si, stream *s, streamID *start, streamID *end) {
    /* Intialize the iterator and translates the iteration start/stop
     * elements into a 128 big big-endian number. */
    if (start) {
        streamEncodeID(si->start_key,start);
    } else {
        si->start_key[0] = 0;
        si->start_key[0] = 0;
    }

    if (end) {
        streamEncodeID(si->end_key,end);
    } else {
        si->end_key[0] = UINT64_MAX;
        si->end_key[0] = UINT64_MAX;
    }

    /* Seek the correct node in the radix tree. */
    raxStart(&si->ri,s->rax);
    if (start && (start->ms || start->seq)) {
        raxSeek(&si->ri,"<=",(unsigned char*)si->start_key,
                sizeof(si->start_key));
        if (raxEOF(&si->ri))
            raxSeek(&si->ri,">",(unsigned char*)si->start_key,
                    sizeof(si->start_key));
    } else {
        raxSeek(&si->ri,"^",NULL,0);
    }
    si->lp = NULL; /* There is no current listpack right now. */
    si->lp_ele = NULL; /* Current listpack cursor. */
}

/* Return 1 and store the current item ID at 'id' if there are still
 * elements within the iteration range, otherwise return 0 in order to
 * signal the iteration terminated. */
int streamIteratorGetID(streamIterator *si, streamID *id, int64_t *numfields) {
    while(1) { /* Will stop when element > stop_key or end of radix tree. */
        /* If the current listpack is set to NULL, this is the start of the
         * iteration or the previous listpack was completely iterated.
         * Go to the next node. */
        if (si->lp == NULL || si->lp_ele == NULL) {
            if (!raxNext(&si->ri)) return 0;
            serverAssert(si->ri.key_len == sizeof(streamID));
            si->lp = si->ri.data;
            si->lp_ele = lpFirst(si->lp); /* Seek the master ID. */
            /* Get the master ID. */
            int64_t e_len;
            unsigned char buf[LP_INTBUF_SIZE];
            unsigned char *e = lpGet(si->lp_ele,&e_len,buf);
            serverAssert(e_len == sizeof(streamID));
            streamDecodeID(e,&si->master_id);
            si->lp_ele = lpNext(si->lp,si->lp_ele); /* Seek fields count. */
            /* Get the master fields count. */
            si->master_fields_count = lpGetInteger(si->lp_ele);
            si->lp_ele = lpNext(si->lp,si->lp_ele); /* Seek first field. */
            si->master_fields_start = si->lp_ele;
            /* Skip master fileds to seek the first entry. */
            for (uint64_t i = 0; i < si->master_fields_count; i++)
                si->lp_ele = lpNext(si->lp,si->lp_ele);
        }

        /* For every radix tree node, iterate the corresponding listpack,
         * returning elements when they are within range. */
        while(si->lp_ele) {
            /* Get the flags entry. */
            int flags = lpGetInteger(si->lp_ele);
            si->lp_ele = lpNext(si->lp,si->lp_ele); /* Seek ID. */

            /* Get the ID: it is encoded as difference between the master
             * ID and this entry ID. */
            *id = si->master_id;
            id->ms += lpGetInteger(si->lp_ele);
            si->lp_ele = lpNext(si->lp,si->lp_ele);
            id->seq += lpGetInteger(si->lp_ele);
            si->lp_ele = lpNext(si->lp,si->lp_ele);
            unsigned char buf[sizeof(streamID)];
            streamEncodeID(buf,id);

            /* The number of entries is here or not depending on the
             * flags. */
            if (flags & STREAM_ITEM_FLAG_SAMEFIELDS) {
                *numfields = si->master_fields_count;
            } else {
                *numfields = lpGetInteger(si->lp_ele);
                si->lp_ele = lpNext(si->lp,si->lp_ele);
            }

            /* If current >= start, and the entry is not marked as
             * deleted, emit it. */
            if (memcmp(buf,si->start_key,sizeof(streamID)) >= 0 &&
                !(flags & STREAM_ITEM_FLAG_DELETED))
            {
                if (memcmp(buf,si->end_key,sizeof(streamID)) > 0)
                    return 0; /* We are already out of range. */
                si->entry_flags = flags;
                if (flags & STREAM_ITEM_FLAG_SAMEFIELDS)
                    si->master_fields_ptr = si->master_fields_start;
                return 1; /* Valid item returned. */
            }

            /* If we do not emit, we have to discard. */
            int to_discard = (flags & STREAM_ITEM_FLAG_SAMEFIELDS) ?
                                *numfields : *numfields*2;
            for (int64_t i = 0; i < to_discard; i++)
                si->lp_ele = lpNext(si->lp,si->lp_ele);
        }

        /* End of listpack reached. Try the next radix tree node. */
    }
}

/* Get the field and value of the current item we are iterating. This should
 * be called immediately after streamIteratorGetID(), and for each field
 * according to the number of fields returned by streamIteratorGetID().
 * The function populates the field and value pointers and the corresponding
 * lengths by reference, that are valid until the next iterator call, assuming
 * no one touches the stream meanwhile. */
void streamIteratorGetField(streamIterator *si, unsigned char **fieldptr, unsigned char **valueptr, int64_t *fieldlen, int64_t *valuelen) {
    if (si->entry_flags & STREAM_ITEM_FLAG_SAMEFIELDS) {
        *fieldptr = lpGet(si->master_fields_ptr,fieldlen,si->field_buf);
        si->master_fields_ptr = lpNext(si->lp,si->master_fields_ptr);
    } else {
        *fieldptr = lpGet(si->lp_ele,fieldlen,si->field_buf);
        si->lp_ele = lpNext(si->lp,si->lp_ele);
    }
    *valueptr = lpGet(si->lp_ele,valuelen,si->value_buf);
    si->lp_ele = lpNext(si->lp,si->lp_ele);
}

/* Stop the stream iterator. The only cleanup we need is to free the rax
 * itereator, since the stream iterator itself is supposed to be stack
 * allocated. */
void streamIteratorStop(streamIterator *si) {
    raxStop(&si->ri);
}

/* Send the specified range to the client 'c'. The range the client will
 * receive is between start and end inclusive, if 'count' is non zero, no more
 * than 'count' elemnets are sent. The 'end' pointer can be NULL to mean that
 * we want all the elements from 'start' till the end of the stream. */
size_t streamReplyWithRange(client *c, stream *s, streamID *start, streamID *end, size_t count) {
    void *arraylen_ptr = addDeferredMultiBulkLength(c);
    size_t arraylen = 0;
    streamIterator si;
    int64_t numfields;
    streamID id;

    streamIteratorStart(&si,s,start,end);
    while(streamIteratorGetID(&si,&id,&numfields)) {
        /* Emit a two elements array for each item. The first is
         * the ID, the second is an array of field-value pairs. */
        sds replyid = sdscatfmt(sdsempty(),"+%U.%U\r\n",id.ms,id.seq);
        addReplyMultiBulkLen(c,2);
        addReplySds(c,replyid);
        addReplyMultiBulkLen(c,numfields*2);

        /* Emit the field-value pairs. */
        while(numfields--) {
            unsigned char *key, *value;
            int64_t key_len, value_len;
            streamIteratorGetField(&si,&key,&value,&key_len,&value_len);
            addReplyBulkCBuffer(c,key,key_len);
            addReplyBulkCBuffer(c,value,value_len);
        }
        arraylen++;
        if (count && count == arraylen) break;
    }
    streamIteratorStop(&si);
    setDeferredMultiBulkLength(c,arraylen_ptr,arraylen);
    return arraylen;
}

/* -----------------------------------------------------------------------
 * Stream commands implementation
 * ----------------------------------------------------------------------- */

/* Look the stream at 'key' and return the corresponding stream object.
 * The function creates a key setting it to an empty stream if needed. */
robj *streamTypeLookupWriteOrCreate(client *c, robj *key) {
    robj *o = lookupKeyWrite(c->db,key);
    if (o == NULL) {
        o = createStreamObject();
        dbAdd(c->db,key,o);
    } else {
        if (o->type != OBJ_STREAM) {
            addReply(c,shared.wrongtypeerr);
            return NULL;
        }
    }
    return o;
}

/* Helper function to convert a string to an unsigned long long value.
 * The function attempts to use the faster string2ll() function inside
 * Redis: if it fails, strtoull() is used instead. The function returns
 * 1 if the conversion happened successfully or 0 if the number is
 * invalid or out of range. */
int string2ull(const char *s, unsigned long long *value) {
    long long ll;
    if (string2ll(s,strlen(s),&ll)) {
        if (ll < 0) return 0; /* Negative values are out of range. */
        *value = ll;
        return 1;
    }
    errno = 0;
    *value = strtoull(s,NULL,10);
    if (errno == EINVAL || errno == ERANGE) return 0; /* strtoull() failed. */
    return 1; /* Conversion done! */
}

/* Parse a stream ID in the format given by clients to Redis, that is
 * <ms>.<seq>, and converts it into a streamID structure. If
 * the specified ID is invalid C_ERR is returned and an error is reported
 * to the client, otherwise C_OK is returned. The ID may be in incomplete
 * form, just stating the milliseconds time part of the stream. In such a case
 * the missing part is set according to the value of 'missing_seq' parameter.
 * The IDs "-" and "+" specify respectively the minimum and maximum IDs
 * that can be represented.
 *
 * If 'c' is set to NULL, no reply is sent to the client. */
int streamParseIDOrReply(client *c, robj *o, streamID *id, uint64_t missing_seq) {
    char buf[128];
    if (sdslen(o->ptr) > sizeof(buf)-1) goto invalid;
    memcpy(buf,o->ptr,sdslen(o->ptr)+1);

    /* Handle the "-" and "+" special cases. */
    if (buf[0] == '-' && buf[1] == '\0') {
        id->ms = 0;
        id->seq = 0;
        return C_OK;
    } else if (buf[0] == '+' && buf[1] == '\0') {
        id->ms = UINT64_MAX;
        id->seq = UINT64_MAX;
        return C_OK;
    }

    /* Parse <ms>.<seq> form. */
    char *dot = strchr(buf,'.');
    if (dot) *dot = '\0';
    uint64_t ms, seq;
    if (string2ull(buf,&ms) == 0) goto invalid;
    if (dot && string2ull(dot+1,&seq) == 0) goto invalid;
    if (!dot) seq = missing_seq;
    id->ms = ms;
    id->seq = seq;
    return C_OK;

invalid:
    if (c) addReplyError(c,"Invalid stream ID specified as stream "
                           "command argument");
    return C_ERR;
}

/* XADD key [MAXLEN <count>] <ID or *> [field value] [field value] ... */
void xaddCommand(client *c) {
    streamID id;
    int id_given = 0; /* Was an ID different than "*" specified? */

    /* Parse options. */
    int i = 2; /* This is the first argument position where we could
                  find an option, or the ID. */
    for (; i < c->argc; i++) {
        int moreargs = i != c->argc-1;
        char *opt = c->argv[i]->ptr;
        if (opt[0] == '*' && opt[1] == '\0') {
            /* This is just a fast path for the common case of auto-ID
             * creation. */
            break;
        } else if (!strcasecmp(opt,"maxlen") && moreargs) {
            addReplyError(c,"Sorry, MAXLEN is still not implemented");
            i++;
            return;
        } else {
            /* If we are here is a syntax error or a valid ID. */
            if (streamParseIDOrReply(NULL,c->argv[i],&id,0) == C_OK) {
                id_given = 1;
                break;
            } else {
                addReply(c,shared.syntaxerr);
                return;
            }
        }
    }
    int field_pos = i+1;

    /* Check arity. */
    if ((c->argc - field_pos) < 2 || (c->argc-field_pos % 2) == 1) {
        addReplyError(c,"wrong number of arguments for XADD");
        return;
    }

    /* Lookup the stream at key. */
    robj *o;
    stream *s;
    if ((o = streamTypeLookupWriteOrCreate(c,c->argv[1])) == NULL) return;
    s = o->ptr;

    /* Append using the low level function and return the ID. */
    if (streamAppendItem(s,c->argv+field_pos,(c->argc-field_pos)/2,
        &id, id_given ? &id : NULL)
        == C_ERR)
    {
        addReplyError(c,"The ID specified in XADD is smaller than the "
                        "target stream top item");
        return;
    }
    sds reply = sdscatfmt(sdsempty(),"+%U.%U\r\n",id.ms,id.seq);
    addReplySds(c,reply);

    signalModifiedKey(c->db,c->argv[1]);
    notifyKeyspaceEvent(NOTIFY_STREAM,"xadd",c->argv[1],c->db->id);
    server.dirty++;

    /* Let's rewrite the ID argument with the one actually generated for
     * AOF/replication propagation. */
    robj *idarg = createObject(OBJ_STRING,
                  sdscatfmt(sdsempty(),"%U.%U",id.ms,id.seq));
    rewriteClientCommandArgument(c,i,idarg);
    decrRefCount(idarg);

    /* We need to signal to blocked clients that there is new data on this
     * stream. */
    if (server.blocked_clients_by_type[BLOCKED_STREAM])
        signalKeyAsReady(c->db, c->argv[1]);
}

/* XRANGE key start end [COUNT <n>] */
void xrangeCommand(client *c) {
    robj *o;
    stream *s;
    streamID startid, endid;
    long long count = 0;

    if (streamParseIDOrReply(c,c->argv[2],&startid,0) == C_ERR) return;
    if (streamParseIDOrReply(c,c->argv[3],&endid,UINT64_MAX) == C_ERR) return;

    /* Parse the COUNT option if any. */
    if (c->argc > 4) {
        if (strcasecmp(c->argv[4]->ptr,"COUNT") == 0) {
            if (getLongLongFromObjectOrReply(c,c->argv[5],&count,NULL) != C_OK)
                return;
            if (count < 0) count = 0;
        } else {
            addReply(c,shared.syntaxerr);
            return;
        }
    }

    /* Return the specified range to the user. */
    if ((o = lookupKeyReadOrReply(c,c->argv[1],shared.emptymultibulk)) == NULL
        || checkType(c,o,OBJ_STREAM)) return;
    s = o->ptr;
    streamReplyWithRange(c,s,&startid,&endid,count);
}

/* XLEN */
void xlenCommand(client *c) {
    robj *o;
    if ((o = lookupKeyReadOrReply(c,c->argv[1],shared.czero)) == NULL
        || checkType(c,o,OBJ_STREAM)) return;
    stream *s = o->ptr;
    addReplyLongLong(c,s->length);
}

/* XREAD [BLOCK <milliseconds>] [COUNT <count>] [GROUP <groupname> <ttl>]
 *       [RETRY <milliseconds> <ttl>] STREAMS key_1 key_2 ... key_N
 *       ID_1 ID_2 ... ID_N */
#define XREAD_BLOCKED_DEFAULT_COUNT 1000
void xreadCommand(client *c) {
    long long timeout = -1; /* -1 means, no BLOCK argument given. */
    long long count = 0;
    int streams_count = 0;
    int streams_arg = 0;
    #define STREAMID_STATIC_VECTOR_LEN 8
    streamID static_ids[STREAMID_STATIC_VECTOR_LEN];
    streamID *ids = static_ids;

    /* Parse arguments. */
    for (int i = 1; i < c->argc; i++) {
        int moreargs = i != c->argc-1;
        char *o = c->argv[i]->ptr;
        if (!strcasecmp(o,"BLOCK") && moreargs) {
            i++;
            if (getTimeoutFromObjectOrReply(c,c->argv[i],&timeout,
                UNIT_MILLISECONDS) != C_OK) return;
        } else if (!strcasecmp(o,"COUNT") && moreargs) {
            i++;
            if (getLongLongFromObjectOrReply(c,c->argv[i],&count,NULL) != C_OK)
                return;
            if (count < 0) count = 0;
        } else if (!strcasecmp(o,"STREAMS") && moreargs) {
            streams_arg = i+1;
            streams_count = (c->argc-streams_arg);
            if ((streams_count % 2) != 0) {
                addReplyError(c,"Unbalanced XREAD list of streams: "
                                "for each stream key an ID or '$' must be "
                                "specified.");
                return;
            }
            streams_count /= 2; /* We have two arguments for each stream. */
            break;
        } else {
            addReply(c,shared.syntaxerr);
            return;
        }
    }

    /* STREAMS option is mandatory. */
    if (streams_arg == 0) {
        addReply(c,shared.syntaxerr);
        return;
    }

    /* Parse the IDs. */
    if (streams_count > STREAMID_STATIC_VECTOR_LEN)
        ids = zmalloc(sizeof(streamID)*streams_count);

    for (int i = streams_arg + streams_count; i < c->argc; i++) {
        /* Specifying "$" as last-known-id means that the client wants to be
         * served with just the messages that will arrive into the stream
         * starting from now. */
        int id_idx = i - streams_arg - streams_count;
        if (strcmp(c->argv[i]->ptr,"$") == 0) {
            robj *o = lookupKeyRead(c->db,c->argv[i-streams_count]);
            if (o) {
                stream *s = o->ptr;
                ids[id_idx] = s->last_id;
            } else {
                ids[id_idx].ms = 0;
                ids[id_idx].seq = 0;
            }
            continue;
        }
        if (streamParseIDOrReply(c,c->argv[i],ids+id_idx,0) != C_OK)
            goto cleanup;
    }

    /* Try to serve the client synchronously. */
    size_t arraylen = 0;
    void *arraylen_ptr = NULL;
    for (int i = 0; i < streams_count; i++) {
        robj *o = lookupKeyRead(c->db,c->argv[streams_arg+i]);
        if (o == NULL) continue;
        stream *s = o->ptr;
        streamID *gt = ids+i; /* ID must be greater than this. */
        if (s->last_id.ms > gt->ms ||
            (s->last_id.ms == gt->ms && s->last_id.seq > gt->seq))
        {
            arraylen++;
            if (arraylen == 1) arraylen_ptr = addDeferredMultiBulkLength(c);
            /* streamReplyWithRange() handles the 'start' ID as inclusive,
             * so start from the next ID, since we want only messages with
             * IDs greater than start. */
            streamID start = *gt;
            start.seq++; /* Can't overflow, it's an uint64_t */

            /* Emit the two elements sub-array consisting of the name
             * of the stream and the data we extracted from it. */
            addReplyMultiBulkLen(c,2);
            addReplyBulk(c,c->argv[i+streams_arg]);
            streamReplyWithRange(c,s,&start,NULL,count);
        }
    }

     /* We replied synchronously! Set the top array len and return to caller. */
    if (arraylen) {
        setDeferredMultiBulkLength(c,arraylen_ptr,arraylen);
        goto cleanup;
    }

    /* Block if needed. */
    if (timeout != -1) {
        /* If we are inside a MULTI/EXEC and the list is empty the only thing
         * we can do is treating it as a timeout (even with timeout 0). */
        if (c->flags & CLIENT_MULTI) {
            addReply(c,shared.nullmultibulk);
            goto cleanup;
        }
        blockForKeys(c, BLOCKED_STREAM, c->argv+streams_arg, streams_count,
                     timeout, NULL, ids);
        /* If no COUNT is given and we block, set a relatively small count:
         * in case the ID provided is too low, we do not want the server to
         * block just to serve this client a huge stream of messages. */
        c->bpop.xread_count = count ? count : XREAD_BLOCKED_DEFAULT_COUNT;
        c->bpop.xread_group = NULL; /* Not used for now. */
        goto cleanup;
    }

    /* No BLOCK option, nor any stream we can serve. Reply as with a
     * timeout happened. */
    addReply(c,shared.nullmultibulk);
    /* Continue to cleanup... */

cleanup:
    /* Cleanup. */
    if (ids != static_ids) zfree(ids);
}


