/* modem app (gprs) */

/* (C) 2022 by sysmocom - s.f.m.c. GmbH <info@sysmocom.de>
 * All Rights Reserved
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/lienses/>.
 *
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/ip6.h>

#include <osmocom/core/msgb.h>
#include <osmocom/core/utils.h>
#include <osmocom/core/signal.h>
#include <osmocom/core/application.h>
#include <osmocom/core/socket.h>
#include <osmocom/core/tun.h>
#include <osmocom/gsm/gsm_utils.h>
#include <osmocom/gsm/lapdm.h>
#include <osmocom/vty/vty.h>

#include <osmocom/bb/common/osmocom_data.h>
#include <osmocom/bb/common/settings.h>
#include <osmocom/bb/common/ms.h>
#include <osmocom/bb/common/logging.h>
#include <osmocom/bb/common/l1ctl.h>
#include <osmocom/bb/common/l23_app.h>
#include <osmocom/bb/common/l1l2_interface.h>
#include <osmocom/bb/common/apn.h>
#include <osmocom/bb/modem/rlcmac.h>
#include <osmocom/bb/modem/llc.h>
#include <osmocom/bb/modem/sndcp.h>
#include <osmocom/bb/modem/gmm.h>
#include <osmocom/bb/modem/sm.h>
#include <osmocom/bb/modem/vty.h>
#include <osmocom/bb/modem/grr.h>
#include <osmocom/bb/modem/modem.h>

#include <l1ctl_proto.h>

#include "config.h"

struct modem_app app_data;

static bool modem_can_gprs_attach(const struct osmocom_ms *ms)
{
	const struct gsm_subscriber *subscr = &ms->subscr;
	const struct gsm322_cellsel *cs = &ms->cellsel;
	const struct gsm48_sysinfo *si = &cs->sel_si;

	if (!subscr->sim_valid)
		goto ret_false;

	if (!si->si1 || !si->si3 || !si->si4 || !si->si13)
		goto ret_false;

	if (!si->gprs.supported)
		goto ret_false;

	return true;

ret_false:
	LOGP(DRLCMAC, LOGL_INFO, "Delaying GPRS attach, waiting for:%s%s%s%s%s%s\n",
	     subscr->sim_valid ? "" : " imsi",
	     si->si1 ? "" : " si1",
	     si->si3 ? "" : " si3",
	     si->si4 ? "" : " si4",
	     si->si13 ? "" : " si13",
	     si->gprs.supported ? "" : " GprsIndicator");
	return false;
}

int modem_gprs_attach_if_needed(struct osmocom_ms *ms)
{
	int rc;

	if (app_data.modem_state != MODEM_ST_IDLE)
		return 0;

	if (!modem_can_gprs_attach(ms))
		return 0;

	app_data.modem_state = MODEM_ST_ATTACHING;
	rc = modem_gmm_gmmreg_attach_req(ms);
	if (rc < 0)
		app_data.modem_state = MODEM_ST_IDLE;
	return rc;
}

/* Local network-originated IP packet, needs to be sent via SNDCP/LLC (GPRS) towards GSM network */
static int modem_tun_data_ind_cb(struct osmo_tundev *tun, struct msgb *msg)
{
	struct osmobb_apn *apn = (struct osmobb_apn *)osmo_tundev_get_priv_data(tun);
	struct osmo_sockaddr dst;
	struct iphdr *iph = (struct iphdr *)msgb_data(msg);
	struct ip6_hdr *ip6h = (struct ip6_hdr *)msgb_data(msg);
	size_t pkt_len = msgb_length(msg);
	uint8_t pref_offset;
	char addrstr[INET6_ADDRSTRLEN];
	int rc = 0;

	switch (iph->version) {
	case 4:
		if (pkt_len < sizeof(*iph) || pkt_len < 4*iph->ihl)
			return -1;
		dst.u.sin.sin_family = AF_INET;
		dst.u.sin.sin_addr.s_addr = iph->daddr;
		break;
	case 6:
		/* Due to the fact that 3GPP requires an allocation of a
		 * /64 prefix to each MS, we must instruct
		 * ippool_getip() below to match only the leading /64
		 * prefix, i.e. the first 8 bytes of the address. If the ll addr
		 * is used, then the match should be done on the trailing 64
		 * bits. */
		dst.u.sin6.sin6_family = AF_INET6;
		pref_offset = IN6_IS_ADDR_LINKLOCAL(&ip6h->ip6_dst) ? 8 : 0;
		memcpy(&dst.u.sin6.sin6_addr, ((uint8_t *)&ip6h->ip6_dst) + pref_offset, 8);
		break;
	default:
		LOGTUN(LOGL_NOTICE, tun, "non-IPv%u packet received\n", iph->version);
		rc = -1;
		goto free_ret;
	}

	LOGPAPN(LOGL_DEBUG, apn, "system wants to transmit IPv%c pkt to %s (%zu bytes)\n",
		iph->version == 4 ? '4' : '6', osmo_sockaddr_ntop(&dst.u.sa, addrstr), pkt_len);

	rc = modem_sndcp_sn_unitdata_req(apn, msgb_data(msg), pkt_len);

free_ret:
	msgb_free(msg);
	return rc;
}

void layer3_app_reset(void)
{
	memset(&app_data, 0x00, sizeof(app_data));
}

/* SIM becomes ATTACHED/DETACHED, or answers a request */
static int modem_l23_subscr_signal_cb(unsigned int subsys, unsigned int signal,
		     void *handler_data, void *signal_data)
{
	struct osmocom_ms *ms;
	struct osmobb_l23_subscr_sim_auth_resp_sig_data *sim_auth_resp;

	OSMO_ASSERT(subsys == SS_L23_SUBSCR);

	switch (signal) {
	case S_L23_SUBSCR_SIM_ATTACHED:
		ms = signal_data;
		modem_gprs_attach_if_needed(ms);
		break;
	case S_L23_SUBSCR_SIM_DETACHED:
		ms = signal_data;
		modem_gmm_gmmreg_detach_req(ms);
		break;
	case S_L23_SUBSCR_SIM_AUTH_RESP:
		sim_auth_resp = signal_data;
		ms = sim_auth_resp->ms;
		modem_gmm_gmmreg_sim_auth_rsp(ms, sim_auth_resp->sres,
					      ms->subscr.key,
					      sizeof(ms->subscr.key));
		break;
	default:
		OSMO_ASSERT(0);
	}

	return 0;
}

static int global_signal_cb(unsigned int subsys, unsigned int signal,
			    void *handler_data, void *signal_data)
{
	struct osmocom_ms *ms;

	if (subsys != SS_L1CTL)
		return 0;

	switch (signal) {
	case S_L1CTL_RESET:
		ms = signal_data;
		layer3_app_reset();
		app_data.ms = ms;

		/* insert test card, if enabled */
		if (ms->settings.sim_type != GSM_SIM_TYPE_NONE) {
			/* insert sim card */
			gsm_subscr_insert(ms);
		} else {
			/* No SIM, trigger PLMN selection process.
			 * FIXME: not implemented. Code in mobile needs to be
			 * moved to common/ and reuse it here.
			 */
		}

		ms->started = true;
		return l1ctl_tx_fbsb_req(ms, ms->test_arfcn,
					 L1CTL_FBSB_F_FB01SB, 100, 0,
					 CCCH_MODE_NONE, dbm2rxlev(-85));
	}

	return 0;
}

static int _modem_start(void)
{
	int rc;

	rc = layer2_open(app_data.ms, app_data.ms->settings.layer2_socket_path);
	if (rc < 0) {
		fprintf(stderr, "Failed during layer2_open()\n");
		return rc;
	}

	l1ctl_tx_reset_req(app_data.ms, L1CTL_RES_T_FULL);
	return 0;
}

/* global exit */
static int _modem_exit(void)
{
	osmo_signal_unregister_handler(SS_L23_SUBSCR, &modem_l23_subscr_signal_cb, NULL);
	osmo_signal_unregister_handler(SS_GLOBAL, &global_signal_cb, NULL);
	return 0;
}

int l23_app_init(void)
{
	int rc;

	l23_app_start = _modem_start;
	l23_app_exit = _modem_exit;

	log_set_category_filter(osmo_stderr_target, DLGLOBAL, 1, LOGL_DEBUG);
	log_set_category_filter(osmo_stderr_target, DLCSN1, 1, LOGL_DEBUG);
	log_set_category_filter(osmo_stderr_target, DRR, 1, LOGL_INFO);

	app_data.ms = osmocom_ms_alloc(l23_ctx, "1");
	OSMO_ASSERT(app_data.ms);

	if ((rc = modem_rlcmac_init(app_data.ms))) {
		LOGP(DRLCMAC, LOGL_FATAL, "Failed initializing RLC/MAC layer\n");
		return rc;
	}

	if ((rc = modem_llc_init(app_data.ms, NULL))) {
		LOGP(DLLC, LOGL_FATAL, "Failed initializing LLC layer\n");
		return rc;
	}

	if ((rc = modem_sndcp_init(app_data.ms))) {
		LOGP(DSNDCP, LOGL_FATAL, "Failed initializing SNDCP layer\n");
		return rc;
	}

	if ((rc = modem_gmm_init(app_data.ms))) {
		LOGP(DGMM, LOGL_FATAL, "Failed initializing GMM layer\n");
		return rc;
	}

	if ((rc = modem_sm_init(app_data.ms))) {
		LOGP(DSM, LOGL_FATAL, "Failed initializing SM layer\n");
		return rc;
	}

	osmo_signal_register_handler(SS_L1CTL, &global_signal_cb, NULL);
	osmo_signal_register_handler(SS_L23_SUBSCR, &modem_l23_subscr_signal_cb, NULL);
	lapdm_channel_set_l3(&app_data.ms->lapdm_channel, &modem_grr_rslms_cb, app_data.ms);
	return 0;
}

static struct vty_app_info _modem_vty_info = {
	.name = "OsmocomBB(modem)",
	.version = PACKAGE_VERSION,
	.go_parent_cb = modem_vty_go_parent,
};

const struct l23_app_info l23_app_info = {
	.copyright = "Copyright (C) 2022 by sysmocom - s.f.m.c. GmbH <info@sysmocom.de>\n",
	.opt_supported = L23_OPT_ARFCN | L23_OPT_TAP | L23_OPT_VTY | L23_OPT_DBG,
	.vty_info = &_modem_vty_info,
	.vty_init = modem_vty_init,
	.tun_data_ind_cb = modem_tun_data_ind_cb,
};
