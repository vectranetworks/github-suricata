/* Copyright (C) 2007-2022 Open Information Security Foundation
 *
 * You can copy, redistribute or modify this Program under the terms of
 * the GNU General Public License version 2 as published by the Free
 * Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * version 2 along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 */

/**
 * \file
 *
 * \author Anoop Saldanha <anoopsaldanha@gmail.com>
 */

#include "suricata-common.h"
#include "detect-engine.h"
#include "detect-engine-build.h"
#include "detect-engine-prefilter.h"
#include "detect-engine-prefilter-common.h"
#include "detect-parse.h"
#include "detect-app-layer-protocol.h"
#include "app-layer.h"
#include "app-layer-parser.h"
#include "util-debug.h"
#include "util-unittest.h"
#include "util-unittest-helper.h"

#ifdef UNITTESTS
static void DetectAppLayerProtocolRegisterTests(void);
#endif

typedef struct DetectAppLayerProtocolData_ {
    AppProto alproto;
    uint8_t negated;
} DetectAppLayerProtocolData;

static int DetectAppLayerProtocolPacketMatch(
        DetectEngineThreadCtx *det_ctx,
        Packet *p, const Signature *s, const SigMatchCtx *ctx)
{
    SCEnter();

    bool r = false;
    const DetectAppLayerProtocolData *data = (const DetectAppLayerProtocolData *)ctx;

    /* if the sig is PD-only we only match when PD packet flags are set */
    if (s->type == SIG_TYPE_PDONLY &&
            (p->flags & (PKT_PROTO_DETECT_TS_DONE | PKT_PROTO_DETECT_TC_DONE)) == 0) {
        SCLogDebug("packet %"PRIu64": flags not set", p->pcap_cnt);
        SCReturnInt(0);
    }

    const Flow *f = p->flow;
    if (f == NULL) {
        SCLogDebug("packet %"PRIu64": no flow", p->pcap_cnt);
        SCReturnInt(0);
    }

    /* unknown means protocol detection isn't ready yet */

    if ((f->alproto_ts != ALPROTO_UNKNOWN) && (p->flowflags & FLOW_PKT_TOSERVER))
    {
        SCLogDebug("toserver packet %"PRIu64": looking for %u/neg %u, got %u",
                p->pcap_cnt, data->alproto, data->negated, f->alproto_ts);

        r = AppProtoEquals(data->alproto, f->alproto_ts);

    } else if ((f->alproto_tc != ALPROTO_UNKNOWN) && (p->flowflags & FLOW_PKT_TOCLIENT))
    {
        SCLogDebug("toclient packet %"PRIu64": looking for %u/neg %u, got %u",
                p->pcap_cnt, data->alproto, data->negated, f->alproto_tc);

        r = AppProtoEquals(data->alproto, f->alproto_tc);
    }
    else {
        SCLogDebug("packet %"PRIu64": default case: direction %02x, approtos %u/%u/%u",
            p->pcap_cnt,
            p->flowflags & (FLOW_PKT_TOCLIENT|FLOW_PKT_TOSERVER),
            f->alproto, f->alproto_ts, f->alproto_tc);
        SCReturnInt(0);
    }
    r = r ^ data->negated;
    if (r) {
        SCReturnInt(1);
    }
    SCReturnInt(0);
}

static DetectAppLayerProtocolData *DetectAppLayerProtocolParse(const char *arg, bool negate)
{
    DetectAppLayerProtocolData *data;
    AppProto alproto = ALPROTO_UNKNOWN;

    if (strcmp(arg, "failed") == 0) {
        alproto = ALPROTO_FAILED;
    } else {
        alproto = AppLayerGetProtoByName((char *)arg);
        if (alproto == ALPROTO_UNKNOWN) {
            SCLogError("app-layer-protocol "
                       "keyword supplied with unknown protocol \"%s\"",
                    arg);
            return NULL;
        }
    }

    data = SCMalloc(sizeof(DetectAppLayerProtocolData));
    if (unlikely(data == NULL))
        return NULL;
    data->alproto = alproto;
    data->negated = negate;

    return data;
}

static bool HasConflicts(const DetectAppLayerProtocolData *us,
                          const DetectAppLayerProtocolData *them)
{
    /* mixing negated and non negated is illegal */
    if (them->negated ^ us->negated)
        return true;
    /* multiple non-negated is illegal */
    if (!us->negated)
        return true;
    /* duplicate option */
    if (us->alproto == them->alproto)
        return true;

    /* all good */
    return false;
}

static int DetectAppLayerProtocolSetup(DetectEngineCtx *de_ctx,
        Signature *s, const char *arg)
{
    DetectAppLayerProtocolData *data = NULL;
    SigMatch *sm = NULL;

    if (s->alproto != ALPROTO_UNKNOWN) {
        SCLogError("Either we already "
                   "have the rule match on an app layer protocol set through "
                   "other keywords that match on this protocol, or have "
                   "already seen a non-negated app-layer-protocol.");
        goto error;
    }

    data = DetectAppLayerProtocolParse(arg, s->init_data->negated);
    if (data == NULL)
        goto error;

    SigMatch *tsm = s->init_data->smlists[DETECT_SM_LIST_MATCH];
    for ( ; tsm != NULL; tsm = tsm->next) {
        if (tsm->type == DETECT_AL_APP_LAYER_PROTOCOL) {
            const DetectAppLayerProtocolData *them = (const DetectAppLayerProtocolData *)tsm->ctx;

            if (HasConflicts(data, them)) {
                SCLogError("can't mix "
                           "positive app-layer-protocol match with negated "
                           "match or match for 'failed'.");
                goto error;
            }
        }
    }

    sm = SigMatchAlloc();
    if (sm == NULL)
        goto error;

    sm->type = DETECT_AL_APP_LAYER_PROTOCOL;
    sm->ctx = (void *)data;

    SigMatchAppendSMToList(s, sm, DETECT_SM_LIST_MATCH);
    return 0;

error:
    if (data != NULL)
        SCFree(data);
    return -1;
}

static void DetectAppLayerProtocolFree(DetectEngineCtx *de_ctx, void *ptr)
{
    SCFree(ptr);
    return;
}

/** \internal
 *  \brief prefilter function for protocol detect matching
 */
static void
PrefilterPacketAppProtoMatch(DetectEngineThreadCtx *det_ctx, Packet *p, const void *pectx)
{
    const PrefilterPacketHeaderCtx *ctx = pectx;

    if (!PrefilterPacketHeaderExtraMatch(ctx, p)) {
        SCLogDebug("packet %"PRIu64": extra match failed", p->pcap_cnt);
        SCReturn;
    }

    if (p->flow == NULL) {
        SCLogDebug("packet %"PRIu64": no flow, no alproto", p->pcap_cnt);
        SCReturn;
    }

    if ((p->flags & (PKT_PROTO_DETECT_TS_DONE|PKT_PROTO_DETECT_TC_DONE)) == 0) {
        SCLogDebug("packet %"PRIu64": flags not set", p->pcap_cnt);
        SCReturn;
    }

    if ((p->flags & PKT_PROTO_DETECT_TS_DONE) && (p->flowflags & FLOW_PKT_TOSERVER) &&
            p->flow->alproto_ts != ALPROTO_UNKNOWN) {
        int r = AppProtoEquals(ctx->v1.u16[0], p->flow->alproto_ts) ^ ctx->v1.u8[2];
        if (r) {
            PrefilterAddSids(&det_ctx->pmq, ctx->sigs_array, ctx->sigs_cnt);
        }
    } else if ((p->flags & PKT_PROTO_DETECT_TC_DONE) && (p->flowflags & FLOW_PKT_TOCLIENT) &&
               p->flow->alproto_tc != ALPROTO_UNKNOWN) {
        int r = AppProtoEquals(ctx->v1.u16[0], p->flow->alproto_tc) ^ ctx->v1.u8[2];
        if (r) {
            PrefilterAddSids(&det_ctx->pmq, ctx->sigs_array, ctx->sigs_cnt);
        }
    }
}

static void
PrefilterPacketAppProtoSet(PrefilterPacketHeaderValue *v, void *smctx)
{
    const DetectAppLayerProtocolData *a = smctx;
    v->u16[0] = a->alproto;
    v->u8[2] = (uint8_t)a->negated;
}

static bool
PrefilterPacketAppProtoCompare(PrefilterPacketHeaderValue v, void *smctx)
{
    const DetectAppLayerProtocolData *a = smctx;
    if (v.u16[0] == a->alproto &&
        v.u8[2] == (uint8_t)a->negated)
        return true;
    return false;
}

static int PrefilterSetupAppProto(DetectEngineCtx *de_ctx, SigGroupHead *sgh)
{
    return PrefilterSetupPacketHeader(de_ctx, sgh, DETECT_AL_APP_LAYER_PROTOCOL,
        PrefilterPacketAppProtoSet,
        PrefilterPacketAppProtoCompare,
        PrefilterPacketAppProtoMatch);
}

static bool PrefilterAppProtoIsPrefilterable(const Signature *s)
{
    if (s->type == SIG_TYPE_PDONLY) {
        SCLogDebug("prefilter on PD %u", s->id);
        return true;
    }
    return false;
}

void DetectAppLayerProtocolRegister(void)
{
    sigmatch_table[DETECT_AL_APP_LAYER_PROTOCOL].name = "app-layer-protocol";
    sigmatch_table[DETECT_AL_APP_LAYER_PROTOCOL].desc = "match on the detected app-layer protocol";
    sigmatch_table[DETECT_AL_APP_LAYER_PROTOCOL].url = "/rules/app-layer.html#app-layer-protocol";
    sigmatch_table[DETECT_AL_APP_LAYER_PROTOCOL].Match =
        DetectAppLayerProtocolPacketMatch;
    sigmatch_table[DETECT_AL_APP_LAYER_PROTOCOL].Setup =
        DetectAppLayerProtocolSetup;
    sigmatch_table[DETECT_AL_APP_LAYER_PROTOCOL].Free =
        DetectAppLayerProtocolFree;
#ifdef UNITTESTS
    sigmatch_table[DETECT_AL_APP_LAYER_PROTOCOL].RegisterTests =
        DetectAppLayerProtocolRegisterTests;
#endif
    sigmatch_table[DETECT_AL_APP_LAYER_PROTOCOL].flags =
        (SIGMATCH_QUOTES_OPTIONAL|SIGMATCH_HANDLE_NEGATION);

    sigmatch_table[DETECT_AL_APP_LAYER_PROTOCOL].SetupPrefilter =
        PrefilterSetupAppProto;
    sigmatch_table[DETECT_AL_APP_LAYER_PROTOCOL].SupportsPrefilter =
        PrefilterAppProtoIsPrefilterable;
    return;
}

/**********************************Unittests***********************************/

#ifdef UNITTESTS

static int DetectAppLayerProtocolTest01(void)
{
    DetectAppLayerProtocolData *data = DetectAppLayerProtocolParse("http", false);
    FAIL_IF_NULL(data);
    FAIL_IF(data->alproto != ALPROTO_HTTP);
    FAIL_IF(data->negated != 0);
    DetectAppLayerProtocolFree(NULL, data);
    PASS;
}

static int DetectAppLayerProtocolTest02(void)
{
    DetectAppLayerProtocolData *data = DetectAppLayerProtocolParse("http", true);
    FAIL_IF_NULL(data);
    FAIL_IF(data->alproto != ALPROTO_HTTP);
    FAIL_IF(data->negated == 0);
    DetectAppLayerProtocolFree(NULL, data);
    PASS;
}

static int DetectAppLayerProtocolTest03(void)
{
    Signature *s = NULL;
    DetectAppLayerProtocolData *data = NULL;
    DetectEngineCtx *de_ctx = DetectEngineCtxInit();
    FAIL_IF_NULL(de_ctx);
    de_ctx->flags |= DE_QUIET;

    s = DetectEngineAppendSig(de_ctx, "alert tcp any any -> any any "
            "(app-layer-protocol:http; sid:1;)");
    FAIL_IF_NULL(s);

    FAIL_IF(s->alproto != ALPROTO_UNKNOWN);

    FAIL_IF_NULL(s->init_data->smlists[DETECT_SM_LIST_MATCH]);
    FAIL_IF_NULL(s->init_data->smlists[DETECT_SM_LIST_MATCH]->ctx);

    data = (DetectAppLayerProtocolData *)s->init_data->smlists[DETECT_SM_LIST_MATCH]->ctx;
    FAIL_IF(data->alproto != ALPROTO_HTTP);
    FAIL_IF(data->negated);
    DetectEngineCtxFree(de_ctx);
    PASS;
}

static int DetectAppLayerProtocolTest04(void)
{
    Signature *s = NULL;
    DetectAppLayerProtocolData *data = NULL;
    DetectEngineCtx *de_ctx = DetectEngineCtxInit();
    FAIL_IF_NULL(de_ctx);
    de_ctx->flags |= DE_QUIET;

    s = DetectEngineAppendSig(de_ctx, "alert tcp any any -> any any "
            "(app-layer-protocol:!http; sid:1;)");
    FAIL_IF_NULL(s);
    FAIL_IF(s->alproto != ALPROTO_UNKNOWN);
    FAIL_IF(s->flags & SIG_FLAG_APPLAYER);

    FAIL_IF_NULL(s->init_data->smlists[DETECT_SM_LIST_MATCH]);
    FAIL_IF_NULL(s->init_data->smlists[DETECT_SM_LIST_MATCH]->ctx);

    data = (DetectAppLayerProtocolData *)s->init_data->smlists[DETECT_SM_LIST_MATCH]->ctx;
    FAIL_IF_NULL(data);
    FAIL_IF(data->alproto != ALPROTO_HTTP);
    FAIL_IF(data->negated == 0);

    DetectEngineCtxFree(de_ctx);
    PASS;
}

static int DetectAppLayerProtocolTest05(void)
{
    Signature *s = NULL;
    DetectAppLayerProtocolData *data = NULL;
    DetectEngineCtx *de_ctx = DetectEngineCtxInit();
    FAIL_IF_NULL(de_ctx);
    de_ctx->flags |= DE_QUIET;

    s = DetectEngineAppendSig(de_ctx, "alert tcp any any -> any any "
            "(app-layer-protocol:!http; app-layer-protocol:!smtp; sid:1;)");
    FAIL_IF_NULL(s);
    FAIL_IF(s->alproto != ALPROTO_UNKNOWN);
    FAIL_IF(s->flags & SIG_FLAG_APPLAYER);

    FAIL_IF_NULL(s->init_data->smlists[DETECT_SM_LIST_MATCH]);
    FAIL_IF_NULL(s->init_data->smlists[DETECT_SM_LIST_MATCH]->ctx);

    data = (DetectAppLayerProtocolData *)s->init_data->smlists[DETECT_SM_LIST_MATCH]->ctx;
    FAIL_IF_NULL(data);
    FAIL_IF(data->alproto != ALPROTO_HTTP);
    FAIL_IF(data->negated == 0);

    data = (DetectAppLayerProtocolData *)s->init_data->smlists[DETECT_SM_LIST_MATCH]->next->ctx;
    FAIL_IF_NULL(data);
    FAIL_IF(data->alproto != ALPROTO_SMTP);
    FAIL_IF(data->negated == 0);

    DetectEngineCtxFree(de_ctx);
    PASS;
}

static int DetectAppLayerProtocolTest06(void)
{
    Signature *s = NULL;
    DetectEngineCtx *de_ctx = DetectEngineCtxInit();
    FAIL_IF_NULL(de_ctx);
    de_ctx->flags |= DE_QUIET;

    s = DetectEngineAppendSig(de_ctx, "alert http any any -> any any "
            "(app-layer-protocol:smtp; sid:1;)");
    FAIL_IF_NOT_NULL(s);
    DetectEngineCtxFree(de_ctx);
    PASS;
}

static int DetectAppLayerProtocolTest07(void)
{
    Signature *s = NULL;
    DetectEngineCtx *de_ctx = DetectEngineCtxInit();
    FAIL_IF_NULL(de_ctx);
    de_ctx->flags |= DE_QUIET;

    s = DetectEngineAppendSig(de_ctx, "alert http any any -> any any "
            "(app-layer-protocol:!smtp; sid:1;)");
    FAIL_IF_NOT_NULL(s);
    DetectEngineCtxFree(de_ctx);
    PASS;
}

static int DetectAppLayerProtocolTest08(void)
{
    Signature *s = NULL;
    DetectEngineCtx *de_ctx = DetectEngineCtxInit();
    FAIL_IF_NULL(de_ctx);
    de_ctx->flags |= DE_QUIET;

    s = DetectEngineAppendSig(de_ctx, "alert tcp any any -> any any "
            "(app-layer-protocol:!smtp; app-layer-protocol:http; sid:1;)");
    FAIL_IF_NOT_NULL(s);
    DetectEngineCtxFree(de_ctx);
    PASS;
}

static int DetectAppLayerProtocolTest09(void)
{
    Signature *s = NULL;
    DetectEngineCtx *de_ctx = DetectEngineCtxInit();
    FAIL_IF_NULL(de_ctx);
    de_ctx->flags |= DE_QUIET;

    s = DetectEngineAppendSig(de_ctx, "alert tcp any any -> any any "
            "(app-layer-protocol:http; app-layer-protocol:!smtp; sid:1;)");
    FAIL_IF_NOT_NULL(s);
    DetectEngineCtxFree(de_ctx);
    PASS;
}

static int DetectAppLayerProtocolTest10(void)
{
    Signature *s = NULL;
    DetectEngineCtx *de_ctx = DetectEngineCtxInit();
    FAIL_IF_NULL(de_ctx);
    de_ctx->flags |= DE_QUIET;

    s = DetectEngineAppendSig(de_ctx, "alert tcp any any -> any any "
            "(app-layer-protocol:smtp; app-layer-protocol:!http; sid:1;)");
    FAIL_IF_NOT_NULL(s);
    DetectEngineCtxFree(de_ctx);
    PASS;
}

static int DetectAppLayerProtocolTest11(void)
{
    DetectAppLayerProtocolData *data = DetectAppLayerProtocolParse("failed", false);
    FAIL_IF_NULL(data);
    FAIL_IF(data->alproto != ALPROTO_FAILED);
    FAIL_IF(data->negated != 0);
    DetectAppLayerProtocolFree(NULL, data);
    PASS;
}

static int DetectAppLayerProtocolTest12(void)
{
    DetectAppLayerProtocolData *data = DetectAppLayerProtocolParse("failed", true);
    FAIL_IF_NULL(data);
    FAIL_IF(data->alproto != ALPROTO_FAILED);
    FAIL_IF(data->negated == 0);
    DetectAppLayerProtocolFree(NULL, data);
    PASS;
}

static int DetectAppLayerProtocolTest13(void)
{
    Signature *s = NULL;
    DetectAppLayerProtocolData *data = NULL;
    DetectEngineCtx *de_ctx = DetectEngineCtxInit();
    FAIL_IF_NULL(de_ctx);
    de_ctx->flags |= DE_QUIET;

    s = DetectEngineAppendSig(de_ctx, "alert tcp any any -> any any "
            "(app-layer-protocol:failed; sid:1;)");
    FAIL_IF_NULL(s);

    FAIL_IF(s->alproto != ALPROTO_UNKNOWN);

    FAIL_IF_NULL(s->init_data->smlists[DETECT_SM_LIST_MATCH]);
    FAIL_IF_NULL(s->init_data->smlists[DETECT_SM_LIST_MATCH]->ctx);

    data = (DetectAppLayerProtocolData *)s->init_data->smlists[DETECT_SM_LIST_MATCH]->ctx;
    FAIL_IF(data->alproto != ALPROTO_FAILED);
    FAIL_IF(data->negated);
    DetectEngineCtxFree(de_ctx);
    PASS;
}

static int DetectAppLayerProtocolTest14(void)
{
    DetectAppLayerProtocolData *data = NULL;
    DetectEngineCtx *de_ctx = DetectEngineCtxInit();
    FAIL_IF_NULL(de_ctx);
    de_ctx->flags |= DE_QUIET;

    Signature *s1 = DetectEngineAppendSig(de_ctx, "alert tcp any any -> any any "
            "(app-layer-protocol:http; flowbits:set,blah; sid:1;)");
    FAIL_IF_NULL(s1);
    FAIL_IF(s1->alproto != ALPROTO_UNKNOWN);
    FAIL_IF_NULL(s1->init_data->smlists[DETECT_SM_LIST_MATCH]);
    FAIL_IF_NULL(s1->init_data->smlists[DETECT_SM_LIST_MATCH]->ctx);
    data = (DetectAppLayerProtocolData *)s1->init_data->smlists[DETECT_SM_LIST_MATCH]->ctx;
    FAIL_IF(data->alproto != ALPROTO_HTTP);
    FAIL_IF(data->negated);

    Signature *s2 = DetectEngineAppendSig(de_ctx, "alert tcp any any -> any any "
            "(app-layer-protocol:http; flow:to_client; sid:2;)");
    FAIL_IF_NULL(s2);
    FAIL_IF(s2->alproto != ALPROTO_UNKNOWN);
    FAIL_IF_NULL(s2->init_data->smlists[DETECT_SM_LIST_MATCH]);
    FAIL_IF_NULL(s2->init_data->smlists[DETECT_SM_LIST_MATCH]->ctx);
    data = (DetectAppLayerProtocolData *)s2->init_data->smlists[DETECT_SM_LIST_MATCH]->ctx;
    FAIL_IF(data->alproto != ALPROTO_HTTP);
    FAIL_IF(data->negated);

    /* flow:established and other options not supported for PD-only */
    Signature *s3 = DetectEngineAppendSig(de_ctx, "alert tcp any any -> any any "
            "(app-layer-protocol:http; flow:to_client,established; sid:3;)");
    FAIL_IF_NULL(s3);
    FAIL_IF(s3->alproto != ALPROTO_UNKNOWN);
    FAIL_IF_NULL(s3->init_data->smlists[DETECT_SM_LIST_MATCH]);
    FAIL_IF_NULL(s3->init_data->smlists[DETECT_SM_LIST_MATCH]->ctx);
    data = (DetectAppLayerProtocolData *)s3->init_data->smlists[DETECT_SM_LIST_MATCH]->ctx;
    FAIL_IF(data->alproto != ALPROTO_HTTP);
    FAIL_IF(data->negated);

    SigGroupBuild(de_ctx);
    FAIL_IF_NOT(s1->type == SIG_TYPE_PDONLY);
    FAIL_IF_NOT(s2->type == SIG_TYPE_PDONLY);
    FAIL_IF(s3->type == SIG_TYPE_PDONLY); // failure now

    DetectEngineCtxFree(de_ctx);
    PASS;
}


static void DetectAppLayerProtocolRegisterTests(void)
{
    UtRegisterTest("DetectAppLayerProtocolTest01",
                   DetectAppLayerProtocolTest01);
    UtRegisterTest("DetectAppLayerProtocolTest02",
                   DetectAppLayerProtocolTest02);
    UtRegisterTest("DetectAppLayerProtocolTest03",
                   DetectAppLayerProtocolTest03);
    UtRegisterTest("DetectAppLayerProtocolTest04",
                   DetectAppLayerProtocolTest04);
    UtRegisterTest("DetectAppLayerProtocolTest05",
                   DetectAppLayerProtocolTest05);
    UtRegisterTest("DetectAppLayerProtocolTest06",
                   DetectAppLayerProtocolTest06);
    UtRegisterTest("DetectAppLayerProtocolTest07",
                   DetectAppLayerProtocolTest07);
    UtRegisterTest("DetectAppLayerProtocolTest08",
                   DetectAppLayerProtocolTest08);
    UtRegisterTest("DetectAppLayerProtocolTest09",
                   DetectAppLayerProtocolTest09);
    UtRegisterTest("DetectAppLayerProtocolTest10",
                   DetectAppLayerProtocolTest10);
    UtRegisterTest("DetectAppLayerProtocolTest11",
                   DetectAppLayerProtocolTest11);
    UtRegisterTest("DetectAppLayerProtocolTest12",
                   DetectAppLayerProtocolTest12);
    UtRegisterTest("DetectAppLayerProtocolTest13",
                   DetectAppLayerProtocolTest13);
    UtRegisterTest("DetectAppLayerProtocolTest14",
                   DetectAppLayerProtocolTest14);
}
#endif /* UNITTESTS */
