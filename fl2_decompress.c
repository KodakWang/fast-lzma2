/*
* Copyright (c) 2018, Conor McCarthy
* All rights reserved.
* Parts based on zstd_decompress.c copyright Yann Collet
*
* This source code is licensed under both the BSD-style license (found in the
* LICENSE file in the root directory of this source tree) and the GPLv2 (found
* in the COPYING file in the root directory of this source tree).
* You may select, at your option, one of the above-listed licenses.
*/

#include <string.h>
#include "fast-lzma2.h"
#include "fl2_internal.h"
#include "mem.h"
#include "util.h"
#include "lzma2_dec.h"
#include "fl2_pool.h"
#ifndef NO_XXHASH
#  include "xxhash.h"
#endif


#define LZMA2_PROP_UNINITIALIZED 0xFF


FL2LIB_API size_t FL2LIB_CALL FL2_findDecompressedSize(const void *src, size_t srcSize)
{
    return FLzma2Dec_UnpackSize(src, srcSize);
}

typedef struct
{
    CLzma2Dec* dec;
    const void *src;
    size_t packPos;
    size_t packSize;
    size_t unpackPos;
    size_t unpackSize;
    size_t res;
    ELzmaFinishMode finish;
} BlockDecMt;

typedef struct FL2_DCtx_s
{
    CLzma2Dec dec;
#ifndef FL2_SINGLETHREAD
    BlockDecMt *blocks;
    FL2POOL_ctx *factory;
    size_t nbThreads;
#endif
    BYTE lzma2prop;
} FL2_DCtx;

FL2LIB_API size_t FL2LIB_CALL FL2_decompress(void* dst, size_t dstCapacity,
    const void* src, size_t compressedSize)
{
    return FL2_decompressMt(dst, dstCapacity, src, compressedSize, 1);
}

FL2LIB_API size_t FL2LIB_CALL FL2_decompressMt(void* dst, size_t dstCapacity,
    const void* src, size_t compressedSize,
    unsigned nbThreads)
{
    size_t dSize;
    FL2_DCtx* const dctx = FL2_createDCtxMt(nbThreads);
    if(dctx == NULL)
        return FL2_ERROR(memory_allocation);
    dSize = FL2_decompressDCtx(dctx,
        dst, dstCapacity,
        src, compressedSize);
    FL2_freeDCtx(dctx);
    return dSize;
}

FL2LIB_API FL2_DCtx* FL2LIB_CALL FL2_createDCtx(void)
{
    return FL2_createDCtxMt(1);
}

FL2LIB_API FL2_DCtx *FL2LIB_CALL FL2_createDCtxMt(unsigned nbThreads)
{
    DEBUGLOG(3, "FL2_createDCtx");

    FL2_DCtx* const dctx = malloc(sizeof(FL2_DCtx));

    if (dctx != NULL) {
        LzmaDec_Construct(&dctx->dec);

        dctx->lzma2prop = LZMA2_PROP_UNINITIALIZED;

        nbThreads = FL2_checkNbThreads(nbThreads);

#ifndef FL2_SINGLETHREAD
        dctx->nbThreads = 1;

        if (nbThreads > 1) {
            dctx->blocks = malloc(nbThreads * sizeof(BlockDecMt));
            dctx->factory = FL2POOL_create(nbThreads - 1);

            if (dctx->blocks == NULL || dctx->factory == NULL) {
                FL2_freeDCtx(dctx);
                return NULL;
            }
            dctx->blocks[0].dec = &dctx->dec;

            for (; dctx->nbThreads < nbThreads; ++dctx->nbThreads) {

                dctx->blocks[dctx->nbThreads].dec = malloc(sizeof(CLzma2Dec));

                if (dctx->blocks[dctx->nbThreads].dec == NULL) {
                    FL2_freeDCtx(dctx);
                    return NULL;
                }
                LzmaDec_Construct(dctx->blocks[dctx->nbThreads].dec);
            }
        }
        else {
            dctx->blocks = NULL;
            dctx->factory = NULL;
        }
#endif
    }
    return dctx;
}

FL2LIB_API size_t FL2LIB_CALL FL2_freeDCtx(FL2_DCtx* dctx)
{
    if (dctx != NULL) {
        DEBUGLOG(3, "FL2_freeDCtx");

        FLzmaDec_Free(&dctx->dec);

#ifndef FL2_SINGLETHREAD
        if (dctx->blocks != NULL) {
            for (unsigned thread = 1; thread < dctx->nbThreads; ++thread) {
                FLzmaDec_Free(dctx->blocks[thread].dec);
                free(dctx->blocks[thread].dec);
            }
            free(dctx->blocks);
        }
        FL2POOL_free(dctx->factory);
#endif
        free(dctx);
    }
    return 0;
}

#ifndef FL2_SINGLETHREAD

/* FL2_decompressCtxBlock() : FL2POOL_function type */
static void FL2_decompressCtxBlock(void* const jobDescription, size_t n)
{
    BlockDecMt* const blocks = (BlockDecMt*)jobDescription;
    size_t srcLen = blocks[n].packSize;
    blocks[n].res = FLzma2Dec_DecodeToDic(blocks[n].dec, blocks[n].unpackSize, blocks[n].src, &srcLen, blocks[n].finish);
    if (!FL2_isError(blocks[n].res))
        blocks[n].res = blocks[n].dec->dicPos;
}

static size_t FL2_decompressCtxBlocksMt(FL2_DCtx* dctx, const BYTE *src, BYTE *dst, size_t dstCapacity, size_t nbThreads)
{
    BlockDecMt* const blocks = dctx->blocks;
    BYTE prop = dctx->lzma2prop & FL2_LZMA_PROP_MASK;

    if (dstCapacity < blocks[0].unpackSize)
        return FL2_ERROR(dstSize_tooSmall);
    blocks[0].packPos = 0;
    blocks[0].unpackPos = 0;
    blocks[0].src = src;
    for (size_t thread = 1; thread < nbThreads; ++thread) {
        blocks[thread].packPos = blocks[thread - 1].packPos + blocks[thread - 1].packSize;
        blocks[thread].unpackPos = blocks[thread - 1].unpackPos + blocks[thread - 1].unpackSize;
        blocks[thread].src = src + blocks[thread].packPos;
        CHECK_F(FLzma2Dec_Init(blocks[thread].dec, prop, dst + blocks[thread].unpackPos, blocks[thread].unpackSize));
    }
    if (dstCapacity < blocks[nbThreads - 1].unpackPos + blocks[nbThreads - 1].unpackSize)
        return FL2_ERROR(dstSize_tooSmall);

    for (size_t thread = 1; thread < nbThreads; ++thread) {
        FL2POOL_add(dctx->factory, FL2_decompressCtxBlock, blocks, thread);
    }
    CHECK_F(FLzma2Dec_Init(blocks[0].dec, prop, dst + blocks[0].unpackPos, blocks[0].unpackSize));
    FL2_decompressCtxBlock(blocks, 0);
    FL2POOL_waitAll(dctx->factory, 0);
    size_t dSize = 0;
    for (size_t thread = 0; thread < nbThreads; ++thread) {
        if (FL2_isError(blocks[thread].res))
            return blocks[thread].res;
        dSize += blocks[thread].res;
    }
    return dSize;
}

static void FL2_resetMtBlocks(FL2_DCtx* dctx)
{
    for (size_t thread = 0; thread < dctx->nbThreads; ++thread) {
        dctx->blocks[thread].finish = LZMA_FINISH_ANY;
        dctx->blocks[thread].packSize = 0;
        dctx->blocks[thread].unpackSize = 0;
    }
}

static size_t FL2_decompressDCtxMt(FL2_DCtx* dctx,
    void* dst, size_t dstCapacity,
    const void* src, size_t *srcLen)
{
    size_t srcSize = *srcLen;
    *srcLen = 0;
    size_t pos = 0;
    size_t unpackSize = 0;
    BlockDecMt* const blocks = dctx->blocks;
    size_t thread = 0;
    FL2_resetMtBlocks(dctx);
    while (pos < srcSize) {
        ChunkParseInfo inf;
        int type = FLzma2Dec_ParseInput(src, pos, srcSize - pos, &inf);
        if (type == CHUNK_ERROR || type == CHUNK_MORE_DATA)
            return FL2_ERROR(corruption_detected);
        if (pos == 0 && type == CHUNK_DICT_RESET)
            type = CHUNK_CONTINUE;
        if (type == CHUNK_DICT_RESET || type == CHUNK_FINAL) {
            if (type == CHUNK_FINAL) {
                blocks[thread].finish = LZMA_FINISH_END;
                ++blocks[thread].packSize;
            }
            ++thread;
        }
        if (type == CHUNK_FINAL || (type == CHUNK_DICT_RESET && thread == dctx->nbThreads)) {
            size_t res = FL2_decompressCtxBlocksMt(dctx, (BYTE*)src, dst, dstCapacity, thread);
            if (FL2_isError(res))
                return res;
            assert(res == blocks[thread - 1].unpackPos + blocks[thread - 1].unpackSize);
            unpackSize += res;
            dctx->dec.dicPos = unpackSize;
            *srcLen += blocks[thread - 1].packPos + blocks[thread - 1].packSize;
            if (type == CHUNK_FINAL)
                return LZMA_STATUS_FINISHED_WITH_MARK;
            src = (BYTE*)src + pos;
            srcSize -= pos;
            dst = (BYTE*)dst + res;
            dstCapacity -= res;
            pos = 0;
            thread = 0;
            FL2_resetMtBlocks(dctx);
        }
        else {
            blocks[thread].packSize += inf.packSize;
            blocks[thread].unpackSize += inf.unpackSize;
            pos += inf.packSize;
        }
    }
    return FL2_ERROR(srcSize_wrong);
}

#endif

FL2LIB_API size_t FL2LIB_CALL FL2_initDCtx(FL2_DCtx * dctx, unsigned char prop)
{
    if((prop & FL2_LZMA_PROP_MASK) > 40)
        return FL2_ERROR(corruption_detected);
    dctx->lzma2prop = prop;
    return FL2_error_no_error;
}

FL2LIB_API size_t FL2LIB_CALL FL2_decompressDCtx(FL2_DCtx* dctx,
    void* dst, size_t dstCapacity,
    const void* src, size_t srcSize)
{
    size_t res;
    BYTE prop = dctx->lzma2prop;
    size_t dicPos = 0;
    const BYTE *srcBuf = src;
    size_t srcPos;

    if (prop == LZMA2_PROP_UNINITIALIZED) {
        prop = *(const BYTE*)src;
        ++srcBuf;
        --srcSize;
    }

#ifndef NO_XXHASH
    BYTE const doHash = prop >> FL2_PROP_HASH_BIT;
#endif
    prop &= FL2_LZMA_PROP_MASK;

    DEBUGLOG(4, "FL2_decompressDCtx : dict prop 0x%X, do hash %u", prop, doHash);

    srcPos = srcSize;

#ifndef FL2_SINGLETHREAD
    if (dctx->blocks != NULL) {
        dctx->lzma2prop = prop;
        res = FL2_decompressDCtxMt(dctx, dst, dstCapacity, srcBuf, &srcPos);
    }
    else 
#endif
    {
        CHECK_F(FLzma2Dec_Init(&dctx->dec, prop, dst, dstCapacity));

        dicPos = dctx->dec.dicPos;

        res = FLzma2Dec_DecodeToDic(&dctx->dec, dstCapacity, srcBuf, &srcPos, LZMA_FINISH_END);
    }

    dctx->lzma2prop = LZMA2_PROP_UNINITIALIZED;

    if (FL2_isError(res))
        return res;
    if (res == LZMA_STATUS_NEEDS_MORE_INPUT)
        return FL2_ERROR(srcSize_wrong);

    dicPos = dctx->dec.dicPos - dicPos;

#ifndef NO_XXHASH
    if (doHash) {
        XXH32_canonical_t canonical;
        U32 hash;

        DEBUGLOG(4, "Checking hash");

        if (srcSize - srcPos < XXHASH_SIZEOF)
            return FL2_ERROR(srcSize_wrong);
        memcpy(&canonical, srcBuf + srcPos, XXHASH_SIZEOF);
        hash = XXH32_hashFromCanonical(&canonical);
        if (hash != XXH32(dst, dicPos, 0))
            return FL2_ERROR(checksum_wrong);
    }
#endif
    return dicPos;
}

typedef enum
{
    FL2DEC_STAGE_INIT,
    FL2DEC_STAGE_MT_PARSE,
    FL2DEC_STAGE_DECOMP,
    FL2DEC_STAGE_MT_WRITE,
    FL2DEC_STAGE_HASH,
    FL2DEC_STAGE_FINISHED
} DecoderStage;

typedef struct
{
    CLzma2Dec dec;
    InputBlock inBlock;
    BYTE *outBuf;
    size_t bufSize;
    size_t res;
} ThreadInfo;

typedef struct
{
    FL2POOL_ctx* factory;
    InBufNode *head;
    size_t numThreads;
    size_t maxThreads;
    size_t srcThread;
    size_t srcPos;
    size_t hashPos;
    int isFinal;
    BYTE prop;
#ifndef NO_XXHASH
    XXH32_canonical_t hash;
#endif
    ThreadInfo threads[1];
} Lzma2DecMt;

struct FL2_DStream_s
{
#ifndef FL2_SINGLETHREAD
    Lzma2DecMt *decmt;
#endif
    CLzma2Dec dec;
#ifndef NO_XXHASH
    XXH32_state_t *xxh;
#endif
    DecoderStage stage;
    BYTE doHash;
    BYTE loopCount;
};

#ifndef FL2_SINGLETHREAD

static void FL2_FreeOutputBuffers(Lzma2DecMt *decmt)
{
    for (size_t thread = 0; thread < decmt->maxThreads; ++thread) {
        free(decmt->threads[thread].outBuf);
        decmt->threads[thread].outBuf = NULL;
    }
    decmt->numThreads = 0;
}

static void FL2_Lzma2DecMt_Free(Lzma2DecMt *decmt)
{
    if (decmt) {
        FL2_FreeOutputBuffers(decmt);
        FLzma2Dec_FreeInbufNodeChain(decmt->head, NULL);
        FL2POOL_free(decmt->factory);
        free(decmt);
    }
}

static void FL2_Lzma2DecMt_Init(Lzma2DecMt *decmt)
{
    if (decmt) {
        decmt->isFinal = 0;
        decmt->hashPos = 0;
        FL2_FreeOutputBuffers(decmt);
        FLzma2Dec_FreeInbufNodeChain(decmt->head->next, NULL);
        decmt->head->length = 0;
        decmt->threads[0].inBlock.first = decmt->head;
        decmt->threads[0].inBlock.last = decmt->head;
        decmt->threads[0].inBlock.startPos = 0;
        decmt->threads[0].inBlock.endPos = 0;
        decmt->threads[0].inBlock.unpackSize = 0;
    }
}

static Lzma2DecMt *FL2_Lzma2DecMt_Create(unsigned maxThreads)
{
    maxThreads += !maxThreads;
    Lzma2DecMt *decmt = malloc(sizeof(Lzma2DecMt) + (maxThreads - 1) * sizeof(ThreadInfo));
    if (!decmt)
        return NULL;
    decmt->head = FLzma2Dec_CreateInbufNode(NULL);
    decmt->factory = FL2POOL_create(maxThreads - 1);
    if (maxThreads > 1 && decmt->factory == NULL) {
        FL2_Lzma2DecMt_Free(decmt);
        return NULL;
    }
    decmt->numThreads = 0;
    decmt->maxThreads = maxThreads;
    for (size_t n = 0; n < maxThreads; ++n) {
        decmt->threads[n].outBuf = NULL;
        LzmaDec_Construct(&decmt->threads[n].dec);
    }
    FL2_Lzma2DecMt_Init(decmt);
    return decmt;
}

static int FL2_ParseMt(Lzma2DecMt* decmt, InputBlock* inBlock)
{
    int res = CHUNK_MORE_DATA;
    ChunkParseInfo inf;
    InBufNode* node = inBlock->last;
    int first = inBlock->unpackSize == 0;
    if (node == NULL)
        return res;
    while (inBlock->endPos < node->length) {
        res = FLzma2Dec_ParseInput(node->inBuf, inBlock->endPos, node->length - inBlock->endPos, &inf);
        if (first && res == CHUNK_DICT_RESET)
            res = CHUNK_CONTINUE;
        if (res != CHUNK_CONTINUE)
            break;
        inBlock->endPos += inf.packSize;
        inBlock->unpackSize += inf.unpackSize;
        first = 0;
    }
    inBlock->endPos += (res == CHUNK_FINAL);
    return res;
}

static size_t FL2_decompressBlockMt(FL2_DStream* fds, size_t thread)
{
    Lzma2DecMt *decmt = fds->decmt;
    ThreadInfo *ti = &decmt->threads[thread];
    CLzma2Dec *dec = &ti->dec;
    CHECK_F(FLzma2Dec_Init(dec, decmt->prop, ti->outBuf, ti->bufSize));

    InBufNode *node = ti->inBlock.first;
    size_t inPos = ti->inBlock.startPos;
    int last = (thread == fds->decmt->numThreads - 1);
    while (1) {
        size_t srcSize = node->length - inPos;
        size_t const res = FLzma2Dec_DecodeToDic(dec, ti->bufSize, node->inBuf + inPos, &srcSize, last && node == ti->inBlock.last ? LZMA_FINISH_END : LZMA_FINISH_ANY);

        if (FL2_isError(res))
            return res;
        if (res == LZMA_STATUS_FINISHED_WITH_MARK) {
            DEBUGLOG(4, "Found end mark");
        }
        if (node == ti->inBlock.last)
            break;
        inPos += srcSize;
        if (inPos + LZMA_REQUIRED_INPUT_MAX >= node->length) {
            inPos -= node->length - LZMA_REQUIRED_INPUT_MAX;
            node = node->next;
        }
    }
    return 0;
}

static size_t FL2_writeStreamBlocks(FL2_DStream* fds, FL2_outBuffer* output)
{
    Lzma2DecMt *decmt = fds->decmt;
    for (; decmt->srcThread < fds->decmt->numThreads; ++decmt->srcThread) {
        ThreadInfo *thread = decmt->threads + decmt->srcThread;
        size_t to_write = MIN(thread->bufSize - decmt->srcPos, output->size - output->pos);
        memcpy((BYTE*)output->dst + output->pos, thread->outBuf + decmt->srcPos, to_write);
#ifndef NO_XXHASH
        if (fds->doHash)
            XXH32_update(fds->xxh, (BYTE*)output->dst + output->pos, to_write);
#endif
        decmt->srcPos += to_write;
        output->pos += to_write;
        if (decmt->srcPos < thread->bufSize)
            break;
        decmt->srcPos = 0;
    }
    if (decmt->srcThread < fds->decmt->numThreads)
        return 0;
    FL2_FreeOutputBuffers(fds->decmt);
    fds->decmt->numThreads = 0;
    return 1;
}

/* FL2_decompressBlock() : FL2POOL_function type */
static void FL2_decompressBlock(void* const jobDescription, size_t n)
{
    FL2_DStream* const fds = (FL2_DStream*)jobDescription;
    fds->decmt->threads[n].res = FL2_decompressBlockMt(fds, n);
}

static size_t FL2_decompressBlocksMt(FL2_DStream* fds)
{
    Lzma2DecMt * const decmt = fds->decmt;
    for (size_t thread = 1; thread < fds->decmt->numThreads; ++thread) {
        FL2POOL_add(fds->decmt->factory, FL2_decompressBlock, fds, thread);
    }
    fds->decmt->threads[0].res = FL2_decompressBlockMt(fds, 0);
    FL2POOL_waitAll(fds->decmt->factory, 0);

    if (decmt->numThreads > 0) {
        InBufNode *keep = decmt->threads[decmt->numThreads - 1].inBlock.last;
        FLzma2Dec_FreeInbufNodeChain(decmt->head, keep);
        decmt->head = keep;
        decmt->threads[0].inBlock.first = keep;
        decmt->threads[0].inBlock.last = keep;
        decmt->threads[0].inBlock.endPos = decmt->threads[decmt->numThreads - 1].inBlock.endPos;
        decmt->threads[0].inBlock.startPos = decmt->threads[0].inBlock.endPos;
        decmt->threads[0].inBlock.unpackSize = 0;
    }

    if (FL2_isError(fds->decmt->threads[0].res))
        return fds->decmt->threads[0].res;

    fds->decmt->srcThread = 0;
    fds->decmt->srcPos = 0;

    return 0;
}

static size_t FL2_LoadInputMt(Lzma2DecMt *decmt, FL2_inBuffer* input)
{
    InputBlock *inBlock = &decmt->threads[decmt->numThreads].inBlock;
    int res = CHUNK_CONTINUE;
    while (input->pos < input->size || inBlock->endPos < inBlock->last->length) {
        if (inBlock->endPos < inBlock->last->length) {
            res = FL2_ParseMt(decmt, inBlock);
            if (res == CHUNK_ERROR)
                return FL2_ERROR(corruption_detected);
            if (res == CHUNK_DICT_RESET || res == CHUNK_FINAL) {
                ThreadInfo * const done = decmt->threads + decmt->numThreads;

                done->bufSize = done->inBlock.unpackSize;
                done->outBuf = malloc(done->bufSize);

                if (!done->outBuf)
                    return FL2_ERROR(memory_allocation);

                decmt->isFinal = (res == CHUNK_FINAL);
                if (decmt->isFinal) {
                    size_t rewind = inBlock->last->length - inBlock->endPos;
                    if(input->pos < rewind)
                        return FL2_ERROR(corruption_detected);
                    input->pos -= rewind;
                }
                ++decmt->numThreads;
                if (decmt->numThreads == decmt->maxThreads || res == CHUNK_FINAL)
                    return 1;

                inBlock = &decmt->threads[decmt->numThreads].inBlock;
                inBlock->first = done->inBlock.last;
                inBlock->last = inBlock->first;
                inBlock->endPos = done->inBlock.endPos;
                inBlock->startPos = inBlock->endPos;
                inBlock->unpackSize = 0;
            }
        }
        if (inBlock->last->length >= LZMA2_MT_INPUT_SIZE && inBlock->endPos + LZMA_REQUIRED_INPUT_MAX >= inBlock->last->length) {
            inBlock->last = FLzma2Dec_CreateInbufNode(inBlock->last);
            if (!inBlock->last)
                return FL2_ERROR(memory_allocation);
            inBlock->endPos -= LZMA2_MT_INPUT_SIZE - LZMA_REQUIRED_INPUT_MAX;
        }

        size_t toread = MIN(input->size - input->pos, LZMA2_MT_INPUT_SIZE - inBlock->last->length);
        memcpy(inBlock->last->inBuf + inBlock->last->length, (BYTE*)input->src + input->pos, toread);
        inBlock->last->length += toread;
        input->pos += toread;

        /* Do not continue if we have an incomplete chunk header */
        if (res == CHUNK_MORE_DATA && toread == 0)
            break;
    }
    return res == CHUNK_FINAL;
}

static size_t FL2_decompressStreamMt(FL2_DStream* fds, FL2_outBuffer* output, FL2_inBuffer* input)
{
    Lzma2DecMt *decmt = fds->decmt;
    if (fds->stage == FL2DEC_STAGE_DECOMP) {
        size_t res;
        res = FL2_LoadInputMt(decmt, input);
        CHECK_F(res);
        if (res > 0) {
            CHECK_F(FL2_decompressBlocksMt(fds));
            fds->stage = FL2DEC_STAGE_MT_WRITE;
        }
    }
    if (fds->stage == FL2DEC_STAGE_MT_WRITE) {
        if (FL2_writeStreamBlocks(fds, output))
            fds->stage = decmt->isFinal ? (fds->doHash ? FL2DEC_STAGE_HASH : FL2DEC_STAGE_FINISHED)
                : FL2DEC_STAGE_DECOMP;
    }
/*    if (fds->stage == FL2DEC_STAGE_HASH) {
#ifndef NO_XXHASH
        size_t to_read = MIN(XXHASH_SIZEOF - decmt->hashPos, input->size - input->pos);
        memcpy(decmt->hash.digest + decmt->hashPos, (BYTE*)input->src + input->pos, to_read);
        decmt->hashPos += to_read;
        if (decmt->hashPos == XXHASH_SIZEOF) {
            U32 hash;

            DEBUGLOG(4, "Checking hash");

            hash = XXH32_hashFromCanonical(&decmt->hash);
            if (hash != XXH32_digest(fds->xxh))
                return FL2_ERROR(checksum_wrong);
            fds->stage = FL2DEC_STAGE_FINISHED;
        }
#else
        fds->stage = FL2DEC_STAGE_FINISHED;
#endif
    }*/
    return fds->stage != FL2DEC_STAGE_FINISHED;
}
#endif

FL2LIB_API FL2_DStream* FL2LIB_CALL FL2_createDStream(void)
{
    return FL2_createDStreamMt(1);
}

FL2LIB_API FL2_DStream *FL2LIB_CALL FL2_createDStreamMt(unsigned nbThreads)
{
    FL2_DStream* const fds = malloc(sizeof(FL2_DStream));
    DEBUGLOG(3, "FL2_createDStream");

    if (fds != NULL) {
        LzmaDec_Construct(&fds->dec);

        nbThreads = FL2_checkNbThreads(nbThreads);

#ifndef FL2_SINGLETHREAD
        fds->decmt = (nbThreads > 1) ? FL2_Lzma2DecMt_Create(nbThreads) : NULL;
#endif

        fds->stage = FL2DEC_STAGE_INIT;
#ifndef NO_XXHASH
        fds->xxh = NULL;
#endif
        fds->doHash = 0;
        fds->loopCount = 0;
    }

    return fds;
}

FL2LIB_API size_t FL2LIB_CALL FL2_freeDStream(FL2_DStream* fds)
{
    if (fds != NULL) {
        DEBUGLOG(3, "FL2_freeDStream");
        FLzmaDec_Free(&fds->dec);
#ifndef NO_XXHASH
        XXH32_freeState(fds->xxh);
#endif
        free(fds);
    }
    return 0;
}

/*===== Streaming decompression functions =====*/
FL2LIB_API size_t FL2LIB_CALL FL2_initDStream(FL2_DStream* fds)
{
    DEBUGLOG(4, "FL2_initDStream");
    fds->stage = FL2DEC_STAGE_INIT;
    fds->loopCount = 0;
#ifndef FL2_SINGLETHREAD
    FL2_Lzma2DecMt_Init(fds->decmt);
#endif
    return FL2_error_no_error;
}

static size_t FL2_initDStream_prop(FL2_DStream* fds, BYTE prop)
{
    fds->doHash = prop >> FL2_PROP_HASH_BIT;
    prop &= FL2_LZMA_PROP_MASK;

#ifndef FL2_SINGLETHREAD
    if (fds->decmt)
        fds->decmt->prop = prop;
    else
#endif
        CHECK_F(FLzma2Dec_Init(&fds->dec, prop, NULL, 0));

#ifndef NO_XXHASH
    if (fds->doHash) {
        if (fds->xxh == NULL) {
            DEBUGLOG(3, "Creating hash state");
            fds->xxh = XXH32_createState();
            if (fds->xxh == NULL)
                return FL2_ERROR(memory_allocation);
        }
        XXH32_reset(fds->xxh, 0);
    }
#endif
    return FL2_error_no_error;
}

FL2LIB_API size_t FL2LIB_CALL FL2_initDStream_withProp(FL2_DStream* fds, unsigned char prop)
{
    FL2_initDStream(fds);
    FL2_initDStream_prop(fds, prop);
    fds->stage = FL2DEC_STAGE_DECOMP;
    return FL2_error_no_error;
}

FL2LIB_API size_t FL2LIB_CALL FL2_decompressStream(FL2_DStream* fds, FL2_outBuffer* output, FL2_inBuffer* input)
{
    size_t prevOut = output->pos;
    size_t prevIn = input->pos;

    if (input->pos < input->size
#ifndef FL2_SINGLETHREAD
        || fds->decmt
#endif
        ) {
        if (fds->stage == FL2DEC_STAGE_INIT) {
            BYTE prop = ((const BYTE*)input->src)[input->pos];
            ++input->pos;
            FL2_initDStream_prop(fds, prop);
            fds->stage = FL2DEC_STAGE_DECOMP;
        }
#ifndef FL2_SINGLETHREAD
        if (fds->decmt)
            FL2_decompressStreamMt(fds, output, input);
        else
#endif
        if (fds->stage == FL2DEC_STAGE_DECOMP) {
            size_t destSize = output->size - output->pos;
            size_t srcSize = input->size - input->pos;
            size_t const res = FLzma2Dec_DecodeToBuf(&fds->dec, (BYTE*)output->dst + output->pos, &destSize, (const BYTE*)input->src + input->pos, &srcSize, LZMA_FINISH_ANY);

            DEBUGLOG(5, "Decoded %u bytes", (U32)destSize);

#ifndef NO_XXHASH
            if(fds->doHash)
                XXH32_update(fds->xxh, (BYTE*)output->dst + output->pos, destSize);
#endif

            output->pos += destSize;
            input->pos += srcSize;

            if (FL2_isError(res))
                return res;
            if (res == LZMA_STATUS_FINISHED_WITH_MARK) {
                DEBUGLOG(4, "Found end mark");
                fds->stage = fds->doHash ? FL2DEC_STAGE_HASH : FL2DEC_STAGE_FINISHED;
            }
        }
        if (fds->stage == FL2DEC_STAGE_HASH) {
#ifndef NO_XXHASH
            XXH32_canonical_t canonical;
            U32 hash;

            DEBUGLOG(4, "Checking hash");

            if (input->size - input->pos >= XXHASH_SIZEOF) {
                memcpy(&canonical, (BYTE*)input->src + input->pos, XXHASH_SIZEOF);
                input->pos += XXHASH_SIZEOF;
                hash = XXH32_hashFromCanonical(&canonical);
                if (hash != XXH32_digest(fds->xxh))
                    return FL2_ERROR(checksum_wrong);
                fds->stage = FL2DEC_STAGE_FINISHED;
            }
#else
            fds->stage = FL2DEC_STAGE_FINISHED;
#endif
        }
    }
    if (fds->stage != FL2DEC_STAGE_FINISHED && prevOut == output->pos && prevIn == input->pos) {
        ++fds->loopCount;
        if (fds->loopCount > 1)
            return FL2_ERROR(infinite_loop);
    }
    else {
        fds->loopCount = 0;
    }
    return fds->stage != FL2DEC_STAGE_FINISHED;
}

FL2LIB_API size_t FL2LIB_CALL FL2_estimateDCtxSize(unsigned nbThreads)
{
    if (nbThreads > 1) {
        return nbThreads * (sizeof(BlockDecMt) + sizeof(FL2_DCtx));
    }
    return sizeof(FL2_DCtx);
}

FL2LIB_API size_t FL2LIB_CALL FL2_estimateDStreamSize(size_t dictSize, unsigned nbThreads)
{
    if (nbThreads > 1) {
        /* Estimate 50% compression and a block size of 4 * dictSize */
        return nbThreads * sizeof(FL2_DCtx) + (dictSize + dictSize / 2) * 4 * nbThreads;
    }
    return FLzma2Dec_MemUsage(dictSize);
}