#include "logger.h"
#include "onem2m.h"
#include "dbmanager.h"
#include "httpd.h"
#include "cJSON.h"
#include "util.h"
#include "config.h"
#include "onem2mTypes.h"
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#ifdef UPPERTESTER

#define LOG_TAG "UT"

/**
 * @brief Handle the special Upper Tester "Reset" command (TS-0019 table 5.4.4.2.2-2).
 *
 * Brings the IUT back to its initial state by wiping every stored resource and
 * rebuilding the CSE. The UtTriggerAck may only carry a response status code,
 * restricted to 2000 (OK) or 4000 (BAD_REQUEST) - see TS-0019 table 5.4.4.2.2-3.
 */
static int handle_ut_reset(oneM2MPrimitive *o2pt)
{
    logger(LOG_TAG, LOG_LEVEL_INFO, "UT command: Reset");

    int rc = reset_cse();
/* ========== DEBUG TRACE — delete this line later ========== */
    logger(LOG_TAG, LOG_LEVEL_INFO, "[DBG] UT Reset: reset_cse() returned %d", rc);
/* ======================================================== */

    if (rc != 0)
    {
        return handle_error(o2pt, RSC_BAD_REQUEST, "reset failed");
    }

    o2pt->rsc = RSC_OK;
    if (o2pt->response_pc)
        cJSON_Delete(o2pt->response_pc);
    o2pt->response_pc = NULL;
    return 0;
}

int handle_uppertester_procedure(oneM2MPrimitive *o2pt)
{
    cJSON *pjson = NULL;
    logger(LOG_TAG, LOG_LEVEL_INFO, "handle_uppertester_procedure");

/* ========== DEBUG TRACE (what reached the UT handler) — delete this block later ========== */
    {
        char *pc_str = o2pt->request_pc ? cJSON_PrintUnformatted(o2pt->request_pc) : NULL;
        logger(LOG_TAG, LOG_LEVEL_INFO,
               "[DBG] entry: utcmd=%s fr=%s rqi=%s rvi=%d body=%s",
               o2pt->utcmd ? o2pt->utcmd : "(none)",
               o2pt->fr ? o2pt->fr : "(none)",
               o2pt->rqi ? o2pt->rqi : "(none)",
               o2pt->rvi,
               pc_str ? pc_str : "(none)");
        if (pc_str)
            free(pc_str);
    }
/* =================================================================================== */

    // Special commands are carried in the X-M2M-UTCMD header, not the body.
    if (o2pt->utcmd)
    {
        // tolerate surrounding whitespace some HTTP clients add to header values
        char *cmd = o2pt->utcmd;
        while (*cmd == ' ' || *cmd == '\t')
            cmd++;
        size_t clen = strlen(cmd);
        while (clen > 0 && (cmd[clen - 1] == ' ' || cmd[clen - 1] == '\t' || cmd[clen - 1] == '\r'))
            cmd[--clen] = '\0';

        if (strcasecmp(cmd, "Reset") == 0)
            return handle_ut_reset(o2pt);
/* ========== DEBUG TRACE — delete this line later ========== */
        logger(LOG_TAG, LOG_LEVEL_ERROR, "[DBG] unknown UT command '%s' (raw '%s') -> 4000", cmd, o2pt->utcmd);
/* ======================================================== */
        return handle_error(o2pt, RSC_BAD_REQUEST, "Unknown Upper Tester command");
    }

    cJSON *pc = cJSON_GetObjectItem(o2pt->request_pc, "m2m:rqp");
    if (!pc)
    {
/* ========== DEBUG TRACE — delete this block later ========== */
        char keys[256] = {0};
        for (cJSON *it = o2pt->request_pc ? o2pt->request_pc->child : NULL; it; it = it->next)
        {
            strncat(keys, it->string ? it->string : "?", sizeof(keys) - strlen(keys) - 2);
            strncat(keys, " ", sizeof(keys) - strlen(keys) - 1);
        }
        logger(LOG_TAG, LOG_LEVEL_ERROR,
               "[DBG] no 'm2m:rqp' and no utcmd header -> 4000 (top-level keys: [%s])",
               keys[0] ? keys : "<empty/null body>");
/* ======================================================== */
        return handle_error(o2pt, RSC_BAD_REQUEST, "Bad Request");
    }
    oneM2MPrimitive *req = calloc(1, sizeof(oneM2MPrimitive));

    if ((pjson = cJSON_GetObjectItem(pc, "op")))
    {
        req->op = pjson->valueint;
    }
    else
    {
/* ========== DEBUG TRACE — delete this line later ========== */
        logger(LOG_TAG, LOG_LEVEL_ERROR, "[DBG] m2m:rqp missing 'op' -> 4000");
/* ======================================================== */
        free_o2pt(req);
        return handle_error(o2pt, RSC_BAD_REQUEST, "Bad Request");
    }

    if ((pjson = cJSON_GetObjectItem(pc, "to")))
    {
        if (cJSON_IsString(pjson))
            req->to = strdup(pjson->valuestring);
    }
    else
    {
/* ========== DEBUG TRACE — delete this line later ========== */
        logger(LOG_TAG, LOG_LEVEL_ERROR, "[DBG] m2m:rqp missing 'to' -> 4000");
/* ======================================================== */
        free_o2pt(req);
        return handle_error(o2pt, RSC_BAD_REQUEST, "Bad Request");
    }

    if ((pjson = cJSON_GetObjectItem(pc, "fr")) && cJSON_IsString(pjson))
    {
        req->fr = strdup(pjson->valuestring);
    }

    if ((pjson = cJSON_GetObjectItem(pc, "rqi")) && cJSON_IsString(pjson))
    {
        req->rqi = strdup(pjson->valuestring);
    }

    if ((pjson = cJSON_GetObjectItem(pc, "pc")))
    {
        req->request_pc = cJSON_Duplicate(pjson, true);
    }

    if ((pjson = cJSON_GetObjectItem(pc, "ty")))
    {
        req->ty = pjson->valueint;
    }

    if ((pjson = cJSON_GetObjectItem(pc, "rvi")))
    {
        // accept "3" (string) or 3 (number)
        if (cJSON_IsString(pjson))
            req->rvi = to_rvi(pjson->valuestring);
        else if (cJSON_IsNumber(pjson))
        {
            char n[8];
            snprintf(n, sizeof(n), "%d", pjson->valueint);
            req->rvi = to_rvi(n);
        }
/* ========== DEBUG TRACE — delete this line later ========== */
        logger(LOG_TAG, LOG_LEVEL_INFO, "[DBG] m2m:rqp rvi field type=%d -> req->rvi=%d", pjson->type, req->rvi);
/* ======================================================== */
    }
    else
    {
/* ========== DEBUG TRACE — delete this line later ========== */
        logger(LOG_TAG, LOG_LEVEL_ERROR, "[DBG] m2m:rqp missing 'rvi' -> 4000");
/* ======================================================== */
        free_o2pt(req);
        return handle_error(o2pt, RSC_BAD_REQUEST, "Bad Request");
    }

/* ========== DEBUG TRACE (assembled inner request) — delete this block later ========== */
    logger(LOG_TAG, LOG_LEVEL_INFO,
           "[DBG] m2m:rqp inner request: op=%d to=%s fr=%s rqi=%s ty=%d rvi=%d has_pc=%d",
           req->op, req->to ? req->to : "-", req->fr ? req->fr : "-",
           req->rqi ? req->rqi : "-", req->ty, req->rvi, req->request_pc != NULL);
/* =============================================================================== */

    route(req);

/* ========== DEBUG TRACE — delete this line later ========== */
    logger(LOG_TAG, LOG_LEVEL_INFO, "[DBG] m2m:rqp inner route done: req->rsc=%d", req->rsc);
/* ======================================================== */

    o2pt->response_pc = cJSON_Duplicate(req->response_pc, true);
    if (req->rsc >= RSC_BAD_REQUEST)
    {
        o2pt->rsc = req->rsc;
    }
    else
    {
        o2pt->rsc = RSC_OK;
    }

    free_o2pt(req);

    return 0;
}

#endif /* UPPERTESTER */
