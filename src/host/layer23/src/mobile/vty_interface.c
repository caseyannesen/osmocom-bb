/*
 * (C) 2010 by Andreas Eversberg <jolly@eversberg.eu>
 *
 * All Rights Reserved
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>
#include <sys/types.h>

#include <osmocom/core/utils.h>
#include <osmocom/gsm/gsm48.h>
#include <osmocom/core/talloc.h>
#include <osmocom/core/signal.h>
#include <osmocom/crypt/auth.h>
#include <osmocom/gsm/gsm23003.h>

#include <osmocom/bb/common/osmocom_data.h>
#include <osmocom/bb/common/ms.h>
#include <osmocom/bb/common/networks.h>
#include <osmocom/bb/common/gps.h>
#include <osmocom/bb/mobile/mncc.h>
#include <osmocom/bb/mobile/mncc_ms.h>
#include <osmocom/bb/mobile/transaction.h>
#include <osmocom/bb/mobile/vty.h>
#include <osmocom/bb/mobile/app_mobile.h>
#include <osmocom/bb/mobile/gsm480_ss.h>
#include <osmocom/bb/mobile/gsm411_sms.h>
#include <osmocom/vty/telnet_interface.h>
#include <osmocom/vty/misc.h>

struct cmd_node support_node = {
	SUPPORT_NODE,
	"%s(support)# ",
	1
};

struct cmd_node audio_node = {
	AUDIO_NODE,
	"%s(audio)# ",
	1
};

int vty_check_number(struct vty *vty, const char *number)
{
	int i;

	for (i = 0; i < strlen(number); i++) {
		/* allow international notation with + */
		if (i == 0 && number[i] == '+')
			continue;
		if (!(number[i] >= '0' && number[i] <= '9')
		 && number[i] != '*'
		 && number[i] != '#'
		 && !(number[i] >= 'a' && number[i] <= 'c')) {
			vty_out(vty, "Invalid digit '%c' of number!%s",
				number[i], VTY_NEWLINE);
			return -EINVAL;
		}
	}
	if (number[0] == '\0' || (number[0] == '+' && number[1] == '\0')) {
		vty_out(vty, "Given number has no digits!%s", VTY_NEWLINE);
		return -EINVAL;
	}

	return 0;
}

static void vty_restart(struct vty *vty, struct osmocom_ms *ms)
{
	if (l23_vty_reading)
		return;
	if (ms->shutdown != MS_SHUTDOWN_NONE)
		return;
	vty_out(vty, "You must restart MS '%s' ('shutdown / no shutdown') for "
		"change to take effect!%s", ms->name, VTY_NEWLINE);
}

static void vty_restart_if_started(struct vty *vty, struct osmocom_ms *ms)
{
	if (!ms->started)
		return;
	vty_restart(vty, ms);
}

static void gsm_ms_dump(struct osmocom_ms *ms, struct vty *vty)
{
	struct gsm_settings *set = &ms->settings;
	struct gsm_trans *trans;
	char *service = "";

	if (!ms->started)
		service = ", radio is not started";
	else if (ms->mmlayer.state == GSM48_MM_ST_MM_IDLE) {
		/* current MM idle state */
		switch (ms->mmlayer.substate) {
		case GSM48_MM_SST_NORMAL_SERVICE:
		case GSM48_MM_SST_PLMN_SEARCH_NORMAL:
			service = ", service is normal";
			break;
		case GSM48_MM_SST_LOC_UPD_NEEDED:
		case GSM48_MM_SST_ATTEMPT_UPDATE:
			service = ", service is limited (pending)";
			break;
		case GSM48_MM_SST_NO_CELL_AVAIL:
			service = ", service is unavailable";
			break;
		default:
			if (ms->subscr.sim_valid)
				service = ", service is limited";
			else
				service = ", service is limited "
					"(IMSI detached)";
			break;
		}
	} else
		service = ", MM connection active";

	vty_out(vty, "MS '%s' is %s%s%s%s", ms->name,
		(ms->shutdown != MS_SHUTDOWN_NONE) ? "administratively " : "",
		(ms->shutdown != MS_SHUTDOWN_NONE || !ms->started) ? "down" : "up",
		(ms->shutdown == MS_SHUTDOWN_NONE) ? service : "",
		VTY_NEWLINE);
	vty_out(vty, "  IMEI: %s%s", set->imei, VTY_NEWLINE);
	vty_out(vty, "     IMEISV: %s%s", set->imeisv, VTY_NEWLINE);
	if (set->imei_random)
		vty_out(vty, "     IMEI generation: random (%d trailing "
			"digits)%s", set->imei_random, VTY_NEWLINE);
	else
		vty_out(vty, "     IMEI generation: fixed%s", VTY_NEWLINE);

	if (ms->shutdown != MS_SHUTDOWN_NONE)
		return;

	if (set->plmn_mode == PLMN_MODE_AUTO)
		vty_out(vty, "  automatic network selection state: %s%s",
			get_a_state_name(ms->plmn.state), VTY_NEWLINE);
	else
		vty_out(vty, "  manual network selection state   : %s%s",
			get_m_state_name(ms->plmn.state), VTY_NEWLINE);
	if (ms->plmn.plmn.mcc)
		vty_out(vty, "                                     MCC=%s "
			"MNC=%s (%s, %s)%s", osmo_mcc_name(ms->plmn.plmn.mcc),
			osmo_mnc_name(ms->plmn.plmn.mnc, ms->plmn.plmn.mnc_3_digits),
			gsm_get_mcc(ms->plmn.plmn.mcc),
			gsm_get_mnc(&ms->plmn.plmn), VTY_NEWLINE);
	vty_out(vty, "  cell selection state: %s%s",
		get_cs_state_name(ms->cellsel.state), VTY_NEWLINE);
	if (ms->cellsel.sel_cgi.lai.plmn.mcc) {
		vty_out(vty, "                        ARFCN=%s CGI=%s%s",
			gsm_print_arfcn(ms->cellsel.sel_arfcn),
			osmo_cgi_name(&ms->cellsel.sel_cgi), VTY_NEWLINE);
		vty_out(vty, "                        (%s, %s)%s",
			gsm_get_mcc(ms->cellsel.sel_cgi.lai.plmn.mcc),
			gsm_get_mnc(&ms->cellsel.sel_cgi.lai.plmn),
			VTY_NEWLINE);
	}
	vty_out(vty, "  radio resource layer state: %s%s",
		gsm48_rr_state_names[ms->rrlayer.state], VTY_NEWLINE);
	vty_out(vty, "  mobility management layer state: %s",
		gsm48_mm_state_names[ms->mmlayer.state]);
	if (ms->mmlayer.state == GSM48_MM_ST_MM_IDLE)
		vty_out(vty, ", %s",
			gsm48_mm_substate_names[ms->mmlayer.substate]);
	vty_out(vty, "%s", VTY_NEWLINE);
	llist_for_each_entry(trans, &ms->trans_list, entry) {
		vty_out(vty, "  call control state: %s%s",
			gsm48_cc_state_name(trans->cc.state), VTY_NEWLINE);
	}
}


DEFUN(show_ms, show_ms_cmd, "show ms [MS_NAME]",
	SHOW_STR "Display available MS entities\n"
	"Display specific MS with given name")
{
	struct osmocom_ms *ms;

	if (argc) {
		llist_for_each_entry(ms, &ms_list, entity) {
			if (!strcmp(ms->name, argv[0])) {
				gsm_ms_dump(ms, vty);
				return CMD_SUCCESS;
			}
		}
		vty_out(vty, "MS name '%s' does not exits.%s", argv[0],
			VTY_NEWLINE);
		return CMD_WARNING;
	}

	llist_for_each_entry(ms, &ms_list, entity) {
		gsm_ms_dump(ms, vty);
		vty_out(vty, "%s", VTY_NEWLINE);
	}

	return CMD_SUCCESS;
}

DEFUN(show_cell, show_cell_cmd, "show cell MS_NAME",
	SHOW_STR "Display information about received cells\n"
	"Name of MS (see \"show ms\")")
{
	struct osmocom_ms *ms;

	ms = l23_vty_get_ms(argv[0], vty);
	if (!ms)
		return CMD_WARNING;

	gsm322_dump_cs_list(&ms->cellsel, GSM322_CS_FLAG_SUPPORT, l23_vty_printf,
		vty);

	return CMD_SUCCESS;
}

DEFUN(show_cell_si, show_cell_si_cmd, "show cell MS_NAME <0-1023> [pcs]",
	SHOW_STR "Display information about received cell\n"
	"Name of MS (see \"show ms\")\nRadio frequency number\n"
	"Given frequency is PCS band (1900) rather than DCS band.")
{
	struct osmocom_ms *ms;
	struct gsm48_sysinfo *s;
	uint16_t arfcn = atoi(argv[1]);

	ms = l23_vty_get_ms(argv[0], vty);
	if (!ms)
		return CMD_WARNING;

	if (argc > 2) {
		if (arfcn < 512 || arfcn > 810) {
			vty_out(vty, "Given ARFCN not in PCS band%s",
				VTY_NEWLINE);
			return CMD_WARNING;
		}
		arfcn |= ARFCN_PCS;
	}

	s = ms->cellsel.list[arfcn2index(arfcn)].sysinfo;
	if (!s) {
		vty_out(vty, "Given ARFCN '%s' has no sysinfo available%s",
			argv[1], VTY_NEWLINE);
		return CMD_SUCCESS;
	}

	gsm48_sysinfo_dump(s, arfcn, l23_vty_printf, vty, ms->settings.freq_map);

	return CMD_SUCCESS;
}

DEFUN(show_nbcells, show_nbcells_cmd, "show neighbour-cells MS_NAME",
	SHOW_STR "Display information about current neighbour cells\n"
	"Name of MS (see \"show ms\")")
{
	struct osmocom_ms *ms;

	ms = l23_vty_get_ms(argv[0], vty);
	if (!ms)
		return CMD_WARNING;

	gsm322_dump_nb_list(&ms->cellsel, l23_vty_printf, vty);

	return CMD_SUCCESS;
}

DEFUN(show_ba, show_ba_cmd, "show ba MS_NAME [MCC] [MNC]",
	SHOW_STR "Display information about band allocations\n"
	"Name of MS (see \"show ms\")\nMobile Country Code\n"
	"Mobile Network Code")
{
	struct osmocom_ms *ms;
	struct osmo_plmn_id plmn;
	struct osmo_plmn_id *plmn_ptr = NULL;

	ms = l23_vty_get_ms(argv[0], vty);
	if (!ms)
		return CMD_WARNING;

	if (argc >= 3) {
		if (osmo_mcc_from_str(argv[1], &plmn.mcc) < 0) {
			vty_out(vty, "Given MCC invalid%s", VTY_NEWLINE);
			return CMD_WARNING;
		}
		if (osmo_mnc_from_str(argv[2], &plmn.mnc, &plmn.mnc_3_digits) < 0) {
			vty_out(vty, "Given MNC invalid%s", VTY_NEWLINE);
			return CMD_WARNING;
		}
		plmn_ptr = &plmn;
	}

	gsm322_dump_ba_list(&ms->cellsel, plmn_ptr, l23_vty_printf, vty);

	return CMD_SUCCESS;
}

DEFUN(show_forb_plmn, show_forb_plmn_cmd, "show forbidden plmn MS_NAME",
	SHOW_STR "Display information about forbidden cells / networks\n"
	"Display forbidden PLMNs\nName of MS (see \"show ms\")")
{
	struct osmocom_ms *ms;

	ms = l23_vty_get_ms(argv[0], vty);
	if (!ms)
		return CMD_WARNING;

	gsm_subscr_dump_forbidden_plmn(ms, l23_vty_printf, vty);

	return CMD_SUCCESS;
}

DEFUN(show_forb_la, show_forb_la_cmd, "show forbidden location-area MS_NAME",
	SHOW_STR "Display information about forbidden cells / networks\n"
	"Display forbidden location areas\nName of MS (see \"show ms\")")
{
	struct osmocom_ms *ms;

	ms = l23_vty_get_ms(argv[0], vty);
	if (!ms)
		return CMD_WARNING;

	gsm322_dump_forbidden_la(ms, l23_vty_printf, vty);

	return CMD_SUCCESS;
}

DEFUN(monitor_network, monitor_network_cmd, "monitor network MS_NAME",
	"Monitor...\nMonitor network information\nName of MS (see \"show ms\")")
{
	struct osmocom_ms *ms;

	ms = l23_vty_get_ms(argv[0], vty);
	if (!ms)
		return CMD_WARNING;

	gsm48_rr_start_monitor(ms);

	return CMD_SUCCESS;
}

DEFUN(no_monitor_network, no_monitor_network_cmd, "no monitor network MS_NAME",
	NO_STR "Monitor...\nDeactivate monitor of network information\n"
	"Name of MS (see \"show ms\")")
{
	struct osmocom_ms *ms;

	ms = l23_vty_get_ms(argv[0], vty);
	if (!ms)
		return CMD_WARNING;

	gsm48_rr_stop_monitor(ms);

	return CMD_SUCCESS;
}

DEFUN(network_select, network_select_cmd,
	"network select MS_NAME MCC MNC [force]",
	"Select ...\nSelect Network\nName of MS (see \"show ms\")\n"
	"Mobile Country Code\nMobile Network Code\n"
	"Force selecting a network that is not in the list")
{
	struct osmocom_ms *ms;
	struct gsm322_plmn *plmn322;
	struct msgb *nmsg;
	struct gsm322_msg *ngm;
	struct gsm322_plmn_list *temp;
	struct osmo_plmn_id plmn;
	int found = 0;

	ms = l23_vty_get_ms(argv[0], vty);
	if (!ms)
		return CMD_WARNING;
	plmn322 = &ms->plmn;

	if (ms->settings.plmn_mode != PLMN_MODE_MANUAL) {
		vty_out(vty, "Not in manual network selection mode%s",
			VTY_NEWLINE);
		return CMD_WARNING;
	}

	if (osmo_mcc_from_str(argv[1], &plmn.mcc) < 0) {
		vty_out(vty, "Given MCC invalid%s", VTY_NEWLINE);
		return CMD_WARNING;
	}
	if (osmo_mnc_from_str(argv[2], &plmn.mnc, &plmn.mnc_3_digits) < 0) {
		vty_out(vty, "Given MNC invalid%s", VTY_NEWLINE);
		return CMD_WARNING;
	}

	if (argc < 4) {
		llist_for_each_entry(temp, &plmn322->sorted_plmn, entry)
			if (osmo_plmn_cmp(&temp->plmn, &plmn) == 0)
				found = 1;
		if (!found) {
			vty_out(vty, "Network not in list!%s", VTY_NEWLINE);
			vty_out(vty, "To force selecting this network, use "
				"'force' keyword%s", VTY_NEWLINE);
			return CMD_WARNING;
		}
	}

	nmsg = gsm322_msgb_alloc(GSM322_EVENT_CHOOSE_PLMN);
	if (!nmsg)
		return CMD_WARNING;
	ngm = (struct gsm322_msg *) nmsg->data;
	memcpy(&ngm->plmn, &plmn, sizeof(struct osmo_plmn_id));
	gsm322_plmn_sendmsg(ms, nmsg);

	return CMD_SUCCESS;
}

DEFUN(call, call_cmd, "call MS_NAME (NUMBER|emergency|answer|hangup|hold)",
	"Make a call\nName of MS (see \"show ms\")\nPhone number to call "
	"(Use digits '0123456789*#abc', and '+' to dial international)\n"
	"Make an emergency call\nAnswer an incoming call\nHangup a call\n"
	"Hold current active call\n")
{
	struct osmocom_ms *ms;
	struct gsm_settings *set;
	struct gsm_settings_abbrev *abbrev;
	char *number;

	ms = l23_vty_get_ms(argv[0], vty);
	if (!ms)
		return CMD_WARNING;
	set = &ms->settings;

	if (set->ch_cap == GSM_CAP_SDCCH) {
		vty_out(vty, "Basic call is not supported for SDCCH only "
			"mobile%s", VTY_NEWLINE);
		return CMD_WARNING;
	}

	number = (char *)argv[1];
	if (!strcmp(number, "emergency"))
		mncc_call(ms, number);
	else if (!strcmp(number, "answer"))
		mncc_answer(ms);
	else if (!strcmp(number, "hangup"))
		mncc_hangup(ms);
	else if (!strcmp(number, "hold"))
		mncc_hold(ms);
	else {
		llist_for_each_entry(abbrev, &set->abbrev, list) {
			if (!strcmp(number, abbrev->abbrev)) {
				number = abbrev->number;
				vty_out(vty, "Dialing number '%s'%s", number,
					VTY_NEWLINE);
				break;
			}
		}
		if (vty_check_number(vty, number))
			return CMD_WARNING;
		mncc_call(ms, number);
	}

	return CMD_SUCCESS;
}

DEFUN(call_retr, call_retr_cmd, "call MS_NAME retrieve [NUMBER]",
	"Make a call\nName of MS (see \"show ms\")\n"
	"Retrieve call on hold\nNumber of call to retrieve")
{
	struct osmocom_ms *ms;

	ms = l23_vty_get_ms(argv[0], vty);
	if (!ms)
		return CMD_WARNING;

	mncc_retrieve(ms, (argc > 1) ? atoi(argv[1]) : 0);

	return CMD_SUCCESS;
}

DEFUN(call_dtmf, call_dtmf_cmd, "call MS_NAME dtmf DIGITS",
	"Make a call\nName of MS (see \"show ms\")\n"
	"One or more DTMF digits to transmit")
{
	struct osmocom_ms *ms;
	struct gsm_settings *set;

	ms = l23_vty_get_ms(argv[0], vty);
	if (!ms)
		return CMD_WARNING;
	set = &ms->settings;

	if (!set->cc_dtmf) {
		vty_out(vty, "DTMF not supported, please enable!%s",
			VTY_NEWLINE);
		return CMD_WARNING;
	}

	mncc_dtmf(ms, (char *)argv[1]);

	return CMD_SUCCESS;
}

DEFUN(sms, sms_cmd, "sms MS_NAME NUMBER .LINE",
	"Send an SMS\nName of MS (see \"show ms\")\nPhone number to send SMS "
	"(Use digits '0123456789*#abc', and '+' to dial international)\n"
	"SMS text\n")
{
	struct osmocom_ms *ms;
	struct gsm_settings *set;
	struct gsm_settings_abbrev *abbrev;
	char *number, *sms_sca = NULL;

	ms = l23_vty_get_ms(argv[0], vty);
	if (!ms)
		return CMD_WARNING;
	set = &ms->settings;

	if (!set->sms_ptp) {
		vty_out(vty, "SMS not supported by this mobile, please enable "
			"SMS support%s", VTY_NEWLINE);
		return CMD_WARNING;
	}

	if (ms->subscr.sms_sca[0])
		sms_sca = ms->subscr.sms_sca;
	else if (set->sms_sca[0])
		sms_sca = set->sms_sca;

	if (!sms_sca) {
		vty_out(vty, "SMS sms-service-center not defined on SIM card, "
			"please define one at settings.%s", VTY_NEWLINE);
		return CMD_WARNING;
	}

	number = (char *)argv[1];
	llist_for_each_entry(abbrev, &set->abbrev, list) {
		if (!strcmp(number, abbrev->abbrev)) {
			number = abbrev->number;
			vty_out(vty, "Using number '%s'%s", number,
				VTY_NEWLINE);
			break;
		}
	}
	if (vty_check_number(vty, number))
		return CMD_WARNING;

	sms_send(ms, sms_sca, number, argv_concat(argv, argc, 2), 42);

	return CMD_SUCCESS;
}

DEFUN(service, service_cmd, "service MS_NAME (*#06#|*#21#|*#67#|*#61#|*#62#"
	"|*#002#|*#004#|*xx*number#|*xx#|#xx#|##xx#|STRING|hangup)",
	"Send a Supplementary Service request\nName of MS (see \"show ms\")\n"
	"Query IMSI\n"
	"Query Call Forwarding Unconditional (CFU)\n"
	"Query Call Forwarding when Busy (CFB)\n"
	"Query Call Forwarding when No Response (CFNR)\n"
	"Query Call Forwarding when Not Reachable\n"
	"Query all Call Forwardings\n"
	"Query all conditional Call Forwardings\n"
	"Set and activate Call Forwarding (xx = Service Code, see above)\n"
	"Activate Call Forwarding (xx = Service Code, see above)\n"
	"Deactivate Call Forwarding (xx = Service Code, see above)\n"
	"Erase and deactivate Call Forwarding (xx = Service Code, see above)\n"
	"Service string "
	"(Example: '*100#' requests account balace on some networks.)\n"
	"Hangup existing service connection")
{
	struct osmocom_ms *ms;

	ms = l23_vty_get_ms(argv[0], vty);
	if (!ms)
		return CMD_WARNING;

	ss_send(ms, argv[1], 0);

	return CMD_SUCCESS;
}

#define TEST_CMD_DESC	"Special commands for testing\n"

DEFUN(test_reselection, test_reselection_cmd, "test re-selection NAME",
      TEST_CMD_DESC "Manually trigger cell re-selection\n"
      "Name of MS (see \"show ms\")")
{
	struct osmocom_ms *ms;
	struct gsm_settings *set;
	struct msgb *nmsg;

	ms = l23_vty_get_ms(argv[0], vty);
	if (!ms)
		return CMD_WARNING;
	set = &ms->settings;

	if (set->stick) {
		vty_out(vty, "Cannot trigger cell re-selection, because we "
			"stick to a cell!%s", VTY_NEWLINE);
		return CMD_WARNING;
	}

	nmsg = gsm322_msgb_alloc(GSM322_EVENT_CELL_RESEL);
	if (!nmsg)
		return CMD_WARNING;
	gsm322_c_event(ms, nmsg);


	return CMD_SUCCESS;
}

DEFUN(delete_forbidden_plmn, delete_forbidden_plmn_cmd,
	"delete forbidden plmn NAME MCC MNC",
	"Delete\nForbidden\nplmn\nName of MS (see \"show ms\")\n"
	"Mobile Country Code\nMobile Network Code")
{
	struct osmocom_ms *ms;
	struct osmo_plmn_id plmn;

	ms = l23_vty_get_ms(argv[0], vty);
	if (!ms)
		return CMD_WARNING;

	if (osmo_mcc_from_str(argv[1], &plmn.mcc) < 0) {
		vty_out(vty, "Given MCC invalid%s", VTY_NEWLINE);
		return CMD_WARNING;
	}
	if (osmo_mnc_from_str(argv[2], &plmn.mnc, &plmn.mnc_3_digits) < 0) {
		vty_out(vty, "Given MNC invalid%s", VTY_NEWLINE);
		return CMD_WARNING;
	}

	gsm_subscr_del_forbidden_plmn(&ms->subscr, &plmn);

	return CMD_SUCCESS;
}

DEFUN(network_show, network_show_cmd, "network show MS_NAME",
	"Network ...\nShow results of network search (again)\n"
	"Name of MS (see \"show ms\")")
{
	struct osmocom_ms *ms;
	struct gsm_settings *set;
	struct gsm322_plmn *plmn;
	struct gsm322_plmn_list *temp;

	ms = l23_vty_get_ms(argv[0], vty);
	if (!ms)
		return CMD_WARNING;
	set = &ms->settings;
	plmn = &ms->plmn;

	if (set->plmn_mode != PLMN_MODE_AUTO
	 && plmn->state != GSM322_M3_NOT_ON_PLMN) {
		vty_out(vty, "Start network search first!%s", VTY_NEWLINE);
		return CMD_WARNING;
	}

	llist_for_each_entry(temp, &plmn->sorted_plmn, entry)
		vty_out(vty, " Network %s, %s (%s, %s)%s",
			osmo_mcc_name(temp->plmn.mcc),
			osmo_mnc_name(temp->plmn.mnc, temp->plmn.mnc_3_digits),
			gsm_get_mcc(temp->plmn.mcc),
			gsm_get_mnc(&temp->plmn), VTY_NEWLINE);

	return CMD_SUCCESS;
}

DEFUN(network_search, network_search_cmd, "network search MS_NAME",
	"Network ...\nTrigger network search\nName of MS (see \"show ms\")")
{
	struct osmocom_ms *ms;
	struct msgb *nmsg;

	ms = l23_vty_get_ms(argv[0], vty);
	if (!ms)
		return CMD_WARNING;

	nmsg = gsm322_msgb_alloc(GSM322_EVENT_USER_RESEL);
	if (!nmsg)
		return CMD_WARNING;
	gsm322_plmn_sendmsg(ms, nmsg);

	return CMD_SUCCESS;
}

DEFUN(cfg_gps_enable, cfg_gps_enable_cmd, "gps enable",
	"GPS receiver")
{
	if (osmo_gps_open()) {
		g.enable = 1;
		vty_out(vty, "Failed to open GPS device!%s", VTY_NEWLINE);
		return CMD_WARNING;
	}
	g.enable = 1;

	return CMD_SUCCESS;
}

DEFUN(cfg_no_gps_enable, cfg_no_gps_enable_cmd, "no gps enable",
	NO_STR "Disable GPS receiver")
{
	if (g.enable)
		osmo_gps_close();
	g.enable = 0;

	return CMD_SUCCESS;
}

#ifdef _HAVE_GPSD
DEFUN(cfg_gps_host, cfg_gps_host_cmd, "gps host HOST:PORT",
	"GPS receiver\nSelect gpsd host and port\n"
	"IP and port (optional) of the host running gpsd")
{
	char* colon = strstr(argv[0], ":");
	if (colon != NULL) {
		memcpy(g.gpsd_host, argv[0], colon - argv[0]);
		g.gpsd_host[colon - argv[0]] = '\0';
		memcpy(g.gpsd_port, colon+1, strlen(colon+1));
		g.gpsd_port[strlen(colon+1)] = '\0';
	} else {
		snprintf(g.gpsd_host, ARRAY_SIZE(g.gpsd_host), "%s", argv[0]);
		g.gpsd_host[ARRAY_SIZE(g.gpsd_host) - 1] = '\0';
		snprintf(g.gpsd_port, ARRAY_SIZE(g.gpsd_port), "2947");
		g.gpsd_port[ARRAY_SIZE(g.gpsd_port) - 1] = '\0';
	}
	g.gps_type = GPS_TYPE_GPSD;
	if (g.enable) {
		osmo_gps_close();
		if (osmo_gps_open()) {
			vty_out(vty, "Failed to connect to gpsd host!%s",
				VTY_NEWLINE);
			return CMD_WARNING;
		}
	}

	return CMD_SUCCESS;
}
#endif

DEFUN(cfg_gps_device, cfg_gps_device_cmd, "gps device DEVICE",
	"GPS receiver\nSelect serial device\n"
	"Full path of serial device including /dev/")
{
	osmo_strlcpy(g.device, argv[0], sizeof(g.device));
	g.device[sizeof(g.device) - 1] = '\0';
	g.gps_type = GPS_TYPE_SERIAL;
	if (g.enable) {
		osmo_gps_close();
		if (osmo_gps_open()) {
			vty_out(vty, "Failed to open GPS device!%s",
				VTY_NEWLINE);
			return CMD_WARNING;
		}
	}

	return CMD_SUCCESS;
}

DEFUN(cfg_gps_baud, cfg_gps_baud_cmd, "gps baudrate "
	"(default|4800|""9600|19200|38400|57600|115200)",
	"GPS receiver\nSelect baud rate\nDefault, don't modify\n\n\n\n\n\n")
{
	if (argv[0][0] == 'd')
		g.baud = 0;
	else
		g.baud = atoi(argv[0]);
	if (g.enable) {
		osmo_gps_close();
		if (osmo_gps_open()) {
			g.enable = 0;
			vty_out(vty, "Failed to open GPS device!%s",
				VTY_NEWLINE);
			return CMD_WARNING;
		}
	}

	return CMD_SUCCESS;
}

/* per MS config */
DEFUN(cfg_ms, cfg_ms_cmd, "ms MS_NAME",
	"Select a mobile station to configure\nName of MS (see \"show ms\")")
{
	struct osmocom_ms *ms;
	int found = 0;

	llist_for_each_entry(ms, &ms_list, entity) {
		if (!strcmp(ms->name, argv[0])) {
			found = 1;
			break;
		}
	}

	if (!found) {
		if (!l23_vty_reading) {
			vty_out(vty, "MS name '%s' does not exits, try "
				"'ms %s create'%s", argv[0], argv[0],
				VTY_NEWLINE);
			return CMD_WARNING;
		}
		ms = mobile_new((char *)argv[0]);
		if (!ms) {
			vty_out(vty, "Failed to add MS name '%s'%s", argv[0],
				VTY_NEWLINE);
			return CMD_WARNING;
		}
	}

	vty->index = ms;
	vty->node = MS_NODE;

	return CMD_SUCCESS;
}

DEFUN(cfg_ms_create, cfg_ms_create_cmd, "ms MS_NAME create",
	"Select a mobile station to configure\nName of MS (see \"show ms\")\n"
	"Create if MS does not exists")
{
	struct osmocom_ms *ms;
	int found = 0;

	llist_for_each_entry(ms, &ms_list, entity) {
		if (!strcmp(ms->name, argv[0])) {
			found = 1;
			break;
		}
	}

	if (!found) {
		ms = mobile_new((char *)argv[0]);
		if (!ms) {
			vty_out(vty, "Failed to add MS name '%s'%s", argv[0],
				VTY_NEWLINE);
			return CMD_WARNING;
		}
	}

	vty->index = ms;
	vty->node = MS_NODE;

	vty_out(vty, "MS '%s' created, after configuration, do 'no shutdown'%s",
		argv[0], VTY_NEWLINE);
	return CMD_SUCCESS;
}

DEFUN(cfg_ms_rename, cfg_ms_rename_cmd, "ms MS_NAME rename MS_NAME",
	"Select a mobile station to configure\nName of MS (see \"show ms\")\n"
	"Rename MS\nNew name of MS")
{
	struct osmocom_ms *ms;
	int found = 0;

	llist_for_each_entry(ms, &ms_list, entity) {
		if (!strcmp(ms->name, argv[0])) {
			found = 1;
			break;
		}
	}

	if (!found) {
		vty_out(vty, "MS name '%s' does not exist%s", argv[0],
			VTY_NEWLINE);
		return CMD_WARNING;
	}

	osmo_talloc_replace_string(ms, &ms->name, argv[1]);

	return CMD_SUCCESS;
}

DEFUN(cfg_no_ms, cfg_no_ms_cmd, "no ms MS_NAME",
	NO_STR "Select a mobile station to remove\n"
	"Name of MS (see \"show ms\")")
{
	struct osmocom_ms *ms;
	int found = 0;

	llist_for_each_entry(ms, &ms_list, entity) {
		if (!strcmp(ms->name, argv[0])) {
			found = 1;
			break;
		}
	}

	if (!found) {
		vty_out(vty, "MS name '%s' does not exist%s", argv[0],
			VTY_NEWLINE);
		return CMD_WARNING;
	}

	mobile_delete(ms, 1);

	return CMD_SUCCESS;
}

#define SUP_WRITE(item, cmd) \
	if (sup->item) \
		if (!l23_vty_hide_default || !set->item) \
			vty_out(vty, "  %s%s%s", (set->item) ? "" : "no ", \
			cmd, VTY_NEWLINE);

static void config_write_ms(struct vty *vty, struct osmocom_ms *ms)
{
	struct gsm_settings *set = &ms->settings;
	struct gsm_support *sup = &ms->support;
	struct gsm_settings_abbrev *abbrev;

	vty_out(vty, "ms %s%s", ms->name, VTY_NEWLINE);

	l23_vty_config_write_ms_node_contents(vty, ms, " ");

	vty_out(vty, " sap-socket %s%s", set->sap_socket_path, VTY_NEWLINE);
	vty_out(vty, " mncc-socket %s%s", set->mncc_socket_path, VTY_NEWLINE);
	switch (set->mncc_handler) {
	case MNCC_HANDLER_INTERNAL:
		vty_out(vty, " mncc-handler internal%s", VTY_NEWLINE);
		break;
	case MNCC_HANDLER_EXTERNAL:
		vty_out(vty, " mncc-handler external%s", VTY_NEWLINE);
		break;
	case MNCC_HANDLER_DUMMY:
		vty_out(vty, " mncc-handler dummy%s", VTY_NEWLINE);
	}
	vty_out(vty, " network-selection-mode %s%s", (set->plmn_mode
			== PLMN_MODE_AUTO) ? "auto" : "manual", VTY_NEWLINE);
	if (set->emergency_imsi[0])
		vty_out(vty, " emergency-imsi %s%s", set->emergency_imsi,
			VTY_NEWLINE);
	else
		if (!l23_vty_hide_default)
			vty_out(vty, " no emergency-imsi%s", VTY_NEWLINE);
	if (set->sms_sca[0])
		vty_out(vty, " sms-service-center %s%s", set->sms_sca,
			VTY_NEWLINE);
	else
		if (!l23_vty_hide_default)
			vty_out(vty, " no sms-service-center%s", VTY_NEWLINE);
	if (!l23_vty_hide_default || set->cw)
		vty_out(vty, " %scall-waiting%s", (set->cw) ? "" : "no ",
			VTY_NEWLINE);
	if (!l23_vty_hide_default || set->auto_answer)
		vty_out(vty, " %sauto-answer%s",
			(set->auto_answer) ? "" : "no ", VTY_NEWLINE);
	if (!l23_vty_hide_default || set->force_rekey)
		vty_out(vty, " %sforce-rekey%s",
			(set->force_rekey) ? "" : "no ", VTY_NEWLINE);
	if (!l23_vty_hide_default || set->clip)
		vty_out(vty, " %sclip%s", (set->clip) ? "" : "no ",
			VTY_NEWLINE);
	if (!l23_vty_hide_default || set->clir)
		vty_out(vty, " %sclir%s", (set->clir) ? "" : "no ",
			VTY_NEWLINE);
	if (set->alter_tx_power)
		if (set->alter_tx_power_value)
			vty_out(vty, " tx-power %d%s",
				set->alter_tx_power_value, VTY_NEWLINE);
		else
			vty_out(vty, " tx-power full%s", VTY_NEWLINE);
	else
		if (!l23_vty_hide_default)
			vty_out(vty, " tx-power auto%s", VTY_NEWLINE);
	if (set->alter_delay)
		vty_out(vty, " simulated-delay %d%s", set->alter_delay,
			VTY_NEWLINE);
	else
		if (!l23_vty_hide_default)
			vty_out(vty, " no simulated-delay%s", VTY_NEWLINE);
	if (set->stick)
		vty_out(vty, " stick %d%s%s", set->stick_arfcn & 1023,
			(set->stick_arfcn & ARFCN_PCS) ? " pcs" : "",
			VTY_NEWLINE);
	else
		if (!l23_vty_hide_default)
			vty_out(vty, " no stick%s", VTY_NEWLINE);
	if (!l23_vty_hide_default || set->no_lupd)
		vty_out(vty, " %slocation-updating%s",
			(set->no_lupd) ? "no " : "", VTY_NEWLINE);
	if (!l23_vty_hide_default || set->no_neighbour)
		vty_out(vty, " %sneighbour-measurement%s",
			(set->no_neighbour) ? "no " : "", VTY_NEWLINE);
	if (set->full_v1 || set->full_v2 || set->full_v3) {
		/* mandatory anyway */
		vty_out(vty, " codec full-speed%s%s",
			(!set->half_prefer) ? " prefer" : "",
			VTY_NEWLINE);
	}
	if (set->half_v1 || set->half_v3) {
		if (set->half)
			vty_out(vty, " codec half-speed%s%s",
				(set->half_prefer) ? " prefer" : "",
				VTY_NEWLINE);
		else
			vty_out(vty, " no codec half-speed%s", VTY_NEWLINE);
	}
	if (llist_empty(&set->abbrev)) {
		if (!l23_vty_hide_default)
			vty_out(vty, " no abbrev%s", VTY_NEWLINE);
	} else {
		llist_for_each_entry(abbrev, &set->abbrev, list)
			vty_out(vty, " abbrev %s %s%s%s%s", abbrev->abbrev,
				abbrev->number, (abbrev->name[0]) ? " " : "",
				abbrev->name, VTY_NEWLINE);
	}
	vty_out(vty, " support%s", VTY_NEWLINE);
	SUP_WRITE(sms_ptp, "sms");
	SUP_WRITE(a5_1, "a5/1");
	SUP_WRITE(a5_2, "a5/2");
	SUP_WRITE(a5_3, "a5/3");
	SUP_WRITE(a5_4, "a5/4");
	SUP_WRITE(a5_5, "a5/5");
	SUP_WRITE(a5_6, "a5/6");
	SUP_WRITE(a5_7, "a5/7");
	SUP_WRITE(p_gsm, "p-gsm");
	SUP_WRITE(e_gsm, "e-gsm");
	SUP_WRITE(r_gsm, "r-gsm");
	SUP_WRITE(pcs, "gsm-850");
	SUP_WRITE(gsm_480, "gsm-480");
	SUP_WRITE(gsm_450, "gsm-450");
	SUP_WRITE(dcs, "dcs");
	SUP_WRITE(pcs, "pcs");
	if (sup->r_gsm || sup->e_gsm || sup->p_gsm)
		if (!l23_vty_hide_default || sup->class_900 != set->class_900)
			vty_out(vty, "  class-900 %d%s", set->class_900,
				VTY_NEWLINE);
	if (sup->gsm_850)
		if (!l23_vty_hide_default || sup->class_850 != set->class_850)
			vty_out(vty, "  class-850 %d%s", set->class_850,
				VTY_NEWLINE);
	if (sup->gsm_480 || sup->gsm_450)
		if (!l23_vty_hide_default || sup->class_400 != set->class_400)
			vty_out(vty, "  class-400 %d%s", set->class_400,
				VTY_NEWLINE);
	if (sup->dcs)
		if (!l23_vty_hide_default || sup->class_dcs != set->class_dcs)
			vty_out(vty, "  class-dcs %d%s", set->class_dcs,
				VTY_NEWLINE);
	if (sup->pcs)
		if (!l23_vty_hide_default || sup->class_pcs != set->class_pcs)
			vty_out(vty, "  class-pcs %d%s", set->class_pcs,
				VTY_NEWLINE);
	if (!l23_vty_hide_default || sup->ch_cap != set->ch_cap) {
		switch (set->ch_cap) {
		case GSM_CAP_SDCCH:
			vty_out(vty, "  channel-capability sdcch%s",
				VTY_NEWLINE);
			break;
		case GSM_CAP_SDCCH_TCHF:
			vty_out(vty, "  channel-capability sdcch+tchf%s",
				VTY_NEWLINE);
			break;
		case GSM_CAP_SDCCH_TCHF_TCHH:
			vty_out(vty, "  channel-capability sdcch+tchf+tchh%s",
				VTY_NEWLINE);
			break;
		}
	}
	SUP_WRITE(full_v1, "full-speech-v1");
	SUP_WRITE(full_v2, "full-speech-v2");
	SUP_WRITE(full_v3, "full-speech-v3");
	SUP_WRITE(half_v1, "half-speech-v1");
	SUP_WRITE(half_v3, "half-speech-v3");
	if (!l23_vty_hide_default || sup->min_rxlev_dbm != set->min_rxlev_dbm)
		vty_out(vty, "  min-rxlev %d%s", set->min_rxlev_dbm,
			VTY_NEWLINE);
	if (!l23_vty_hide_default || sup->dsc_max != set->dsc_max)
		vty_out(vty, "  dsc-max %d%s", set->dsc_max, VTY_NEWLINE);
	if (!l23_vty_hide_default || set->skip_max_per_band)
		vty_out(vty, "  %sskip-max-per-band%s",
			(set->skip_max_per_band) ? "" : "no ", VTY_NEWLINE);
	if (!l23_vty_hide_default || set->any_timeout != MOB_C7_DEFLT_ANY_TIMEOUT)
		vty_out(vty, " c7-any-timeout %d%s",
			set->any_timeout, VTY_NEWLINE);

	vty_out(vty, " audio%s", VTY_NEWLINE);
	vty_out(vty, "  io-handler %s%s",
		audio_io_handler_name(set->audio.io_handler), VTY_NEWLINE);
	if (set->audio.io_handler == AUDIO_IOH_GAPK) {
		vty_out(vty, "  io-tch-format %s%s",
			audio_io_format_name(set->audio.io_format), VTY_NEWLINE);
		vty_out(vty, "  alsa-output-dev %s%s",
			set->audio.alsa_output_dev, VTY_NEWLINE);
		vty_out(vty, "  alsa-input-dev %s%s",
			set->audio.alsa_input_dev, VTY_NEWLINE);
	}

	if (ms->lua_script)
		vty_out(vty, " lua-script %s%s", ms->lua_script, VTY_NEWLINE);

	l23_vty_config_write_ms_node_contents_final(vty, ms, " ");
}

static int config_write(struct vty *vty)
{
	struct osmocom_ms *ms;

#ifdef _HAVE_GPSD
	vty_out(vty, "gps host %s:%s%s", g.gpsd_host, g.gpsd_port, VTY_NEWLINE);
#endif
	vty_out(vty, "gps device %s%s", g.device, VTY_NEWLINE);
	if (g.baud)
		vty_out(vty, "gps baudrate %d%s", g.baud, VTY_NEWLINE);
	else
		vty_out(vty, "gps baudrate default%s", VTY_NEWLINE);
	vty_out(vty, "%sgps enable%s", (g.enable) ? "" : "no ", VTY_NEWLINE);
	vty_out(vty, "!%s", VTY_NEWLINE);

	vty_out(vty, "%shide-default%s", (l23_vty_hide_default) ? "" : "no ",
		VTY_NEWLINE);
	vty_out(vty, "!%s", VTY_NEWLINE);

	llist_for_each_entry(ms, &ms_list, entity)
		config_write_ms(vty, ms);

	return CMD_SUCCESS;
}

DEFUN(cfg_ms_show_this, cfg_ms_show_this_cmd, "show this",
	SHOW_STR "Show config of this MS")
{
	struct osmocom_ms *ms = vty->index;

	config_write_ms(vty, ms);

	return CMD_SUCCESS;
}

DEFUN(cfg_ms_sap, cfg_ms_sap_cmd, "sap-socket PATH",
	"Define socket path to connect to SIM reader\n"
	"Unix socket, default '/tmp/osmocom_sap'")
{
	struct osmocom_ms *ms = vty->index;
	struct gsm_settings *set = &ms->settings;

	OSMO_STRLCPY_ARRAY(set->sap_socket_path, argv[0]);

	vty_restart(vty, ms);
	return CMD_SUCCESS;
}

DEFUN(cfg_ms_mncc_sock, cfg_ms_mncc_sock_cmd, "mncc-socket PATH",
	"Define socket path for MNCC interface\n"
	"UNIX socket path (default '/tmp/ms_mncc_' + MS_NAME)")
{
	struct osmocom_ms *ms = vty->index;
	struct gsm_settings *set = &ms->settings;

	OSMO_STRLCPY_ARRAY(set->mncc_socket_path, argv[0]);

	vty_restart(vty, ms);
	return CMD_SUCCESS;
}

DEFUN(cfg_ms_mncc_handler, cfg_ms_mncc_handler_cmd,
      "mncc-handler (internal|external|dummy)",
      "Set MNCC (Call Control) handler\n"
      "Built-in MNCC handler (default)\n"
      "External MNCC application via UNIX-socket (e.g. LCR)\n"
      "Dummy MNCC handler (no Call Control)\n")
{
	struct osmocom_ms *ms = vty->index;
	struct gsm_settings *set = &ms->settings;

	switch (argv[0][0]) {
	case 'i':
		if (set->ch_cap == GSM_CAP_SDCCH) { /* SDCCH only */
			vty_out(vty, "TCH support is disabled, "
				"check 'channel-capability' param%s", VTY_NEWLINE);
			return CMD_WARNING;
		}
		set->mncc_handler = MNCC_HANDLER_INTERNAL;
		break;
	case 'e':
		set->mncc_handler = MNCC_HANDLER_EXTERNAL;
		break;
	case 'd':
		set->mncc_handler = MNCC_HANDLER_DUMMY;
		break;
	default:
		/* Shall not happen */
		OSMO_ASSERT(0);
	}

	vty_restart_if_started(vty, ms);
	return CMD_SUCCESS;
}

DEFUN(cfg_ms_no_mncc_handler, cfg_ms_no_mncc_handler_cmd,
      "no mncc-handler", NO_STR "Disable Call Control")
{
	struct osmocom_ms *ms = vty->index;
	struct gsm_settings *set = &ms->settings;

	set->mncc_handler = MNCC_HANDLER_DUMMY;

	vty_restart_if_started(vty, ms);
	return CMD_SUCCESS;
}

DEFUN(cfg_ms_mode, cfg_ms_mode_cmd, "network-selection-mode (auto|manual)",
	"Set network selection mode\nAutomatic network selection\n"
	"Manual network selection")
{
	struct osmocom_ms *ms = vty->index;
	struct gsm_settings *set = &ms->settings;
	struct msgb *nmsg;

	if (!ms->started) {
		if (argv[0][0] == 'a')
			set->plmn_mode = PLMN_MODE_AUTO;
		else
			set->plmn_mode = PLMN_MODE_MANUAL;
	} else {
		if (argv[0][0] == 'a')
			nmsg = gsm322_msgb_alloc(GSM322_EVENT_SEL_AUTO);
		else
			nmsg = gsm322_msgb_alloc(GSM322_EVENT_SEL_MANUAL);
		if (!nmsg)
			return CMD_WARNING;
		gsm322_plmn_sendmsg(ms, nmsg);
	}

	return CMD_SUCCESS;
}

DEFUN(cfg_ms_emerg_imsi, cfg_ms_emerg_imsi_cmd, "emergency-imsi IMSI",
	"Use special IMSI for emergency calls\n15 digits IMSI")
{
	struct osmocom_ms *ms = vty->index;
	struct gsm_settings *set = &ms->settings;

	if (!osmo_imsi_str_valid(argv[0])) {
		vty_out(vty, "Wrong IMSI format%s", VTY_NEWLINE);
		return CMD_WARNING;
	}
	strcpy(set->emergency_imsi, argv[0]);

	return CMD_SUCCESS;
}

DEFUN(cfg_ms_no_emerg_imsi, cfg_ms_no_emerg_imsi_cmd, "no emergency-imsi",
	NO_STR "Use IMSI of SIM or IMEI for emergency calls")
{
	struct osmocom_ms *ms = vty->index;
	struct gsm_settings *set = &ms->settings;

	set->emergency_imsi[0] = '\0';

	return CMD_SUCCESS;
}

DEFUN(cfg_ms_sms_sca, cfg_ms_sms_sca_cmd, "sms-service-center NUMBER",
	"Use Service center address for outgoing SMS\nNumber of service center "
	"(Use digits '0123456789*#abc', and '+' to dial international)")
{
	struct osmocom_ms *ms = vty->index;
	struct gsm_settings *set = &ms->settings;
	const char *number = argv[0];

	if ((strlen(number) > 20 && number[0] != '+') || strlen(number) > 21) {
		vty_out(vty, "Number too long%s", VTY_NEWLINE);
		return CMD_WARNING;
	}
	if (vty_check_number(vty, number))
		return CMD_WARNING;

	strcpy(set->sms_sca, number);

	return CMD_SUCCESS;
}

DEFUN(cfg_ms_no_sms_sca, cfg_ms_no_sms_sca_cmd, "no sms-service-center",
	NO_STR "Use Service center address for outgoing SMS")
{
	struct osmocom_ms *ms = vty->index;
	struct gsm_settings *set = &ms->settings;

	set->sms_sca[0] = '\0';

	return CMD_SUCCESS;
}

DEFUN(cfg_ms_no_sms_store, cfg_ms_no_sms_store_cmd, "no sms-store",
	NO_STR "Store SMS in the home directory")
{
	struct osmocom_ms *ms = vty->index;
	struct gsm_settings *set = &ms->settings;

	set->store_sms = false;
	return CMD_SUCCESS;
}

DEFUN(cfg_ms_sms_store, cfg_ms_sms_store_cmd, "sms-store",
	"Store SMS in the home directory")
{
	struct osmocom_ms *ms = vty->index;
	struct gsm_settings *set = &ms->settings;

	set->store_sms = true;
	return CMD_SUCCESS;
}

DEFUN(cfg_no_cw, cfg_ms_no_cw_cmd, "no call-waiting",
	NO_STR "Disallow waiting calls")
{
	struct osmocom_ms *ms = vty->index;
	struct gsm_settings *set = &ms->settings;

	set->cw = 0;

	return CMD_SUCCESS;
}

DEFUN(cfg_cw, cfg_ms_cw_cmd, "call-waiting",
	"Allow waiting calls")
{
	struct osmocom_ms *ms = vty->index;
	struct gsm_settings *set = &ms->settings;

	set->cw = 1;

	return CMD_SUCCESS;
}

DEFUN(cfg_no_auto_answer, cfg_ms_no_auto_answer_cmd, "no auto-answer",
	NO_STR "Disable auto-answering calls")
{
	struct osmocom_ms *ms = vty->index;
	struct gsm_settings *set = &ms->settings;

	set->auto_answer = 0;

	return CMD_SUCCESS;
}

DEFUN(cfg_auto_answer, cfg_ms_auto_answer_cmd, "auto-answer",
	"Enable auto-answering calls")
{
	struct osmocom_ms *ms = vty->index;
	struct gsm_settings *set = &ms->settings;

	set->auto_answer = 1;

	return CMD_SUCCESS;
}

DEFUN(cfg_no_force_rekey, cfg_ms_no_force_rekey_cmd, "no force-rekey",
	NO_STR "Disable key renew forcing after every event")
{
	struct osmocom_ms *ms = vty->index;
	struct gsm_settings *set = &ms->settings;

	set->force_rekey = 0;

	return CMD_SUCCESS;
}

DEFUN(cfg_force_rekey, cfg_ms_force_rekey_cmd, "force-rekey",
	"Enable key renew forcing after every event")
{
	struct osmocom_ms *ms = vty->index;
	struct gsm_settings *set = &ms->settings;

	set->force_rekey = 1;

	return CMD_SUCCESS;
}

DEFUN(cfg_clip, cfg_ms_clip_cmd, "clip",
	"Force caller ID presentation")
{
	struct osmocom_ms *ms = vty->index;
	struct gsm_settings *set = &ms->settings;

	set->clip = 1;
	set->clir = 0;

	return CMD_SUCCESS;
}

DEFUN(cfg_clir, cfg_ms_clir_cmd, "clir",
	"Force caller ID restriction")
{
	struct osmocom_ms *ms = vty->index;
	struct gsm_settings *set = &ms->settings;

	set->clip = 0;
	set->clir = 1;

	return CMD_SUCCESS;
}

DEFUN(cfg_no_clip, cfg_ms_no_clip_cmd, "no clip",
	NO_STR "Disable forcing of caller ID presentation")
{
	struct osmocom_ms *ms = vty->index;
	struct gsm_settings *set = &ms->settings;

	set->clip = 0;

	return CMD_SUCCESS;
}

DEFUN(cfg_no_clir, cfg_ms_no_clir_cmd, "no clir",
	NO_STR "Disable forcing of caller ID restriction")
{
	struct osmocom_ms *ms = vty->index;
	struct gsm_settings *set = &ms->settings;

	set->clir = 0;

	return CMD_SUCCESS;
}

DEFUN(cfg_ms_tx_power, cfg_ms_tx_power_cmd, "tx-power (auto|full)",
	"Set the way to choose transmit power\nControlled by BTS\n"
	"Always full power\nFixed GSM power value if supported")
{
	struct osmocom_ms *ms = vty->index;
	struct gsm_settings *set = &ms->settings;

	switch (argv[0][0]) {
	case 'a':
		set->alter_tx_power = 0;
		break;
	case 'f':
		set->alter_tx_power = 1;
		set->alter_tx_power_value = 0;
		break;
	}

	return CMD_SUCCESS;
}

DEFUN(cfg_ms_tx_power_val, cfg_ms_tx_power_val_cmd, "tx-power <0-31>",
	"Set the way to choose transmit power\n"
	"Fixed GSM power value if supported")
{
	struct osmocom_ms *ms = vty->index;
	struct gsm_settings *set = &ms->settings;

	set->alter_tx_power = 1;
	set->alter_tx_power_value = atoi(argv[0]);

	return CMD_SUCCESS;
}

DEFUN(cfg_ms_sim_delay, cfg_ms_sim_delay_cmd, "simulated-delay <-128-127>",
	"Simulate a lower or higher distance from the BTS\n"
	"Delay in half bits (distance in 553.85 meter steps)")
{
	struct osmocom_ms *ms = vty->index;
	struct gsm_settings *set = &ms->settings;

	set->alter_delay = atoi(argv[0]);
	gsm48_rr_alter_delay(ms);

	return CMD_SUCCESS;
}

DEFUN(cfg_ms_no_sim_delay, cfg_ms_no_sim_delay_cmd, "no simulated-delay",
	NO_STR "Do not simulate a lower or higher distance from the BTS")
{
	struct osmocom_ms *ms = vty->index;
	struct gsm_settings *set = &ms->settings;

	set->alter_delay = 0;
	gsm48_rr_alter_delay(ms);

	return CMD_SUCCESS;
}

DEFUN(cfg_ms_stick, cfg_ms_stick_cmd, "stick <0-1023> [pcs]",
	"Stick to the given cell\nARFCN of the cell to stick to\n"
	"Given frequency is PCS band (1900) rather than DCS band.")
{
	struct osmocom_ms *ms = vty->index;
	struct gsm_settings *set = &ms->settings;
	uint16_t arfcn = atoi(argv[0]);

	if (argc > 1) {
		if (arfcn < 512 || arfcn > 810) {
			vty_out(vty, "Given ARFCN not in PCS band%s",
				VTY_NEWLINE);
			return CMD_WARNING;
		}
		arfcn |= ARFCN_PCS;
	}
	set->stick = 1;
	set->stick_arfcn = arfcn;

	return CMD_SUCCESS;
}

DEFUN(cfg_ms_no_stick, cfg_ms_no_stick_cmd, "no stick",
	NO_STR "Do not stick to any cell")
{
	struct osmocom_ms *ms = vty->index;
	struct gsm_settings *set = &ms->settings;

	set->stick = 0;

	return CMD_SUCCESS;
}

DEFUN(cfg_ms_lupd, cfg_ms_lupd_cmd, "location-updating",
	"Allow location updating")
{
	struct osmocom_ms *ms = vty->index;
	struct gsm_settings *set = &ms->settings;

	set->no_lupd = 0;

	return CMD_SUCCESS;
}

DEFUN(cfg_ms_no_lupd, cfg_ms_no_lupd_cmd, "no location-updating",
	NO_STR "Do not allow location updating")
{
	struct osmocom_ms *ms = vty->index;
	struct gsm_settings *set = &ms->settings;

	set->no_lupd = 1;

	return CMD_SUCCESS;
}

DEFUN(cfg_codec_full, cfg_ms_codec_full_cmd, "codec full-speed",
	"Enable codec\nFull speed speech codec")
{
	struct osmocom_ms *ms = vty->index;
	struct gsm_settings *set = &ms->settings;

	if (!set->full_v1 && !set->full_v2 && !set->full_v3) {
		vty_out(vty, "Full-rate codec not supported%s", VTY_NEWLINE);
		return CMD_WARNING;
	}

	return CMD_SUCCESS;
}

DEFUN(cfg_codec_full_pref, cfg_ms_codec_full_pref_cmd, "codec full-speed "
	"prefer",
	"Enable codec\nFull speed speech codec\nPrefer this codec")
{
	struct osmocom_ms *ms = vty->index;
	struct gsm_settings *set = &ms->settings;

	if (!set->full_v1 && !set->full_v2 && !set->full_v3) {
		vty_out(vty, "Full-rate codec not supported%s", VTY_NEWLINE);
		return CMD_WARNING;
	}

	set->half_prefer = 0;

	return CMD_SUCCESS;
}

DEFUN(cfg_codec_half, cfg_ms_codec_half_cmd, "codec half-speed",
	"Enable codec\nHalf speed speech codec")
{
	struct osmocom_ms *ms = vty->index;
	struct gsm_settings *set = &ms->settings;

	if (!set->half_v1 && !set->half_v3) {
		vty_out(vty, "Half-rate codec not supported%s", VTY_NEWLINE);
		return CMD_WARNING;
	}

	set->half = 1;

	return CMD_SUCCESS;
}

DEFUN(cfg_codec_half_pref, cfg_ms_codec_half_pref_cmd, "codec half-speed "
	"prefer",
	"Enable codec\nHalf speed speech codec\nPrefer this codec")
{
	struct osmocom_ms *ms = vty->index;
	struct gsm_settings *set = &ms->settings;

	if (!set->half_v1 && !set->half_v3) {
		vty_out(vty, "Half-rate codec not supported%s", VTY_NEWLINE);
		return CMD_WARNING;
	}

	set->half = 1;
	set->half_prefer = 1;

	return CMD_SUCCESS;
}

DEFUN(cfg_no_codec_half, cfg_ms_no_codec_half_cmd, "no codec half-speed",
	NO_STR "Disable codec\nHalf speed speech codec")
{
	struct osmocom_ms *ms = vty->index;
	struct gsm_settings *set = &ms->settings;

	if (!set->half_v1 && !set->half_v3) {
		vty_out(vty, "Half-rate codec not supported%s", VTY_NEWLINE);
		return CMD_WARNING;
	}

	set->half = 0;
	set->half_prefer = 0;

	return CMD_SUCCESS;
}

DEFUN(cfg_abbrev, cfg_ms_abbrev_cmd, "abbrev ABBREVIATION NUMBER [NAME]",
	"Store given abbreviation number\n1-3 digits abbreviation\n"
	"Number to store for the abbreviation "
	"(Use digits '0123456789*#abc', and '+' to dial international)\n"
	"Name of the abbreviation")
{
	struct osmocom_ms *ms = vty->index;
	struct gsm_settings *set = &ms->settings;
	struct gsm_settings_abbrev *abbrev;
	int i;

	llist_for_each_entry(abbrev, &set->abbrev, list) {
		if (!strcmp(argv[0], abbrev->abbrev)) {
			vty_out(vty, "Given abbreviation '%s' already stored, "
				"delete first!%s", argv[0], VTY_NEWLINE);
			return CMD_WARNING;
		}
	}

	if (strlen(argv[0]) >= sizeof(abbrev->abbrev)) {
		vty_out(vty, "Given abbreviation too long%s", VTY_NEWLINE);
		return CMD_WARNING;
	}

	for (i = 0; i < strlen(argv[0]); i++) {
		if (argv[0][i] < '0' || argv[0][i] > '9') {
			vty_out(vty, "Given abbreviation must have digits "
				"0..9 only!%s", VTY_NEWLINE);
			return CMD_WARNING;
		}
	}

	if (vty_check_number(vty, argv[1]))
		return CMD_WARNING;

	abbrev = talloc_zero(ms, struct gsm_settings_abbrev);
	if (!abbrev) {
		vty_out(vty, "No Memory!%s", VTY_NEWLINE);
		return CMD_WARNING;
	}
	llist_add_tail(&abbrev->list, &set->abbrev);
	OSMO_STRLCPY_ARRAY(abbrev->abbrev, argv[0]);
	OSMO_STRLCPY_ARRAY(abbrev->number, argv[1]);
	if (argc >= 3)
		OSMO_STRLCPY_ARRAY(abbrev->name, argv[2]);

	return CMD_SUCCESS;
}

DEFUN(cfg_no_abbrev, cfg_ms_no_abbrev_cmd, "no abbrev [ABBREVIATION]",
	NO_STR "Remove given abbreviation number or all numbers\n"
	"Abbreviation number to remove")
{
	struct osmocom_ms *ms = vty->index;
	struct gsm_settings *set = &ms->settings;
	struct gsm_settings_abbrev *abbrev, *abbrev2;
	uint8_t deleted = 0;

	llist_for_each_entry_safe(abbrev, abbrev2, &set->abbrev, list) {
		if (argc < 1 || !strcmp(argv[0], abbrev->abbrev)) {
			llist_del(&abbrev->list);
			deleted = 1;
		}
	}

	if (argc >= 1 && !deleted) {
		vty_out(vty, "Given abbreviation '%s' not found!%s",
			argv[0], VTY_NEWLINE);
		return CMD_WARNING;
	}

	return CMD_SUCCESS;
}

DEFUN(cfg_ms_neighbour, cfg_ms_neighbour_cmd, "neighbour-measurement",
	"Allow neighbour cell measurement in idle mode")
{
	struct osmocom_ms *ms = vty->index;
	struct gsm_settings *set = &ms->settings;

	set->no_neighbour = 0;

	vty_restart_if_started(vty, ms);


	return CMD_SUCCESS;
}

DEFUN(cfg_ms_no_neighbour, cfg_ms_no_neighbour_cmd, "no neighbour-measurement",
	NO_STR "Do not allow neighbour cell measurement in idle mode")
{
	struct osmocom_ms *ms = vty->index;
	struct gsm_settings *set = &ms->settings;

	set->no_neighbour = 1;

	vty_restart_if_started(vty, ms);

	return CMD_SUCCESS;
}

DEFUN(cfg_ms_any_timeout, cfg_ms_any_timeout_cmd, "c7-any-timeout <0-255>",
	"Seconds to wait in C7 before doing a PLMN search")
{
	struct osmocom_ms *ms = vty->index;
	struct gsm_settings *set = &ms->settings;

	set->any_timeout = atoi(argv[0]);

	vty_restart_if_started(vty, ms);

	return CMD_SUCCESS;
}

static int config_write_dummy(struct vty *vty)
{
	return CMD_SUCCESS;
}

/* per support config */
DEFUN(cfg_ms_support, cfg_ms_support_cmd, "support",
	"Define supported features")
{
	vty->node = SUPPORT_NODE;

	return CMD_SUCCESS;
}

#define SUP_EN(cfg, cfg_cmd, item, cmd, desc, restart) \
DEFUN(cfg, cfg_cmd, cmd, "Enable " desc "support") \
{ \
	struct osmocom_ms *ms = vty->index; \
	struct gsm_settings *set = &ms->settings; \
	struct gsm_support *sup = &ms->support; \
	if (!sup->item) { \
		vty_out(vty, desc " not supported%s", VTY_NEWLINE); \
		if (l23_vty_reading) \
			return CMD_SUCCESS; \
		return CMD_WARNING; \
	} \
	if (restart) \
		vty_restart(vty, ms); \
	set->item = 1; \
	return CMD_SUCCESS; \
}

#define SUP_DI(cfg, cfg_cmd, item, cmd, desc, restart) \
DEFUN(cfg, cfg_cmd, "no " cmd, NO_STR "Disable " desc " support") \
{ \
	struct osmocom_ms *ms = vty->index; \
	struct gsm_settings *set = &ms->settings; \
	struct gsm_support *sup = &ms->support; \
	if (!sup->item) { \
		vty_out(vty, desc " not supported%s", VTY_NEWLINE); \
		if (l23_vty_reading) \
			return CMD_SUCCESS; \
		return CMD_WARNING; \
	} \
	if (restart) \
		vty_restart(vty, ms); \
	set->item = 0; \
	return CMD_SUCCESS; \
}

#define SET_EN(cfg, cfg_cmd, item, cmd, desc, restart) \
DEFUN(cfg, cfg_cmd, cmd, "Enable " desc "support") \
{ \
	struct osmocom_ms *ms = vty->index; \
	struct gsm_settings *set = &ms->settings; \
	if (restart) \
		vty_restart(vty, ms); \
	set->item = 1; \
	return CMD_SUCCESS; \
}

#define SET_DI(cfg, cfg_cmd, item, cmd, desc, restart) \
DEFUN(cfg, cfg_cmd, "no " cmd, NO_STR "Disable " desc " support") \
{ \
	struct osmocom_ms *ms = vty->index; \
	struct gsm_settings *set = &ms->settings; \
	if (restart) \
		vty_restart(vty, ms); \
	set->item = 0; \
	return CMD_SUCCESS; \
}

SET_EN(cfg_ms_sup_dtmf, cfg_ms_sup_dtmf_cmd, cc_dtmf, "dtmf", "DTMF", 0);
SET_DI(cfg_ms_sup_no_dtmf, cfg_ms_sup_no_dtmf_cmd, cc_dtmf, "dtmf", "DTMF", 0);
SUP_EN(cfg_ms_sup_sms, cfg_ms_sup_sms_cmd, sms_ptp, "sms", "SMS", 0);
SUP_DI(cfg_ms_sup_no_sms, cfg_ms_sup_no_sms_cmd, sms_ptp, "sms", "SMS", 0);
SUP_EN(cfg_ms_sup_a5_1, cfg_ms_sup_a5_1_cmd, a5_1, "a5/1", "A5/1", 0);
SUP_DI(cfg_ms_sup_no_a5_1, cfg_ms_sup_no_a5_1_cmd, a5_1, "a5/1", "A5/1", 0);
SUP_EN(cfg_ms_sup_a5_2, cfg_ms_sup_a5_2_cmd, a5_2, "a5/2", "A5/2", 0);
SUP_DI(cfg_ms_sup_no_a5_2, cfg_ms_sup_no_a5_2_cmd, a5_2, "a5/2", "A5/2", 0);
SUP_EN(cfg_ms_sup_a5_3, cfg_ms_sup_a5_3_cmd, a5_3, "a5/3", "A5/3", 0);
SUP_DI(cfg_ms_sup_no_a5_3, cfg_ms_sup_no_a5_3_cmd, a5_3, "a5/3", "A5/3", 0);
SUP_EN(cfg_ms_sup_a5_4, cfg_ms_sup_a5_4_cmd, a5_4, "a5/4", "A5/4", 0);
SUP_DI(cfg_ms_sup_no_a5_4, cfg_ms_sup_no_a5_4_cmd, a5_4, "a5/4", "A5/4", 0);
SUP_EN(cfg_ms_sup_a5_5, cfg_ms_sup_a5_5_cmd, a5_5, "a5/5", "A5/5", 0);
SUP_DI(cfg_ms_sup_no_a5_5, cfg_ms_sup_no_a5_5_cmd, a5_5, "a5/5", "A5/5", 0);
SUP_EN(cfg_ms_sup_a5_6, cfg_ms_sup_a5_6_cmd, a5_6, "a5/6", "A5/6", 0);
SUP_DI(cfg_ms_sup_no_a5_6, cfg_ms_sup_no_a5_6_cmd, a5_6, "a5/6", "A5/6", 0);
SUP_EN(cfg_ms_sup_a5_7, cfg_ms_sup_a5_7_cmd, a5_7, "a5/7", "A5/7", 0);
SUP_DI(cfg_ms_sup_no_a5_7, cfg_ms_sup_no_a5_7_cmd, a5_7, "a5/7", "A5/7", 0);
SUP_EN(cfg_ms_sup_p_gsm, cfg_ms_sup_p_gsm_cmd, p_gsm, "p-gsm", "P-GSM (900)",
	1);
SUP_DI(cfg_ms_sup_no_p_gsm, cfg_ms_sup_no_p_gsm_cmd, p_gsm, "p-gsm",
	"P-GSM (900)", 1);
SUP_EN(cfg_ms_sup_e_gsm, cfg_ms_sup_e_gsm_cmd, e_gsm, "e-gsm", "E-GSM (850)",
	1);
SUP_DI(cfg_ms_sup_no_e_gsm, cfg_ms_sup_no_e_gsm_cmd, e_gsm, "e-gsm",
	"E-GSM (850)", 1);
SUP_EN(cfg_ms_sup_r_gsm, cfg_ms_sup_r_gsm_cmd, r_gsm, "r-gsm", "R-GSM (850)",
	1);
SUP_DI(cfg_ms_sup_no_r_gsm, cfg_ms_sup_no_r_gsm_cmd, r_gsm, "r-gsm",
	"R-GSM (850)", 1);
SUP_EN(cfg_ms_sup_dcs, cfg_ms_sup_dcs_cmd, dcs, "dcs", "DCS (1800)", 1);
SUP_DI(cfg_ms_sup_no_dcs, cfg_ms_sup_no_dcs_cmd, dcs, "dcs", "DCS (1800)", 1);
SUP_EN(cfg_ms_sup_gsm_850, cfg_ms_sup_gsm_850_cmd, gsm_850, "gsm-850",
	"GSM 850", 1);
SUP_DI(cfg_ms_sup_no_gsm_850, cfg_ms_sup_no_gsm_850_cmd, gsm_850, "gsm-850",
	"GSM 850", 1);
SUP_EN(cfg_ms_sup_pcs, cfg_ms_sup_pcs_cmd, pcs, "pcs", "PCS (1900)", 1);
SUP_DI(cfg_ms_sup_no_pcs, cfg_ms_sup_no_pcs_cmd, pcs, "pcs", "PCS (1900)", 1);
SUP_EN(cfg_ms_sup_gsm_480, cfg_ms_sup_gsm_480_cmd, gsm_480, "gsm-480",
	"GSM 480", 1);
SUP_DI(cfg_ms_sup_no_gsm_480, cfg_ms_sup_no_gsm_480_cmd, gsm_480, "gsm-480",
	"GSM 480", 1);
SUP_EN(cfg_ms_sup_gsm_450, cfg_ms_sup_gsm_450_cmd, gsm_450, "gsm-450",
	"GSM 450", 1);
SUP_DI(cfg_ms_sup_no_gsm_450, cfg_ms_sup_no_gsm_450_cmd, gsm_450, "gsm-450",
	"GSM 450", 1);

DEFUN(cfg_ms_sup_class_900, cfg_ms_sup_class_900_cmd, "class-900 (1|2|3|4|5)",
	"Select power class for GSM 900\n"
	"20 Watts\n"
	"8 Watts\n"
	"5 Watts\n"
	"2 Watts\n"
	"0.8 Watts")
{
	struct osmocom_ms *ms = vty->index;
	struct gsm_settings *set = &ms->settings;
	struct gsm_support *sup = &ms->support;

	set->class_900 = atoi(argv[0]);

	if (set->class_900 < sup->class_900 && !l23_vty_reading)
		vty_out(vty, "Note: You selected a higher class than supported "
			" by hardware!%s", VTY_NEWLINE);

	return CMD_SUCCESS;
}

DEFUN(cfg_ms_sup_class_850, cfg_ms_sup_class_850_cmd, "class-850 (1|2|3|4|5)",
	"Select power class for GSM 850\n"
	"20 Watts\n"
	"8 Watts\n"
	"5 Watts\n"
	"2 Watts\n"
	"0.8 Watts")
{
	struct osmocom_ms *ms = vty->index;
	struct gsm_settings *set = &ms->settings;
	struct gsm_support *sup = &ms->support;

	set->class_850 = atoi(argv[0]);

	if (set->class_850 < sup->class_850 && !l23_vty_reading)
		vty_out(vty, "Note: You selected a higher class than supported "
			" by hardware!%s", VTY_NEWLINE);

	return CMD_SUCCESS;
}

DEFUN(cfg_ms_sup_class_400, cfg_ms_sup_class_400_cmd, "class-400 (1|2|3|4|5)",
	"Select power class for GSM 400 (480 and 450)\n"
	"20 Watts\n"
	"8 Watts\n"
	"5 Watts\n"
	"2 Watts\n"
	"0.8 Watts")
{
	struct osmocom_ms *ms = vty->index;
	struct gsm_settings *set = &ms->settings;
	struct gsm_support *sup = &ms->support;

	set->class_400 = atoi(argv[0]);

	if (set->class_400 < sup->class_400 && !l23_vty_reading)
		vty_out(vty, "Note: You selected a higher class than supported "
			" by hardware!%s", VTY_NEWLINE);

	return CMD_SUCCESS;
}

DEFUN(cfg_ms_sup_class_dcs, cfg_ms_sup_class_dcs_cmd, "class-dcs (1|2|3)",
	"Select power class for DCS 1800\n"
	"1 Watt\n"
	"0.25 Watts\n"
	"4 Watts")
{
	struct osmocom_ms *ms = vty->index;
	struct gsm_settings *set = &ms->settings;
	struct gsm_support *sup = &ms->support;

	set->class_dcs = atoi(argv[0]);

	if (((set->class_dcs + 1) & 3) < ((sup->class_dcs + 1) & 3)
	 && !l23_vty_reading)
		vty_out(vty, "Note: You selected a higher class than supported "
			" by hardware!%s", VTY_NEWLINE);

	return CMD_SUCCESS;
}

DEFUN(cfg_ms_sup_class_pcs, cfg_ms_sup_class_pcs_cmd, "class-pcs (1|2|3)",
	"Select power class for PCS 1900\n"
	"1 Watt\n"
	"0.25 Watts\n"
	"2 Watts")
{
	struct osmocom_ms *ms = vty->index;
	struct gsm_settings *set = &ms->settings;
	struct gsm_support *sup = &ms->support;

	set->class_pcs = atoi(argv[0]);

	if (((set->class_pcs + 1) & 3) < ((sup->class_pcs + 1) & 3)
	 && !l23_vty_reading)
		vty_out(vty, "Note: You selected a higher class than supported "
			" by hardware!%s", VTY_NEWLINE);

	return CMD_SUCCESS;
}

DEFUN(cfg_ms_sup_ch_cap, cfg_ms_sup_ch_cap_cmd,
	"channel-capability (sdcch|sdcch+tchf|sdcch+tchf+tchh)",
	"Select channel capability\n"
	"SDCCH only\n"
	"SDCCH + TCH/F\n"
	"SDCCH + TCH/F + TCH/H")
{
	struct osmocom_ms *ms = vty->index;
	struct gsm_settings *set = &ms->settings;
	struct gsm_support *sup = &ms->support;
	uint8_t ch_cap;

	if (!strcmp(argv[0], "sdcch+tchf+tchh"))
		ch_cap = GSM_CAP_SDCCH_TCHF_TCHH;
	else if (!strcmp(argv[0], "sdcch+tchf"))
		ch_cap = GSM_CAP_SDCCH_TCHF;
	else
		ch_cap = GSM_CAP_SDCCH;

	if (ch_cap > sup->ch_cap && !l23_vty_reading) {
		vty_out(vty, "You selected an higher capability than supported "
			" by hardware!%s", VTY_NEWLINE);
		return CMD_WARNING;
	}

	if (ms->started && ch_cap != set->ch_cap
	 && (ch_cap == GSM_CAP_SDCCH || set->ch_cap == GSM_CAP_SDCCH))
		vty_restart_if_started(vty, ms);

	set->ch_cap = ch_cap;

	return CMD_SUCCESS;
}

SUP_EN(cfg_ms_sup_full_v1, cfg_ms_sup_full_v1_cmd, full_v1, "full-speech-v1",
	"Full rate speech V1", 0);
SUP_DI(cfg_ms_sup_no_full_v1, cfg_ms_sup_no_full_v1_cmd, full_v1,
	"full-speech-v1", "Full rate speech V1", 0);
SUP_EN(cfg_ms_sup_full_v2, cfg_ms_sup_full_v2_cmd, full_v2, "full-speech-v2",
	"Full rate speech V2 (EFR)", 0);
SUP_DI(cfg_ms_sup_no_full_v2, cfg_ms_sup_no_full_v2_cmd, full_v2,
	"full-speech-v2", "Full rate speech V2 (EFR)", 0);
SUP_EN(cfg_ms_sup_full_v3, cfg_ms_sup_full_v3_cmd, full_v3, "full-speech-v3",
	"Full rate speech V3 (AMR)", 0);
SUP_DI(cfg_ms_sup_no_full_v3, cfg_ms_sup_no_full_v3_cmd, full_v3,
	"full-speech-v3", "Full rate speech V3 (AMR)", 0);
SUP_EN(cfg_ms_sup_half_v1, cfg_ms_sup_half_v1_cmd, half_v1, "half-speech-v1",
	"Half rate speech V1", 0);
SUP_DI(cfg_ms_sup_no_half_v1, cfg_ms_sup_no_half_v1_cmd, half_v1,
	"half-speech-v1", "Half rate speech V1", 0);
SUP_EN(cfg_ms_sup_half_v3, cfg_ms_sup_half_v3_cmd, half_v3, "half-speech-v3",
	"Half rate speech V3 (AMR)", 0);
SUP_DI(cfg_ms_sup_no_half_v3, cfg_ms_sup_no_half_v3_cmd, half_v3,
	"half-speech-v3", "Half rate speech V3 (AMR)", 0);

DEFUN(cfg_ms_sup_min_rxlev, cfg_ms_sup_min_rxlev_cmd, "min-rxlev <-110--47>",
	"Set the minimum receive level to select a cell\n"
	"Minimum receive level from -110 dBm to -47 dBm")
{
	struct osmocom_ms *ms = vty->index;
	struct gsm_settings *set = &ms->settings;

	set->min_rxlev_dbm = atoi(argv[0]);

	return CMD_SUCCESS;
}

DEFUN(cfg_ms_sup_dsc_max, cfg_ms_sup_dsc_max_cmd, "dsc-max <90-500>",
	"Set the maximum DSC value. Standard is 90. Increase to make mobile "
	"more reliable against bad RX signal. This increase the propability "
	"of missing a paging requests\n"
	"DSC initial and maximum value (standard is 90)")
{
	struct osmocom_ms *ms = vty->index;
	struct gsm_settings *set = &ms->settings;

	set->dsc_max = atoi(argv[0]);

	return CMD_SUCCESS;
}

DEFUN(cfg_ms_sup_skip_max_per_band, cfg_ms_sup_skip_max_per_band_cmd,
	"skip-max-per-band",
	"Scan all frequencies per band, not only a maximum number")
{
	struct osmocom_ms *ms = vty->index;
	struct gsm_settings *set = &ms->settings;

	set->skip_max_per_band = 1;

	return CMD_SUCCESS;
}

DEFUN(cfg_ms_sup_no_skip_max_per_band, cfg_ms_sup_no_skip_max_per_band_cmd,
	"no skip-max-per-band",
	NO_STR "Scan only a maximum number of frequencies per band")
{
	struct osmocom_ms *ms = vty->index;
	struct gsm_settings *set = &ms->settings;

	set->skip_max_per_band = 0;

	return CMD_SUCCESS;
}

/* per audio config */
DEFUN(cfg_ms_audio, cfg_ms_audio_cmd, "audio",
	"Configure audio settings")
{
	vty->node = AUDIO_NODE;
	return CMD_SUCCESS;
}

static int set_audio_io_handler(struct vty *vty, enum audio_io_handler val)
{
	struct osmocom_ms *ms = (struct osmocom_ms *) vty->index;
	struct gsm_settings *set = &ms->settings;

	/* Don't restart on unchanged value */
	if (val == set->audio.io_handler)
		return CMD_SUCCESS;
	set->audio.io_handler = val;

	/* Restart required */
	vty_restart_if_started(vty, ms);

	return CMD_SUCCESS;
}

DEFUN(cfg_ms_audio_io_handler, cfg_ms_audio_io_handler_cmd,
	"io-handler (none|gapk|l1phy|mncc-sock|loopback)",
	"Set TCH frame I/O handler\n"
	"No handler, drop TCH frames (default)\n"
	"libosmo-gapk based I/O handler (requires ALSA)\n"
	"L1 PHY (e.g. Calypso DSP in Motorola C1xx phones)\n"
	"External MNCC application (e.g. LCR) via MNCC socket\n"
	"Return TCH frame payload back to sender\n")
{
	struct osmocom_ms *ms = (struct osmocom_ms *) vty->index;
	int val = get_string_value(audio_io_handler_names, argv[0]);

	if (val == AUDIO_IOH_MNCC_SOCK) {
		if (ms->settings.mncc_handler != MNCC_HANDLER_INTERNAL) {
			vty_out(vty, "Audio I/O handler 'mncc-sock' can only be used "
				"with MNCC handler 'external'%s", VTY_NEWLINE);
			return CMD_WARNING;
		}
	}

#ifndef WITH_GAPK_IO
	if (val == AUDIO_IOH_GAPK) {
		vty_out(vty, "GAPK I/O is not compiled in (--with-gapk-io)%s", VTY_NEWLINE);
		return CMD_WARNING;
	}
#endif

	return set_audio_io_handler(vty, val);
}

DEFUN(cfg_ms_audio_no_io_handler, cfg_ms_audio_no_io_handler_cmd,
	"no io-handler", NO_STR "Disable TCH frame processing")
{
	return set_audio_io_handler(vty, AUDIO_IOH_NONE);
}

DEFUN(cfg_ms_audio_io_tch_format, cfg_ms_audio_io_tch_format_cmd,
	"io-tch-format (rtp|ti)",
	"Set TCH I/O frame format used by the L1 PHY (for GAPK only)\n"
	"RTP format (RFC3551 for FR/EFR, RFC5993 for HR, RFC4867 for AMR)\n"
	"Texas Instruments format, used by Calypso based phones (e.g. Motorola C1xx)\n")
{
	int val = get_string_value(audio_io_format_names, argv[0]);
	struct osmocom_ms *ms = (struct osmocom_ms *) vty->index;
	struct gsm_settings *set = &ms->settings;

	if (set->audio.io_handler != AUDIO_IOH_GAPK) {
		vty_out(vty, "This parameter is only valid for GAPK%s", VTY_NEWLINE);
		return CMD_WARNING;
	}

	OSMO_ASSERT(val >= 0);
	set->audio.io_format = val;

	return CMD_SUCCESS;
}

DEFUN(cfg_ms_audio_alsa_out_dev, cfg_ms_audio_alsa_out_dev_cmd,
	"alsa-output-dev (default|NAME)",
	"Set ALSA output (playback) device name (for GAPK only)\n"
	"Default system playback device (default)\n"
	"Name of a custom playback device")
{
	struct osmocom_ms *ms = vty->index;
	struct gsm_settings *set = &ms->settings;

	OSMO_STRLCPY_ARRAY(set->audio.alsa_output_dev, argv[0]);

	return CMD_SUCCESS;
}

DEFUN(cfg_ms_audio_alsa_in_dev, cfg_ms_audio_alsa_in_dev_cmd,
	"alsa-input-dev (default|NAME)",
	"Set ALSA input (capture) device name (for GAPK only)\n"
	"Default system recording device (default)\n"
	"Name of a custom recording device")
{
	struct osmocom_ms *ms = vty->index;
	struct gsm_settings *set = &ms->settings;

	OSMO_STRLCPY_ARRAY(set->audio.alsa_input_dev, argv[0]);

	return CMD_SUCCESS;
}

DEFUN(cfg_ms_script_load_run, cfg_ms_script_load_run_cmd, "lua-script FILENAME",
	"Load and execute a LUA script\nFilename for lua script")
{
	struct osmocom_ms *ms = vty->index;

	osmo_talloc_replace_string(ms, &ms->lua_script, argv[0]);
	if (!ms->lua_script)
		return CMD_WARNING;

	script_lua_load(vty, ms, ms->lua_script);
	return CMD_SUCCESS;
}

DEFUN(cfg_ms_no_script_load_run, cfg_ms_no_script_load_run_cmd, "no lua-script",
	NO_STR "Load and execute LUA script")
{
	struct osmocom_ms *ms = vty->index;

	script_lua_close(ms);
	talloc_free(ms->lua_script);
	ms->lua_script = NULL;
	return CMD_SUCCESS;
}

DEFUN(off, off_cmd, "off",
	"Turn mobiles off (shutdown) and exit")
{
	osmo_signal_dispatch(SS_GLOBAL, S_GLOBAL_SHUTDOWN, NULL);

	return CMD_SUCCESS;
}

/* run ms instance, if layer1 is available */
static int l23_vty_signal_cb(unsigned int subsys, unsigned int signal,
		     void *handler_data, void *signal_data)
{
	struct osmobb_l23_vty_sig_data *d = signal_data;
	struct vty *vty = d->vty;
	char *other_name = NULL;
	int rc;

	if (subsys != SS_L23_VTY)
		return 0;

	switch (signal) {
	case S_L23_VTY_MS_START:
		rc = mobile_start(d->ms_start.ms, &other_name);
		switch (rc) {
		case -1:
			vty_out(vty, "Cannot start MS '%s', because MS '%s' "
				"use the same layer2-socket.%sPlease shutdown "
				"MS '%s' first.%s", d->ms_start.ms->name, other_name,
				VTY_NEWLINE, other_name, VTY_NEWLINE);
			break;
		case -2:
			vty_out(vty, "Cannot start MS '%s', because MS '%s' "
				"use the same sap-socket.%sPlease shutdown "
				"MS '%s' first.%s", d->ms_start.ms->name, other_name,
				VTY_NEWLINE, other_name, VTY_NEWLINE);
			break;
		case -3:
			vty_out(vty, "Connection to layer 1 failed!%s",
				VTY_NEWLINE);
			break;
		}
		d->ms_start.rc = (rc == 0) ? CMD_SUCCESS : CMD_WARNING;
		break;
	case S_L23_VTY_MS_STOP:
		mobile_stop(d->ms_stop.ms, d->ms_stop.force);
		d->ms_start.rc = CMD_SUCCESS;
		break;
	}
	return 0;
}


#define SUP_NODE(item) \
	install_element(SUPPORT_NODE, &cfg_ms_sup_item_cmd);

int ms_vty_init(void)
{
	int rc;

	if ((rc = l23_vty_init(config_write, l23_vty_signal_cb)) < 0)
		return rc;

	install_element_ve(&show_ms_cmd);
	install_element_ve(&show_cell_cmd);
	install_element_ve(&show_cell_si_cmd);
	install_element_ve(&show_nbcells_cmd);
	install_element_ve(&show_ba_cmd);
	install_element_ve(&show_forb_la_cmd);
	install_element_ve(&show_forb_plmn_cmd);
	install_element_ve(&monitor_network_cmd);
	install_element_ve(&no_monitor_network_cmd);
	install_element(ENABLE_NODE, &off_cmd);

	install_element(ENABLE_NODE, &network_search_cmd);
	install_element(ENABLE_NODE, &network_show_cmd);
	install_element(ENABLE_NODE, &network_select_cmd);
	install_element(ENABLE_NODE, &call_cmd);
	install_element(ENABLE_NODE, &call_retr_cmd);
	install_element(ENABLE_NODE, &call_dtmf_cmd);
	install_element(ENABLE_NODE, &sms_cmd);
	install_element(ENABLE_NODE, &service_cmd);
	install_element(ENABLE_NODE, &test_reselection_cmd);
	install_element(ENABLE_NODE, &delete_forbidden_plmn_cmd);

#ifdef _HAVE_GPSD
	install_element(CONFIG_NODE, &cfg_gps_host_cmd);
#endif
	install_element(CONFIG_NODE, &cfg_gps_device_cmd);
	install_element(CONFIG_NODE, &cfg_gps_baud_cmd);
	install_element(CONFIG_NODE, &cfg_gps_enable_cmd);
	install_element(CONFIG_NODE, &cfg_no_gps_enable_cmd);

	install_element(CONFIG_NODE, &cfg_ms_cmd);
	install_element(CONFIG_NODE, &cfg_ms_create_cmd);
	install_element(CONFIG_NODE, &cfg_ms_rename_cmd);
	install_element(CONFIG_NODE, &cfg_no_ms_cmd);

	/* MS_NODE is installed by l23_vty_init(). App specific commands below: */
	install_element(MS_NODE, &cfg_ms_show_this_cmd);
	install_element(MS_NODE, &cfg_ms_sap_cmd);
	install_element(MS_NODE, &cfg_ms_mncc_sock_cmd);
	install_element(MS_NODE, &cfg_ms_mncc_handler_cmd);
	install_element(MS_NODE, &cfg_ms_no_mncc_handler_cmd);
	install_element(MS_NODE, &cfg_ms_mode_cmd);
	install_element(MS_NODE, &cfg_ms_no_emerg_imsi_cmd);
	install_element(MS_NODE, &cfg_ms_emerg_imsi_cmd);
	install_element(MS_NODE, &cfg_ms_no_sms_sca_cmd);
	install_element(MS_NODE, &cfg_ms_sms_sca_cmd);
	install_element(MS_NODE, &cfg_ms_cw_cmd);
	install_element(MS_NODE, &cfg_ms_no_cw_cmd);
	install_element(MS_NODE, &cfg_ms_auto_answer_cmd);
	install_element(MS_NODE, &cfg_ms_no_auto_answer_cmd);
	install_element(MS_NODE, &cfg_ms_force_rekey_cmd);
	install_element(MS_NODE, &cfg_ms_no_force_rekey_cmd);
	install_element(MS_NODE, &cfg_ms_clip_cmd);
	install_element(MS_NODE, &cfg_ms_clir_cmd);
	install_element(MS_NODE, &cfg_ms_no_clip_cmd);
	install_element(MS_NODE, &cfg_ms_no_clir_cmd);
	install_element(MS_NODE, &cfg_ms_tx_power_cmd);
	install_element(MS_NODE, &cfg_ms_tx_power_val_cmd);
	install_element(MS_NODE, &cfg_ms_sim_delay_cmd);
	install_element(MS_NODE, &cfg_ms_no_sim_delay_cmd);
	install_element(MS_NODE, &cfg_ms_stick_cmd);
	install_element(MS_NODE, &cfg_ms_no_stick_cmd);
	install_element(MS_NODE, &cfg_ms_lupd_cmd);
	install_element(MS_NODE, &cfg_ms_no_lupd_cmd);
	install_element(MS_NODE, &cfg_ms_codec_full_cmd);
	install_element(MS_NODE, &cfg_ms_codec_full_pref_cmd);
	install_element(MS_NODE, &cfg_ms_codec_half_cmd);
	install_element(MS_NODE, &cfg_ms_codec_half_pref_cmd);
	install_element(MS_NODE, &cfg_ms_no_codec_half_cmd);
	install_element(MS_NODE, &cfg_ms_abbrev_cmd);
	install_element(MS_NODE, &cfg_ms_no_abbrev_cmd);
	install_element(MS_NODE, &cfg_ms_audio_cmd);
	install_element(MS_NODE, &cfg_ms_neighbour_cmd);
	install_element(MS_NODE, &cfg_ms_no_neighbour_cmd);
	install_element(MS_NODE, &cfg_ms_any_timeout_cmd);
	install_element(MS_NODE, &cfg_ms_sms_store_cmd);
	install_element(MS_NODE, &cfg_ms_no_sms_store_cmd);
	install_element(MS_NODE, &cfg_ms_support_cmd);
	install_node(&support_node, config_write_dummy);
	install_element(SUPPORT_NODE, &cfg_ms_sup_dtmf_cmd);
	install_element(SUPPORT_NODE, &cfg_ms_sup_no_dtmf_cmd);
	install_element(SUPPORT_NODE, &cfg_ms_sup_sms_cmd);
	install_element(SUPPORT_NODE, &cfg_ms_sup_no_sms_cmd);
	install_element(SUPPORT_NODE, &cfg_ms_sup_a5_1_cmd);
	install_element(SUPPORT_NODE, &cfg_ms_sup_no_a5_1_cmd);
	install_element(SUPPORT_NODE, &cfg_ms_sup_a5_2_cmd);
	install_element(SUPPORT_NODE, &cfg_ms_sup_no_a5_2_cmd);
	install_element(SUPPORT_NODE, &cfg_ms_sup_a5_3_cmd);
	install_element(SUPPORT_NODE, &cfg_ms_sup_no_a5_3_cmd);
	install_element(SUPPORT_NODE, &cfg_ms_sup_a5_4_cmd);
	install_element(SUPPORT_NODE, &cfg_ms_sup_no_a5_4_cmd);
	install_element(SUPPORT_NODE, &cfg_ms_sup_a5_5_cmd);
	install_element(SUPPORT_NODE, &cfg_ms_sup_no_a5_5_cmd);
	install_element(SUPPORT_NODE, &cfg_ms_sup_a5_6_cmd);
	install_element(SUPPORT_NODE, &cfg_ms_sup_no_a5_6_cmd);
	install_element(SUPPORT_NODE, &cfg_ms_sup_a5_7_cmd);
	install_element(SUPPORT_NODE, &cfg_ms_sup_no_a5_7_cmd);
	install_element(SUPPORT_NODE, &cfg_ms_sup_p_gsm_cmd);
	install_element(SUPPORT_NODE, &cfg_ms_sup_no_p_gsm_cmd);
	install_element(SUPPORT_NODE, &cfg_ms_sup_e_gsm_cmd);
	install_element(SUPPORT_NODE, &cfg_ms_sup_no_e_gsm_cmd);
	install_element(SUPPORT_NODE, &cfg_ms_sup_r_gsm_cmd);
	install_element(SUPPORT_NODE, &cfg_ms_sup_no_r_gsm_cmd);
	install_element(SUPPORT_NODE, &cfg_ms_sup_dcs_cmd);
	install_element(SUPPORT_NODE, &cfg_ms_sup_no_dcs_cmd);
	install_element(SUPPORT_NODE, &cfg_ms_sup_gsm_850_cmd);
	install_element(SUPPORT_NODE, &cfg_ms_sup_no_gsm_850_cmd);
	install_element(SUPPORT_NODE, &cfg_ms_sup_pcs_cmd);
	install_element(SUPPORT_NODE, &cfg_ms_sup_no_pcs_cmd);
	install_element(SUPPORT_NODE, &cfg_ms_sup_gsm_480_cmd);
	install_element(SUPPORT_NODE, &cfg_ms_sup_no_gsm_480_cmd);
	install_element(SUPPORT_NODE, &cfg_ms_sup_gsm_450_cmd);
	install_element(SUPPORT_NODE, &cfg_ms_sup_no_gsm_450_cmd);
	install_element(SUPPORT_NODE, &cfg_ms_sup_class_900_cmd);
	install_element(SUPPORT_NODE, &cfg_ms_sup_class_dcs_cmd);
	install_element(SUPPORT_NODE, &cfg_ms_sup_class_850_cmd);
	install_element(SUPPORT_NODE, &cfg_ms_sup_class_pcs_cmd);
	install_element(SUPPORT_NODE, &cfg_ms_sup_class_400_cmd);
	install_element(SUPPORT_NODE, &cfg_ms_sup_ch_cap_cmd);
	install_element(SUPPORT_NODE, &cfg_ms_sup_full_v1_cmd);
	install_element(SUPPORT_NODE, &cfg_ms_sup_no_full_v1_cmd);
	install_element(SUPPORT_NODE, &cfg_ms_sup_full_v2_cmd);
	install_element(SUPPORT_NODE, &cfg_ms_sup_no_full_v2_cmd);
	install_element(SUPPORT_NODE, &cfg_ms_sup_full_v3_cmd);
	install_element(SUPPORT_NODE, &cfg_ms_sup_no_full_v3_cmd);
	install_element(SUPPORT_NODE, &cfg_ms_sup_half_v1_cmd);
	install_element(SUPPORT_NODE, &cfg_ms_sup_no_half_v1_cmd);
	install_element(SUPPORT_NODE, &cfg_ms_sup_half_v3_cmd);
	install_element(SUPPORT_NODE, &cfg_ms_sup_no_half_v3_cmd);
	install_element(SUPPORT_NODE, &cfg_ms_sup_min_rxlev_cmd);
	install_element(SUPPORT_NODE, &cfg_ms_sup_dsc_max_cmd);
	install_element(SUPPORT_NODE, &cfg_ms_sup_skip_max_per_band_cmd);
	install_element(SUPPORT_NODE, &cfg_ms_sup_no_skip_max_per_band_cmd);
	install_element(MS_NODE, &cfg_ms_script_load_run_cmd);
	install_element(MS_NODE, &cfg_ms_no_script_load_run_cmd);

	install_node(&audio_node, config_write_dummy);
	install_element(AUDIO_NODE, &cfg_ms_audio_io_handler_cmd);
	install_element(AUDIO_NODE, &cfg_ms_audio_no_io_handler_cmd);
	install_element(AUDIO_NODE, &cfg_ms_audio_io_tch_format_cmd);
	install_element(AUDIO_NODE, &cfg_ms_audio_alsa_out_dev_cmd);
	install_element(AUDIO_NODE, &cfg_ms_audio_alsa_in_dev_cmd);

	return 0;
}

