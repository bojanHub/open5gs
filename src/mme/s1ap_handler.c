#define TRACE_MODULE _s1ap_handler

#include "core_debug.h"

#include "mme_event.h"

#include "s1ap_conv.h"
#include "s1ap_build.h"
#include "s1ap_path.h"
#include "nas_message.h"
#include "nas_security.h"
#include "mme_s11_build.h"
#include "mme_gtp_path.h"

#include "s1ap_handler.h"

static void event_s1ap_to_nas(enb_ue_t *enb_ue, S1ap_NAS_PDU_t *nasPdu)
{
    nas_security_header_t *sh = NULL;
    nas_security_header_type_t security_header_type;

    nas_esm_header_t *h = NULL;
    pkbuf_t *nasbuf = NULL;
    event_t e;

    d_assert(enb_ue, return, "Null param");
    d_assert(nasPdu, return, "Null param");

    /* The Packet Buffer(pkbuf_t) for NAS message MUST make a HEADROOM. 
     * When calculating AES_CMAC, we need to use the headroom of the packet. */
    nasbuf = pkbuf_alloc(NAS_HEADROOM, nasPdu->size);
    d_assert(nasbuf, return, "Null param");
    memcpy(nasbuf->payload, nasPdu->buf, nasPdu->size);

    sh = nasbuf->payload;
    d_assert(sh, return, "Null param");

    memset(&security_header_type, 0, sizeof(nas_security_header_type_t));
    switch(sh->security_header_type)
    {
        case NAS_SECURITY_HEADER_PLAIN_NAS_MESSAGE:
            break;
        case NAS_SECURITY_HEADER_FOR_SERVICE_REQUEST_MESSAGE:
            security_header_type.service_request = 1;
            break;
        case NAS_SECURITY_HEADER_INTEGRITY_PROTECTED:
            security_header_type.integrity_protected = 1;
            d_assert(pkbuf_header(nasbuf, -6) == CORE_OK,
                    return, "pkbuf_header error");
            break;
        case NAS_SECURITY_HEADER_INTEGRITY_PROTECTED_AND_CIPHERED:
            security_header_type.integrity_protected = 1;
            security_header_type.ciphered = 1;
            d_assert(pkbuf_header(nasbuf, -6) == CORE_OK,
                    return, "pkbuf_header error");
            break;
        case NAS_SECURITY_HEADER_INTEGRITY_PROTECTED_AND_NEW_SECURITY_CONTEXT:
            security_header_type.integrity_protected = 1;
            security_header_type.new_security_context = 1;
            d_assert(pkbuf_header(nasbuf, -6) == CORE_OK,
                    return, "pkbuf_header error");
            break;
        case NAS_SECURITY_HEADER_INTEGRITY_PROTECTED_AND_CIPHTERD_WITH_NEW_INTEGRITY_CONTEXT:
            security_header_type.integrity_protected = 1;
            security_header_type.ciphered = 1;
            security_header_type.new_security_context = 1;
            d_assert(pkbuf_header(nasbuf, -6) == CORE_OK,
                    return, "pkbuf_header error");
            break;
        default:
            d_error("Not implemented(securiry header type:0x%x)", 
                    sh->security_header_type);
            return;
    }

    if (enb_ue->mme_ue)
    {
        d_assert(nas_security_decode(
            enb_ue->mme_ue, security_header_type, nasbuf) == CORE_OK,
            pkbuf_free(nasbuf);return, "nas_security_decode failed");
    }

    h = nasbuf->payload;
    d_assert(h, pkbuf_free(nasbuf); return, "Null param");
    if (h->protocol_discriminator == NAS_PROTOCOL_DISCRIMINATOR_EMM)
    {
        event_set(&e, MME_EVT_EMM_MESSAGE);
        event_set_param1(&e, (c_uintptr_t)enb_ue->index);
        event_set_param2(&e, (c_uintptr_t)security_header_type.type);
        event_set_param3(&e, (c_uintptr_t)nasbuf);
        mme_event_send(&e);
    }
    else if (h->protocol_discriminator == NAS_PROTOCOL_DISCRIMINATOR_ESM)
    {
        mme_sess_t *sess = NULL;
        mme_ue_t *mme_ue = enb_ue->mme_ue;

        if (!mme_ue)
        {
            d_error("No mme_ue exists");
            pkbuf_free(nasbuf);
            return;
        }

        sess = mme_sess_find_by_pti(mme_ue, h->procedure_transaction_identity);
        if (sess)
        {
            event_set(&e, MME_EVT_ESM_MESSAGE);
            event_set_param1(&e, (c_uintptr_t)sess->index);
            event_set_param2(&e, (c_uintptr_t)security_header_type.type);
            event_set_param3(&e, (c_uintptr_t)nasbuf);
            mme_event_send(&e);
        }
        else
            d_error("Can't find ESM context(UE:%s, PTI:%d)",
                    mme_ue->imsi_bcd, h->procedure_transaction_identity);
    }
    else
        d_assert(0, pkbuf_free(nasbuf); return, "Unknown protocol:%d", 
                h->protocol_discriminator);
}


void s1ap_handle_s1_setup_request(mme_enb_t *enb, s1ap_message_t *message)
{
    char buf[INET_ADDRSTRLEN];

    S1ap_S1SetupRequestIEs_t *ies = NULL;
    pkbuf_t *s1apbuf = NULL;
    c_uint32_t enb_id;
    int i,j;
    int num_of_tai = 0;

    d_assert(enb, return, "Null param");
    d_assert(enb->s1ap_sock, return, "Null param");
    d_assert(message, return, "Null param");

    ies = &message->s1ap_S1SetupRequestIEs;
    d_assert(ies, return, "Null param");

    s1ap_ENB_ID_to_uint32(&ies->global_ENB_ID.eNB_ID, &enb_id);


    /* Parse Supported TA */
    for (i = 0; i < ies->supportedTAs.list.count; i++)
    {
        S1ap_SupportedTAs_Item_t *tai = NULL;
        S1ap_TAC_t *tAC;

        tai = (S1ap_SupportedTAs_Item_t *)ies->supportedTAs.list.array[i];
        tAC = &tai->tAC;

        for (j = 0; j < tai->broadcastPLMNs.list.count; j++)
        {
            S1ap_PLMNidentity_t *pLMNidentity = NULL;
            pLMNidentity = 
                (S1ap_PLMNidentity_t *)tai->broadcastPLMNs.list.array[j];

            memcpy(&enb->tai[num_of_tai].tac, tAC->buf, sizeof(c_uint16_t));
            enb->tai[num_of_tai].tac = ntohs(enb->tai[num_of_tai].tac);

            memcpy(&enb->tai[num_of_tai].plmn_id, pLMNidentity->buf, 
                    sizeof(plmn_id_t));
            num_of_tai++;
        }
    }

    enb->num_of_tai = num_of_tai;

    if (enb->num_of_tai == 0)
    {
        d_error("No supported TA exist in s1stup_req messages");
    }


#if 0 /* FIXME : does it needed? */
    if (mme_ctx_enb_find_by_enb_id(enb_id))
    {
        S1ap_Cause_t cause;
        d_error("eNB-id[0x%x] duplicated from [%s]", enb_id,
                INET_NTOP(&enb->s1ap_sock->remote.sin_addr.s_addr, buf));

        cause.present = S1ap_Cause_PR_protocol;
        cause.choice.protocol = 
            S1ap_CauseProtocol_message_not_compatible_with_receiver_state;
        rv = s1ap_build_setup_failure(&s1apbuf, cause);
    }
#endif
    d_assert(enb->s1ap_sock, return,);
    d_trace(3, "[S1AP] S1SetupRequest : eNB[%s:%d] --> MME\n",
        INET_NTOP(&enb->s1ap_sock->remote.sin_addr.s_addr, buf),
        enb_id);

    enb->enb_id = enb_id;

    d_assert(s1ap_build_setup_rsp(&s1apbuf) == CORE_OK, 
            return, "build error");
    d_assert(s1ap_send_to_enb(enb, s1apbuf) == CORE_OK, , "send error");

    d_assert(enb->s1ap_sock, return,);
    d_trace(3, "[S1AP] S1SetupResponse: eNB[%s:%d] <-- MME\n",
        INET_NTOP(&enb->s1ap_sock->remote.sin_addr.s_addr, buf),
        enb_id);
}

void s1ap_handle_initial_ue_message(mme_enb_t *enb, s1ap_message_t *message)
{
    char buf[INET_ADDRSTRLEN];

    enb_ue_t *enb_ue = NULL;
    S1ap_InitialUEMessage_IEs_t *ies = NULL;
    S1ap_TAI_t *tai = NULL;
	S1ap_PLMNidentity_t *pLMNidentity = NULL;
	S1ap_TAC_t *tAC = NULL;
    S1ap_EUTRAN_CGI_t *eutran_cgi = NULL;
	S1ap_CellIdentity_t	*cell_ID = NULL;

    d_assert(enb, return, "Null param");

    ies = &message->s1ap_InitialUEMessage_IEs;
    d_assert(ies, return, "Null param");

    tai = &ies->tai;
    d_assert(tai, return,);
    pLMNidentity = &tai->pLMNidentity;
    d_assert(pLMNidentity && pLMNidentity->size == sizeof(plmn_id_t), return,);
    tAC = &tai->tAC;
    d_assert(tAC && tAC->size == sizeof(c_uint16_t), return,);

    eutran_cgi = &ies->eutran_cgi;
    d_assert(eutran_cgi, return,);
    pLMNidentity = &eutran_cgi->pLMNidentity;
    d_assert(pLMNidentity && pLMNidentity->size == sizeof(plmn_id_t), return,);
    cell_ID = &eutran_cgi->cell_ID;
    d_assert(cell_ID, return,);

    enb_ue = enb_ue_find_by_enb_ue_s1ap_id(enb, ies->eNB_UE_S1AP_ID);
    if (!enb_ue)
    {
        enb_ue = enb_ue_add(enb);
        d_assert(enb_ue, return, "Null param");

        enb_ue->enb_ue_s1ap_id = ies->eNB_UE_S1AP_ID;

        /* Find MME_UE if s_tmsi included */
        if (ies->presenceMask &S1AP_INITIALUEMESSAGE_IES_S_TMSI_PRESENT)
        {
            S1ap_S_TMSI_t *s_tmsi = &ies->s_tmsi;
            served_gummei_t *served_gummei = &mme_self()->served_gummei[0];
            guti_t guti;
            mme_ue_t *mme_ue = NULL;

            memset(&guti, 0, sizeof(guti_t));

            /* FIXME : Use the first configured plmn_id and mme group id */
            memcpy(&guti.plmn_id, &served_gummei->plmn_id[0], PLMN_ID_LEN);
            guti.mme_gid = served_gummei->mme_gid[0];

            /* size must be 1 */
            memcpy(&guti.mme_code, s_tmsi->mMEC.buf, s_tmsi->mMEC.size);
            /* size must be 4 */
            memcpy(&guti.m_tmsi, s_tmsi->m_TMSI.buf, s_tmsi->m_TMSI.size);
            guti.m_tmsi = ntohl(guti.m_tmsi);

            mme_ue = mme_ue_find_by_guti(&guti);
            if (!mme_ue)
            {
                d_error("Can not find mme_ue with mme_code = %d, m_tmsi = %d",
                        guti.mme_code, guti.m_tmsi);
            }
            else
            {
                mme_associate_ue_context(mme_ue, enb_ue);
            }
        }
    }

    memcpy(&enb_ue->tai.plmn_id, pLMNidentity->buf, 
            sizeof(enb_ue->tai.plmn_id));
    memcpy(&enb_ue->tai.tac, tAC->buf, sizeof(enb_ue->tai.tac));
    enb_ue->tai.tac = ntohs(enb_ue->tai.tac);
    memcpy(&enb_ue->e_cgi.plmn_id, pLMNidentity->buf, 
            sizeof(enb_ue->e_cgi.plmn_id));
    memcpy(&enb_ue->e_cgi.cell_id, cell_ID->buf, sizeof(enb_ue->e_cgi.cell_id));
    enb_ue->e_cgi.cell_id = (ntohl(enb_ue->e_cgi.cell_id) >> 4);

    d_assert(enb->s1ap_sock, enb_ue_remove(enb_ue); return,);
    d_trace(3, "[S1AP] InitialUEMessage : "
            "UE[eNB-UE-S1AP-ID(%d)] --> eNB[%s:%d]\n",
        enb_ue->enb_ue_s1ap_id,
        INET_NTOP(&enb->s1ap_sock->remote.sin_addr.s_addr, buf),
        enb->enb_id);

    event_s1ap_to_nas(enb_ue, &ies->nas_pdu);
}

void s1ap_handle_uplink_nas_transport(
        mme_enb_t *enb, s1ap_message_t *message)
{
    char buf[INET_ADDRSTRLEN];

    enb_ue_t *enb_ue = NULL;
    S1ap_UplinkNASTransport_IEs_t *ies = NULL;

    ies = &message->s1ap_UplinkNASTransport_IEs;
    d_assert(ies, return, "Null param");

    enb_ue = enb_ue_find_by_enb_ue_s1ap_id(enb, ies->eNB_UE_S1AP_ID);
    d_assert(enb_ue, return, "Null param");

    d_trace(3, "[S1AP] uplinkNASTransport : "
            "UE[eNB-UE-S1AP-ID(%d)] --> eNB[%s:%d]\n",
        enb_ue->enb_ue_s1ap_id,
        INET_NTOP(&enb->s1ap_sock->remote.sin_addr.s_addr, buf),
        enb->enb_id);

    event_s1ap_to_nas(enb_ue, &ies->nas_pdu);
}

void s1ap_handle_ue_capability_info_indication(
        mme_enb_t *enb, s1ap_message_t *message)
{
    char buf[INET_ADDRSTRLEN];

    enb_ue_t *enb_ue = NULL;
    S1ap_UECapabilityInfoIndicationIEs_t *ies = NULL;

    ies = &message->s1ap_UECapabilityInfoIndicationIEs;
    d_assert(ies, return, "Null param");

    enb_ue = enb_ue_find_by_enb_ue_s1ap_id(enb, ies->eNB_UE_S1AP_ID);
    d_assert(enb_ue, return, "No UE Context[%d]", ies->eNB_UE_S1AP_ID);

    if (enb_ue->mme_ue)
    {
        S1ap_UERadioCapability_t *ue_radio_capa = NULL;
        S1ap_UERadioCapability_t *radio_capa = NULL;
        mme_ue_t *mme_ue = enb_ue->mme_ue;

        ue_radio_capa = &ies->ueRadioCapability;

        /* Release the previous one */
        if (mme_ue->radio_capa)
        {
            radio_capa = (S1ap_UERadioCapability_t *)mme_ue->radio_capa;

            if (radio_capa->buf)
                core_free(radio_capa->buf);
            core_free(mme_ue->radio_capa);
        }
        /* Save UE radio capability */ 
        mme_ue->radio_capa = core_calloc(1, sizeof(S1ap_UERadioCapability_t));
        radio_capa = (S1ap_UERadioCapability_t *)mme_ue->radio_capa;
        d_assert(radio_capa,return,"core_calloc Error");

        radio_capa->size = ue_radio_capa->size;
        radio_capa->buf = 
            core_calloc(radio_capa->size, sizeof(c_uint8_t)); 
        d_assert(radio_capa->buf,return,"core_calloc Error(size=%d)",
                radio_capa->size);
        memcpy(radio_capa->buf, ue_radio_capa->buf, radio_capa->size);
    }

    d_trace(3, "[S1AP] UE Capability Info Indication : "
            "UE[eNB-UE-S1AP-ID(%d)] --> eNB[%s:%d]\n",
            enb_ue->enb_ue_s1ap_id,
        INET_NTOP(&enb->s1ap_sock->remote.sin_addr.s_addr, buf),
        enb->enb_id);
}

void s1ap_handle_initial_context_setup_response(
        mme_enb_t *enb, s1ap_message_t *message)
{
    char buf[INET_ADDRSTRLEN];
    int i = 0;

    enb_ue_t *enb_ue = NULL;
    S1ap_InitialContextSetupResponseIEs_t *ies = NULL;

    ies = &message->s1ap_InitialContextSetupResponseIEs;
    d_assert(ies, return, "Null param");

    enb_ue = enb_ue_find_by_enb_ue_s1ap_id(enb, ies->eNB_UE_S1AP_ID);
    d_assert(enb_ue, return, "No UE Context[%d]", ies->eNB_UE_S1AP_ID);

    d_trace(3, "[S1AP] Initial Context Setup Response : "
            "UE[eNB-UE-S1AP-ID(%d)] --> eNB[%s:%d]\n",
            enb_ue->enb_ue_s1ap_id,
        INET_NTOP(&enb->s1ap_sock->remote.sin_addr.s_addr, buf),
        enb->enb_id);

    for (i = 0; 
        i < ies->e_RABSetupListCtxtSURes.s1ap_E_RABSetupItemCtxtSURes.count; 
        i++)
    {
        status_t rv;
        gtp_header_t h;
        gtp_xact_t *xact = NULL;
        mme_sess_t *sess = NULL;
        pkbuf_t *pkbuf = NULL;

        mme_bearer_t *bearer = NULL;
        mme_ue_t *mme_ue = enb_ue->mme_ue;
        S1ap_E_RABSetupItemCtxtSURes_t *e_rab = NULL;

        e_rab = (S1ap_E_RABSetupItemCtxtSURes_t *)
            ies->e_RABSetupListCtxtSURes.s1ap_E_RABSetupItemCtxtSURes.array[i];
        d_assert(e_rab, return, "Null param");

        sess = mme_sess_find_by_ebi(mme_ue, e_rab->e_RAB_ID);
        d_assert(sess, return, "Null param");
        bearer = mme_default_bearer_in_sess(sess);
        d_assert(bearer, return, "Null param");
        memcpy(&bearer->enb_s1u_teid, e_rab->gTP_TEID.buf, 
                sizeof(bearer->enb_s1u_teid));
        bearer->enb_s1u_teid = ntohl(bearer->enb_s1u_teid);
        memcpy(&bearer->enb_s1u_addr, e_rab->transportLayerAddress.buf,
                sizeof(bearer->enb_s1u_addr));

        memset(&h, 0, sizeof(gtp_header_t));
        h.type = GTP_MODIFY_BEARER_REQUEST_TYPE;
        h.teid = sess->sgw_s11_teid;

        rv = mme_s11_build_modify_bearer_request(&pkbuf, h.type, bearer);
        d_assert(rv == CORE_OK, return, "S11 build error");

        xact = gtp_xact_local_create(
                mme_self()->s11_sock, (gtp_node_t *)sess->sgw, &h, pkbuf);
        d_assert(xact, return, "Null param");

        rv = gtp_xact_commit(xact);
        d_assert(rv == CORE_OK, return, "xact_commit error");
    }
}

void s1ap_handle_ue_context_release_request(
        mme_enb_t *enb, s1ap_message_t *message)
{
    char buf[INET_ADDRSTRLEN];

    enb_ue_t *enb_ue = NULL;
    S1ap_UEContextReleaseRequest_IEs_t *ies = NULL;
    long cause;

    ies = &message->s1ap_UEContextReleaseRequest_IEs;
    d_assert(ies, return, "Null param");

    enb_ue = enb_ue_find_by_mme_ue_s1ap_id(ies->mme_ue_s1ap_id);
    d_assert(enb_ue, return, "No UE Context[%d]", ies->mme_ue_s1ap_id);

    d_trace(3, "[S1AP] UE Context Release Request : "
            "UE[mME-UE-S1AP-ID(%d)] --> eNB[%s:%d]\n",
            enb_ue->mme_ue_s1ap_id,
        INET_NTOP(&enb->s1ap_sock->remote.sin_addr.s_addr, buf),
        enb->enb_id);

    switch(ies->cause.present)
    {
        case S1ap_Cause_PR_radioNetwork:
            cause = ies->cause.choice.radioNetwork;
            if (cause == S1ap_CauseRadioNetwork_user_inactivity)
            {
                mme_ue_t *mme_ue = enb_ue->mme_ue;
                status_t rv;

                if (MME_UE_HAVE_SESSION(mme_ue))
                {
                    mme_sess_t *sess = mme_sess_first(mme_ue);
                    while (sess != NULL)
                    {
                        gtp_header_t h;
                        pkbuf_t *pkbuf = NULL;
                        gtp_xact_t *xact = NULL;

                        memset(&h, 0, sizeof(gtp_header_t));
                        h.type = GTP_RELEASE_ACCESS_BEARERS_REQUEST_TYPE;
                        h.teid = sess->sgw_s11_teid;

                        rv = mme_s11_build_release_access_bearers_request(
                                &pkbuf, h.type);
                        d_assert(rv == CORE_OK, return, "S11 build error");

                        xact = gtp_xact_local_create(
                                mme_self()->s11_sock, (gtp_node_t *)sess->sgw,
                                &h, pkbuf);
                        d_assert(xact, return, "Null param");

                        rv = gtp_xact_commit(xact);
                        d_assert(rv == CORE_OK, return, "xact_commit error");

                        sess = mme_sess_next(sess);
                    }
                }
                else
                {
                    s1ap_handle_release_access_bearers_response(enb_ue);
                }
            }
            else
            {
                d_warn("Not implmented (radioNetwork cause : %d)", cause);
            }
            break;
        case S1ap_Cause_PR_transport:
            cause = ies->cause.choice.transport;
            d_warn("Not implmented (transport cause : %d)", cause);
            break;
        case S1ap_Cause_PR_nas:
            cause = ies->cause.choice.nas;
            d_warn("Not implmented (nas cause : %d)", cause);
            break;
        case S1ap_Cause_PR_protocol:
            cause = ies->cause.choice.protocol;
            d_warn("Not implmented (protocol cause : %d)", cause);
            break;
        case S1ap_Cause_PR_misc:
            cause = ies->cause.choice.misc;
            d_warn("Not implmented (misc cause : %d)", cause);
            break;
        default:
            d_warn("Invalid cause type : %d", ies->cause.present);
            break;
    }
}

void s1ap_handle_release_access_bearers_response(enb_ue_t *enb_ue)
{
    status_t rv;
    mme_enb_t *enb = NULL;
    pkbuf_t *s1apbuf;
    S1ap_Cause_t cause;

    d_assert(enb_ue, return, "Null param");
    enb = enb_ue->enb;
    d_assert(enb, return, "Null param");

    cause.present = S1ap_Cause_PR_nas;
    cause.choice.nas = S1ap_CauseNas_normal_release;

    rv = s1ap_build_ue_context_release_commmand(&s1apbuf, enb_ue, &cause);
    d_assert(rv == CORE_OK && s1apbuf, return, "s1ap build error");

    d_assert(s1ap_send_to_enb(enb, s1apbuf) == CORE_OK,, "s1ap send error");
}

void s1ap_handle_ue_context_release_complete(
        mme_enb_t *enb, s1ap_message_t *message)
{
    char buf[INET_ADDRSTRLEN];

    enb_ue_t *enb_ue = NULL;
    S1ap_UEContextReleaseComplete_IEs_t *ies = NULL;

    ies = &message->s1ap_UEContextReleaseComplete_IEs;
    d_assert(ies, return, "Null param");

    enb_ue = enb_ue_find_by_mme_ue_s1ap_id(ies->mme_ue_s1ap_id);
    d_assert(enb_ue, return, "No UE Context[%d]", ies->mme_ue_s1ap_id);

    d_trace(3, "[S1AP] UE Context Release Complete : "
            "UE[mME-UE-S1AP-ID(%d)] --> eNB[%s:%d]\n",
            enb_ue->mme_ue_s1ap_id,
        INET_NTOP(&enb->s1ap_sock->remote.sin_addr.s_addr, buf),
        enb->enb_id);

    enb_ue_remove(enb_ue);
}

/* FIXME : Where is a good location S1AP handler or EMM handler?*/
void s1ap_handle_paging(mme_ue_t *mme_ue)
{
    pkbuf_t *s1apbuf = NULL;
    mme_enb_t *enb = NULL;
    int i;
    status_t rv;

    /* Find enB with matched TAI */
    enb =  mme_enb_first();
    while (enb)
    {
        for (i = 0; i < enb->num_of_tai; i++)
        {
            if (!memcmp(&enb->tai[i], &mme_ue->tai, sizeof(tai_t)))
            {
                if (mme_ue->last_paging_msg)
                    s1apbuf = mme_ue->last_paging_msg;
                else
                {
                    /* Buidl S1Ap Paging message */
                    rv = s1ap_build_paging(&s1apbuf, mme_ue);
                    d_assert(rv == CORE_OK && s1apbuf, return, 
                            "s1ap build error");

                    /* Save it for later use */
                    mme_ue->last_paging_msg = pkbuf_copy(s1apbuf);
                }

                /* Send to enb */
                d_assert(s1ap_send_to_enb(enb, s1apbuf) == CORE_OK, return,
                        "s1ap send error");
            }
        }
        enb = mme_enb_next(enb);
    }
}
