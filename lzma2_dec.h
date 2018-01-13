/* lzma2_dec.h -- LZMA2 Decoder
2017-04-03 : Igor Pavlov : Public domain
Modified for FL2 by Conor McCarthy */

#ifndef __LZMA_DEC_H
#define __LZMA_DEC_H

#include "mem.h"

#if defined (__cplusplus)
extern "C" {
#endif

/* #define _LZMA_PROB32 */
/* _LZMA_PROB32 can increase the speed on some CPUs,
   but memory usage for CLzma2Dec::probs will be doubled in that case */

#ifdef _LZMA_PROB32
#define Probability U32
#else
#define Probability U16
#endif


/* ---------- LZMA Properties ---------- */

#define LZMA_PROPS_SIZE 5

typedef struct _CLzmaProps
{
  unsigned lc, lp, pb;
  U32 dicSize;
} CLzmaProps;

/* LzmaProps_Decode - decodes properties
Returns:
  SZ_OK
  SZ_ERROR_UNSUPPORTED - Unsupported properties
*/

size_t LzmaProps_Decode(CLzmaProps *p, const BYTE *data, unsigned size);


/* ---------- LZMA Decoder state ---------- */

/* LZMA_REQUIRED_INPUT_MAX = number of required input bytes for worst case.
   Num bits = log2((2^11 / 31) ^ 22) + 26 < 134 + 26 = 160; */

#define LZMA_REQUIRED_INPUT_MAX 20

#define kNumPosBitsMax 4
#define kNumPosStatesMax (1 << kNumPosBitsMax)

#define kLenNumLowBits 3
#define kLenNumLowSymbols (1 << kLenNumLowBits)
#define kLenNumMidBits 3
#define kLenNumMidSymbols (1 << kLenNumMidBits)
#define kLenNumHighBits 8
#define kLenNumHighSymbols (1 << kLenNumHighBits)

#define LenChoice 0
#define LenChoice2 (LenChoice + 1)
#define LenLow (LenChoice2 + 1)
#define LenMid (LenLow + (kNumPosStatesMax << kLenNumLowBits))
#define LenHigh (LenMid + (kNumPosStatesMax << kLenNumMidBits))
#define kNumLenProbs (LenHigh + kLenNumHighSymbols)


#define kNumStates 12
#define kNumLitStates 7

#define kStartPosModelIndex 4
#define kEndPosModelIndex 14
#define kNumFullDistances (1 << (kEndPosModelIndex >> 1))

#define kNumPosSlotBits 6
#define kNumLenToPosStates 4

#define kNumAlignBits 4
#define kAlignTableSize (1 << kNumAlignBits)

#define kMatchMinLen 2
#define kMatchSpecLenStart (kMatchMinLen + kLenNumLowSymbols + kLenNumMidSymbols + kLenNumHighSymbols)

#define IsMatch 0
#define IsRep (IsMatch + (kNumStates << kNumPosBitsMax))
#define IsRepG0 (IsRep + kNumStates)
#define IsRepG1 (IsRepG0 + kNumStates)
#define IsRepG2 (IsRepG1 + kNumStates)
#define IsRep0Long (IsRepG2 + kNumStates)
#define PosSlot (IsRep0Long + (kNumStates << kNumPosBitsMax))
#define SpecPos (PosSlot + (kNumLenToPosStates << kNumPosSlotBits))
#define Align (SpecPos + kNumFullDistances - kEndPosModelIndex)
#define LenCoder (Align + kAlignTableSize)
#define RepLenCoder (LenCoder + kNumLenProbs)
#define Literal (RepLenCoder + kNumLenProbs)

#define LZMA_BASE_SIZE 1846
#define LZMA_LIT_SIZE 0x300

#if Literal != LZMA_BASE_SIZE
StopCompilingDueBUG
#endif

#define LZMA2_LCLP_MAX 4

typedef struct CLzma2Dec_s
{
    CLzmaProps prop;
    BYTE *dic;
    const BYTE *buf;
    U32 range, code;
    size_t dicPos;
    size_t dicBufSize;
    U32 processedPos;
    U32 checkDicSize;
    unsigned state;
    U32 reps[4];
    unsigned remainLen;
    int needFlush;
    int needInitState;
    U32 numProbs;
    U32 packSize;
    U32 unpackSize;
    unsigned state2;
    BYTE control;
    BYTE needInitDic;
    BYTE needInitState2;
    BYTE needInitProp;
    BYTE extDic;
    Probability probs[Literal + ((U32)LZMA_LIT_SIZE << LZMA2_LCLP_MAX)];
} CLzma2Dec;

typedef struct
{
    U32 packSize;
    U32 unpackSize;
    CLzmaProps prop;
} ChunkInfo;

void LzmaDec_Construct(CLzma2Dec *p);

void LzmaDec_Init(CLzma2Dec *p);

/* There are two types of LZMA streams:
     0) Stream with end mark. That end mark adds about 6 bytes to compressed size.
     1) Stream without end mark. You must know exact uncompressed size to decompress such stream. */

typedef enum
{
  LZMA_FINISH_ANY,   /* finish at any point */
  LZMA_FINISH_END    /* block must be finished at the end */
} ELzmaFinishMode;

/* ELzmaFinishMode has meaning only if the decoding reaches output limit !!!

   You must use LZMA_FINISH_END, when you know that current output buffer
   covers last bytes of block. In other cases you must use LZMA_FINISH_ANY.

   If LZMA decoder sees end marker before reaching output limit, it returns SZ_OK,
   and output value of destLen will be less than output buffer size limit.
   You can check status result also.

   You can use multiple checks to test data integrity after full decompression:
     1) Check Result and "status" variable.
     2) Check that output(destLen) = uncompressedSize, if you know real uncompressedSize.
     3) Check that output(srcLen) = compressedSize, if you know real compressedSize.
        You must use correct finish mode in that case. */

typedef enum
{
  LZMA_STATUS_NOT_SPECIFIED,               /* use main error code instead */
  LZMA_STATUS_FINISHED_WITH_MARK,          /* stream was finished with end mark. */
  LZMA_STATUS_NOT_FINISHED,                /* stream was not finished */
  LZMA_STATUS_NEEDS_MORE_INPUT,            /* you must provide more input bytes */
  LZMA_STATUS_MAYBE_FINISHED_WITHOUT_MARK  /* there is probability that stream was finished without end mark */
} ELzmaStatus;

/* ELzmaStatus is used only as output value for function call */


/* ---------- Interfaces ---------- */

/* There are 3 levels of interfaces:
     1) Dictionary Interface
     2) Buffer Interface
     3) One Call Interface
   You can select any of these interfaces, but don't mix functions from different
   groups for same object. */


/* There are two variants to allocate state for Dictionary Interface:
     1) LzmaDec_Allocate / LzmaDec_Free
     2) LzmaDec_AllocateProbs / LzmaDec_FreeProbs
   You can use variant 2, if you set dictionary buffer manually.
   For Buffer Interface you must always use variant 1.

LzmaDec_Allocate* can return:
  SZ_OK
  SZ_ERROR_MEM         - Memory allocation error
  SZ_ERROR_UNSUPPORTED - Unsupported properties
*/
   
size_t LzmaDec_AllocateProbs(CLzma2Dec *p, const BYTE *props, unsigned propsSize);

size_t LzmaDec_Allocate(CLzma2Dec *state, const BYTE *prop, unsigned propsSize);
void LzmaDec_Free(CLzma2Dec *state);

/* ---------- Dictionary Interface ---------- */

/* You can use it, if you want to eliminate the overhead for data copying from
   dictionary to some other external buffer.
   You must work with CLzma2Dec variables directly in this interface.

   STEPS:
     LzmaDec_Constr()
     LzmaDec_Allocate()
     for (each new stream)
     {
       LzmaDec_Init()
       while (it needs more decompression)
       {
         LzmaDec_DecodeToDic()
         use data from CLzma2Dec::dic and update CLzma2Dec::dicPos
       }
     }
     LzmaDec_Free()
*/

/* LzmaDec_DecodeToDic
   
   The decoding to internal dictionary buffer (CLzma2Dec::dic).
   You must manually update CLzma2Dec::dicPos, if it reaches CLzma2Dec::dicBufSize !!!

finishMode:
  It has meaning only if the decoding reaches output limit (dicLimit).
  LZMA_FINISH_ANY - Decode just dicLimit bytes.
  LZMA_FINISH_END - Stream must be finished after dicLimit.

Returns:
  SZ_OK
    status:
      LZMA_STATUS_FINISHED_WITH_MARK
      LZMA_STATUS_NOT_FINISHED
      LZMA_STATUS_NEEDS_MORE_INPUT
      LZMA_STATUS_MAYBE_FINISHED_WITHOUT_MARK
  SZ_ERROR_DATA - Data error
*/

size_t LzmaDec_DecodeToDic(CLzma2Dec *p, size_t dicLimit,
    const BYTE *src, size_t *srcLen, ELzmaFinishMode finishMode);


/* ---------- Buffer Interface ---------- */

/* It's zlib-like interface.
   See LzmaDec_DecodeToDic description for information about STEPS and return results,
   but you must use LzmaDec_DecodeToBuf instead of LzmaDec_DecodeToDic and you don't need
   to work with CLzma2Dec variables manually.

finishMode:
  It has meaning only if the decoding reaches output limit (*destLen).
  LZMA_FINISH_ANY - Decode just destLen bytes.
  LZMA_FINISH_END - Stream must be finished after (*destLen).
*/

size_t LzmaDec_DecodeToBuf(CLzma2Dec *p, BYTE *dest, size_t *destLen,
    const BYTE *src, size_t *srcLen, ELzmaFinishMode finishMode);

#define LZMA2_CONTENTSIZE_UNKNOWN (size_t)-1
#define LZMA2_CONTENTSIZE_ERROR   (size_t)-2

size_t Lzma2Dec_UnpackSize(const BYTE *src, size_t srcLen);

/* ---------- One Call Interface ---------- */

/* LzmaDecode

finishMode:
  It has meaning only if the decoding reaches output limit (*destLen).
  LZMA_FINISH_ANY - Decode just destLen bytes.
  LZMA_FINISH_END - Stream must be finished after (*destLen).

Returns:
  SZ_OK
    status:
      LZMA_STATUS_FINISHED_WITH_MARK
      LZMA_STATUS_NOT_FINISHED
      LZMA_STATUS_MAYBE_FINISHED_WITHOUT_MARK
  SZ_ERROR_DATA - Data error
  SZ_ERROR_MEM  - Memory allocation error
  SZ_ERROR_UNSUPPORTED - Unsupported properties
  SZ_ERROR_INPUT_EOF - It needs more bytes in input buffer (src).
*/

size_t LzmaDecode(BYTE *dest, size_t *destLen, const BYTE *src, size_t *srcLen,
    const BYTE *propData, unsigned propSize, ELzmaFinishMode finishMode);

/* ---------- State Interface ---------- */

size_t Lzma2Dec_Init(CLzma2Dec *p, BYTE dictProp, BYTE *dic, size_t dicBufSize);


/*
finishMode:
It has meaning only if the decoding reaches output limit (*destLen or dicLimit).
LZMA_FINISH_ANY - use smallest number of input bytes
LZMA_FINISH_END - read EndOfStream marker after decoding

Returns:
SZ_OK
status:
LZMA_STATUS_FINISHED_WITH_MARK
LZMA_STATUS_NOT_FINISHED
LZMA_STATUS_NEEDS_MORE_INPUT
SZ_ERROR_DATA - Data error
*/

size_t Lzma2Dec_DecodeToDic(CLzma2Dec *p, size_t dicLimit,
    const BYTE *src, size_t *srcLen, ELzmaFinishMode finishMode);

size_t Lzma2Dec_DecodeToBuf(CLzma2Dec *p, BYTE *dest, size_t *destLen,
    const BYTE *src, size_t *srcLen, ELzmaFinishMode finishMode);


/* ---------- One Call Interface ---------- */

/*
finishMode:
It has meaning only if the decoding reaches output limit (*destLen).
LZMA_FINISH_ANY - use smallest number of input bytes
LZMA_FINISH_END - read EndOfStream marker after decoding

Returns:
SZ_OK
status:
LZMA_STATUS_FINISHED_WITH_MARK
LZMA_STATUS_NOT_FINISHED
SZ_ERROR_DATA - Data error
SZ_ERROR_MEM  - Memory allocation error
SZ_ERROR_UNSUPPORTED - Unsupported properties
SZ_ERROR_INPUT_EOF - It needs more bytes in input buffer (src).
*/

size_t Lzma2Decode(BYTE *dest, size_t *destLen, const BYTE *src, size_t *srcLen,
    BYTE prop, ELzmaFinishMode finishMode);

#if defined (__cplusplus)
}
#endif

#endif
