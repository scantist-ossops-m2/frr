/*
 * PIM for Quagga
 * Copyright (C) 2008  Everton da Silva Marques
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; see the file COPYING; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include <zebra.h>

#include "lib/json.h"
#include "command.h"
#include "if.h"
#include "prefix.h"
#include "zclient.h"
#include "plist.h"
#include "hash.h"
#include "nexthop.h"
#include "vrf.h"
#include "ferr.h"

#include "pimd.h"
#include "pim_mroute.h"
#include "pim_cmd.h"
#include "pim_iface.h"
#include "pim_vty.h"
#include "pim_mroute.h"
#include "pim_str.h"
#include "pim_igmp.h"
#include "pim_igmpv3.h"
#include "pim_sock.h"
#include "pim_time.h"
#include "pim_util.h"
#include "pim_oil.h"
#include "pim_neighbor.h"
#include "pim_pim.h"
#include "pim_ifchannel.h"
#include "pim_hello.h"
#include "pim_msg.h"
#include "pim_upstream.h"
#include "pim_rpf.h"
#include "pim_macro.h"
#include "pim_ssmpingd.h"
#include "pim_zebra.h"
#include "pim_static.h"
#include "pim_rp.h"
#include "pim_zlookup.h"
#include "pim_msdp.h"
#include "pim_ssm.h"
#include "pim_nht.h"
#include "pim_bfd.h"
#include "pim_vxlan.h"
#include "pim_mlag.h"
#include "bfd.h"
#include "pim_bsm.h"
#include "lib/northbound_cli.h"
#include "pim_errors.h"
#include "pim_nb.h"

#ifndef VTYSH_EXTRACT_PL
#include "pimd/pim_cmd_clippy.c"
#endif

static struct cmd_node debug_node = {
	.name = "debug",
	.node = DEBUG_NODE,
	.prompt = "",
	.config_write = pim_debug_config_write,
};

static inline bool pim_sgaddr_match(pim_sgaddr item, pim_sgaddr match)
{
	return (pim_addr_is_any(match.grp) ||
		!pim_addr_cmp(match.grp, item.grp)) &&
	       (pim_addr_is_any(match.src) ||
		!pim_addr_cmp(match.src, item.src));
}

static struct vrf *pim_cmd_lookup_vrf(struct vty *vty, struct cmd_token *argv[],
				      const int argc, int *idx)
{
	struct vrf *vrf;

	if (argv_find(argv, argc, "NAME", idx))
		vrf = vrf_lookup_by_name(argv[*idx]->arg);
	else
		vrf = vrf_lookup_by_id(VRF_DEFAULT);

	if (!vrf)
		vty_out(vty, "Specified VRF: %s does not exist\n",
			argv[*idx]->arg);

	return vrf;
}

static void pim_show_assert_helper(struct vty *vty,
				   struct pim_interface *pim_ifp,
				   struct pim_ifchannel *ch, time_t now)
{
	char winner_str[INET_ADDRSTRLEN];
	struct in_addr ifaddr;
	char uptime[10];
	char timer[10];
	char buf[PREFIX_STRLEN];

	ifaddr = pim_ifp->primary_address;

	pim_inet4_dump("<assrt_win?>", ch->ifassert_winner, winner_str,
		       sizeof(winner_str));

	pim_time_uptime(uptime, sizeof(uptime), now - ch->ifassert_creation);
	pim_time_timer_to_mmss(timer, sizeof(timer), ch->t_ifassert_timer);

	vty_out(vty, "%-16s %-15s %-15pPAs %-15pPAs %-6s %-15s %-8s %-5s\n",
		ch->interface->name,
		inet_ntop(AF_INET, &ifaddr, buf, sizeof(buf)), &ch->sg.src,
		&ch->sg.grp, pim_ifchannel_ifassert_name(ch->ifassert_state),
		winner_str, uptime, timer);
}

static void pim_show_assert(struct pim_instance *pim, struct vty *vty)
{
	struct pim_interface *pim_ifp;
	struct pim_ifchannel *ch;
	struct interface *ifp;
	time_t now;

	now = pim_time_monotonic_sec();

	vty_out(vty,
		"Interface        Address         Source          Group           State  Winner          Uptime   Timer\n");

	FOR_ALL_INTERFACES (pim->vrf, ifp) {
		pim_ifp = ifp->info;
		if (!pim_ifp)
			continue;

		RB_FOREACH (ch, pim_ifchannel_rb, &pim_ifp->ifchannel_rb) {
			if (ch->ifassert_state == PIM_IFASSERT_NOINFO)
				continue;

			pim_show_assert_helper(vty, pim_ifp, ch, now);
		} /* scan interface channels */
	}
}

static void pim_show_assert_internal_helper(struct vty *vty,
					    struct pim_interface *pim_ifp,
					    struct pim_ifchannel *ch)
{
	struct in_addr ifaddr;
	char buf[PREFIX_STRLEN];

	ifaddr = pim_ifp->primary_address;

	vty_out(vty, "%-16s %-15s %-15pPAs %-15pPAs %-3s %-3s %-3s %-4s\n",
		ch->interface->name,
		inet_ntop(AF_INET, &ifaddr, buf, sizeof(buf)), &ch->sg.src,
		&ch->sg.grp,
		PIM_IF_FLAG_TEST_COULD_ASSERT(ch->flags) ? "yes" : "no",
		pim_macro_ch_could_assert_eval(ch) ? "yes" : "no",
		PIM_IF_FLAG_TEST_ASSERT_TRACKING_DESIRED(ch->flags) ? "yes"
		: "no",
		pim_macro_assert_tracking_desired_eval(ch) ? "yes" : "no");
}

static void pim_show_assert_internal(struct pim_instance *pim, struct vty *vty)
{
	struct pim_interface *pim_ifp;
	struct pim_ifchannel *ch;
	struct interface *ifp;

	vty_out(vty,
		"CA:   CouldAssert\n"
		"ECA:  Evaluate CouldAssert\n"
		"ATD:  AssertTrackingDesired\n"
		"eATD: Evaluate AssertTrackingDesired\n\n");

	vty_out(vty,
		"Interface        Address         Source          Group           CA  eCA ATD eATD\n");
	FOR_ALL_INTERFACES (pim->vrf, ifp) {
		pim_ifp = ifp->info;
		if (!pim_ifp)
			continue;

		RB_FOREACH (ch, pim_ifchannel_rb, &pim_ifp->ifchannel_rb) {
			pim_show_assert_internal_helper(vty, pim_ifp, ch);
		} /* scan interface channels */
	}
}

static void pim_show_assert_metric_helper(struct vty *vty,
					  struct pim_interface *pim_ifp,
					  struct pim_ifchannel *ch)
{
	char addr_str[INET_ADDRSTRLEN];
	struct pim_assert_metric am;
	struct in_addr ifaddr;
	char buf[PREFIX_STRLEN];

	ifaddr = pim_ifp->primary_address;

	am = pim_macro_spt_assert_metric(&ch->upstream->rpf,
					 pim_ifp->primary_address);

	pim_inet4_dump("<addr?>", am.ip_address, addr_str, sizeof(addr_str));

	vty_out(vty, "%-16s %-15s %-15pPAs %-15pPAs %-3s %4u %6u %-15s\n",
		ch->interface->name,
		inet_ntop(AF_INET, &ifaddr, buf, sizeof(buf)), &ch->sg.src,
		&ch->sg.grp, am.rpt_bit_flag ? "yes" : "no",
		am.metric_preference, am.route_metric, addr_str);
}

static void pim_show_assert_metric(struct pim_instance *pim, struct vty *vty)
{
	struct pim_interface *pim_ifp;
	struct pim_ifchannel *ch;
	struct interface *ifp;

	vty_out(vty,
		"Interface        Address         Source          Group           RPT Pref Metric Address        \n");

	FOR_ALL_INTERFACES (pim->vrf, ifp) {
		pim_ifp = ifp->info;
		if (!pim_ifp)
			continue;

		RB_FOREACH (ch, pim_ifchannel_rb, &pim_ifp->ifchannel_rb) {
			pim_show_assert_metric_helper(vty, pim_ifp, ch);
		} /* scan interface channels */
	}
}

static void pim_show_assert_winner_metric_helper(struct vty *vty,
						 struct pim_interface *pim_ifp,
						 struct pim_ifchannel *ch)
{
	char addr_str[INET_ADDRSTRLEN];
	struct pim_assert_metric *am;
	struct in_addr ifaddr;
	char pref_str[16];
	char metr_str[16];
	char buf[PREFIX_STRLEN];

	ifaddr = pim_ifp->primary_address;

	am = &ch->ifassert_winner_metric;

	pim_inet4_dump("<addr?>", am->ip_address, addr_str, sizeof(addr_str));

	if (am->metric_preference == PIM_ASSERT_METRIC_PREFERENCE_MAX)
		snprintf(pref_str, sizeof(pref_str), "INFI");
	else
		snprintf(pref_str, sizeof(pref_str), "%4u",
			 am->metric_preference);

	if (am->route_metric == PIM_ASSERT_ROUTE_METRIC_MAX)
		snprintf(metr_str, sizeof(metr_str), "INFI");
	else
		snprintf(metr_str, sizeof(metr_str), "%6u", am->route_metric);

	vty_out(vty, "%-16s %-15s %-15pPAs %-15pPAs %-3s %-4s %-6s %-15s\n",
		ch->interface->name,
		inet_ntop(AF_INET, &ifaddr, buf, sizeof(buf)), &ch->sg.src,
		&ch->sg.grp, am->rpt_bit_flag ? "yes" : "no", pref_str,
		metr_str, addr_str);
}

static void pim_show_assert_winner_metric(struct pim_instance *pim,
					  struct vty *vty)
{
	struct pim_interface *pim_ifp;
	struct pim_ifchannel *ch;
	struct interface *ifp;

	vty_out(vty,
		"Interface        Address         Source          Group           RPT Pref Metric Address        \n");

	FOR_ALL_INTERFACES (pim->vrf, ifp) {
		pim_ifp = ifp->info;
		if (!pim_ifp)
			continue;

		RB_FOREACH (ch, pim_ifchannel_rb, &pim_ifp->ifchannel_rb) {
			pim_show_assert_winner_metric_helper(vty, pim_ifp, ch);
		} /* scan interface channels */
	}
}

static void json_object_pim_ifp_add(struct json_object *json,
				    struct interface *ifp)
{
	struct pim_interface *pim_ifp;

	pim_ifp = ifp->info;
	json_object_string_add(json, "name", ifp->name);
	json_object_string_add(json, "state", if_is_up(ifp) ? "up" : "down");
	json_object_string_addf(json, "address", "%pI4",
				&pim_ifp->primary_address);
	json_object_int_add(json, "index", ifp->ifindex);

	if (if_is_multicast(ifp))
		json_object_boolean_true_add(json, "flagMulticast");

	if (if_is_broadcast(ifp))
		json_object_boolean_true_add(json, "flagBroadcast");

	if (ifp->flags & IFF_ALLMULTI)
		json_object_boolean_true_add(json, "flagAllMulticast");

	if (ifp->flags & IFF_PROMISC)
		json_object_boolean_true_add(json, "flagPromiscuous");

	if (PIM_IF_IS_DELETED(ifp))
		json_object_boolean_true_add(json, "flagDeleted");

	if (pim_if_lan_delay_enabled(ifp))
		json_object_boolean_true_add(json, "lanDelayEnabled");
}

static void pim_show_membership_helper(struct vty *vty,
				       struct pim_interface *pim_ifp,
				       struct pim_ifchannel *ch,
				       struct json_object *json)
{
	char ch_grp_str[PIM_ADDRSTRLEN];
	json_object *json_iface = NULL;
	json_object *json_row = NULL;

	json_object_object_get_ex(json, ch->interface->name, &json_iface);
	if (!json_iface) {
		json_iface = json_object_new_object();
		json_object_pim_ifp_add(json_iface, ch->interface);
		json_object_object_add(json, ch->interface->name, json_iface);
	}

	snprintfrr(ch_grp_str, sizeof(ch_grp_str), "%pPAs", &ch->sg.grp);

	json_row = json_object_new_object();
	json_object_string_addf(json_row, "source", "%pPAs", &ch->sg.src);
	json_object_string_add(json_row, "group", ch_grp_str);
	json_object_string_add(json_row, "localMembership",
			       ch->local_ifmembership == PIM_IFMEMBERSHIP_NOINFO
			       ? "NOINFO"
			       : "INCLUDE");
	json_object_object_add(json_iface, ch_grp_str, json_row);
}

static void pim_show_membership(struct pim_instance *pim, struct vty *vty,
				bool uj)
{
	struct pim_interface *pim_ifp;
	struct pim_ifchannel *ch;
	struct interface *ifp;
	enum json_type type;
	json_object *json = NULL;
	json_object *json_tmp = NULL;

	json = json_object_new_object();

	FOR_ALL_INTERFACES (pim->vrf, ifp) {
		pim_ifp = ifp->info;
		if (!pim_ifp)
			continue;

		RB_FOREACH (ch, pim_ifchannel_rb, &pim_ifp->ifchannel_rb) {
			pim_show_membership_helper(vty, pim_ifp, ch, json);
		} /* scan interface channels */
	}

	if (uj) {
		vty_out(vty, "%s\n", json_object_to_json_string_ext(
				json, JSON_C_TO_STRING_PRETTY));
	} else {
		vty_out(vty,
			"Interface         Address          Source           Group            Membership\n");

		/*
		 * Example of the json data we are traversing
		 *
		 * {
		 *   "swp3":{
		 *     "name":"swp3",
		 *     "state":"up",
		 *     "address":"10.1.20.1",
		 *     "index":5,
		 *     "flagMulticast":true,
		 *     "flagBroadcast":true,
		 *     "lanDelayEnabled":true,
		 *     "226.10.10.10":{
		 *       "source":"*",
		 *       "group":"226.10.10.10",
		 *       "localMembership":"INCLUDE"
		 *     }
		 *   }
		 * }
		 */

		/* foreach interface */
		json_object_object_foreach(json, key, val)
		{

			/* Find all of the keys where the val is an object. In
			 * the example
			 * above the only one is 226.10.10.10
			 */
			json_object_object_foreach(val, if_field_key,
						   if_field_val)
			{
				type = json_object_get_type(if_field_val);

				if (type == json_type_object) {
					vty_out(vty, "%-16s  ", key);

					json_object_object_get_ex(
						val, "address", &json_tmp);
					vty_out(vty, "%-15s  ",
						json_object_get_string(
							json_tmp));

					json_object_object_get_ex(if_field_val,
								  "source",
								  &json_tmp);
					vty_out(vty, "%-15s  ",
						json_object_get_string(
							json_tmp));

					/* Group */
					vty_out(vty, "%-15s  ", if_field_key);

					json_object_object_get_ex(
						if_field_val, "localMembership",
						&json_tmp);
					vty_out(vty, "%-10s\n",
						json_object_get_string(
							json_tmp));
				}
			}
		}
	}

	json_object_free(json);
}

static void pim_print_ifp_flags(struct vty *vty, struct interface *ifp,
				int mloop)
{
	vty_out(vty, "Flags\n");
	vty_out(vty, "-----\n");
	vty_out(vty, "All Multicast   : %s\n",
		(ifp->flags & IFF_ALLMULTI) ? "yes" : "no");
	vty_out(vty, "Broadcast       : %s\n",
		if_is_broadcast(ifp) ? "yes" : "no");
	vty_out(vty, "Deleted         : %s\n",
		PIM_IF_IS_DELETED(ifp) ? "yes" : "no");
	vty_out(vty, "Interface Index : %d\n", ifp->ifindex);
	vty_out(vty, "Multicast       : %s\n",
		if_is_multicast(ifp) ? "yes" : "no");
	vty_out(vty, "Multicast Loop  : %d\n", mloop);
	vty_out(vty, "Promiscuous     : %s\n",
		(ifp->flags & IFF_PROMISC) ? "yes" : "no");
	vty_out(vty, "\n");
	vty_out(vty, "\n");
}

static void igmp_show_interfaces(struct pim_instance *pim, struct vty *vty,
				 bool uj)
{
	struct interface *ifp;
	time_t now;
	char buf[PREFIX_STRLEN];
	json_object *json = NULL;
	json_object *json_row = NULL;

	now = pim_time_monotonic_sec();

	if (uj)
		json = json_object_new_object();
	else
		vty_out(vty,
			"Interface         State          Address  V  Querier          QuerierIp  Query Timer    Uptime\n");

	FOR_ALL_INTERFACES (pim->vrf, ifp) {
		struct pim_interface *pim_ifp;
		struct listnode *sock_node;
		struct gm_sock *igmp;

		pim_ifp = ifp->info;

		if (!pim_ifp)
			continue;

		for (ALL_LIST_ELEMENTS_RO(pim_ifp->gm_socket_list, sock_node,
					  igmp)) {
			char uptime[10];
			char query_hhmmss[10];

			pim_time_uptime(uptime, sizeof(uptime),
					now - igmp->sock_creation);
			pim_time_timer_to_hhmmss(query_hhmmss,
						 sizeof(query_hhmmss),
						 igmp->t_igmp_query_timer);

			if (uj) {
				json_row = json_object_new_object();
				json_object_pim_ifp_add(json_row, ifp);
				json_object_string_add(json_row, "upTime",
						       uptime);
				json_object_int_add(json_row, "version",
						    pim_ifp->igmp_version);

				if (igmp->t_igmp_query_timer) {
					json_object_boolean_true_add(json_row,
								     "querier");
					json_object_string_add(json_row,
							       "queryTimer",
							       query_hhmmss);
				}
				json_object_string_addf(json_row, "querierIp",
							"%pI4",
							&igmp->querier_addr);

				json_object_object_add(json, ifp->name,
						       json_row);

				if (igmp->mtrace_only) {
					json_object_boolean_true_add(
						json_row, "mtraceOnly");
				}
			} else {
				vty_out(vty,
					"%-16s  %5s  %15s  %d  %7s  %17pI4  %11s  %8s\n",
					ifp->name,
					if_is_up(ifp)
						? (igmp->mtrace_only ? "mtrc"
								     : "up")
						: "down",
					inet_ntop(AF_INET, &igmp->ifaddr, buf,
						  sizeof(buf)),
					pim_ifp->igmp_version,
					igmp->t_igmp_query_timer ? "local"
								 : "other",
					&igmp->querier_addr, query_hhmmss,
					uptime);
			}
		}
	}

	if (uj)
		vty_json(vty, json);
}

static void igmp_show_interfaces_single(struct pim_instance *pim,
					struct vty *vty, const char *ifname,
					bool uj)
{
	struct gm_sock *igmp;
	struct interface *ifp;
	struct listnode *sock_node;
	struct pim_interface *pim_ifp;
	char uptime[10];
	char query_hhmmss[10];
	char other_hhmmss[10];
	int found_ifname = 0;
	int sqi;
	int mloop = 0;
	long gmi_msec; /* Group Membership Interval */
	long lmqt_msec;
	long ohpi_msec;
	long oqpi_msec; /* Other Querier Present Interval */
	long qri_msec;
	time_t now;
	int lmqc;

	json_object *json = NULL;
	json_object *json_row = NULL;

	if (uj)
		json = json_object_new_object();

	now = pim_time_monotonic_sec();

	FOR_ALL_INTERFACES (pim->vrf, ifp) {
		pim_ifp = ifp->info;

		if (!pim_ifp)
			continue;

		if (strcmp(ifname, "detail") && strcmp(ifname, ifp->name))
			continue;

		for (ALL_LIST_ELEMENTS_RO(pim_ifp->gm_socket_list, sock_node,
					  igmp)) {
			found_ifname = 1;
			pim_time_uptime(uptime, sizeof(uptime),
					now - igmp->sock_creation);
			pim_time_timer_to_hhmmss(query_hhmmss,
						 sizeof(query_hhmmss),
						 igmp->t_igmp_query_timer);
			pim_time_timer_to_hhmmss(other_hhmmss,
						 sizeof(other_hhmmss),
						 igmp->t_other_querier_timer);

			gmi_msec = PIM_IGMP_GMI_MSEC(
				igmp->querier_robustness_variable,
				igmp->querier_query_interval,
				pim_ifp->gm_query_max_response_time_dsec);

			sqi = PIM_IGMP_SQI(pim_ifp->gm_default_query_interval);

			oqpi_msec = PIM_IGMP_OQPI_MSEC(
				igmp->querier_robustness_variable,
				igmp->querier_query_interval,
				pim_ifp->gm_query_max_response_time_dsec);

			lmqt_msec = PIM_IGMP_LMQT_MSEC(
				pim_ifp->gm_specific_query_max_response_time_dsec,
				pim_ifp->gm_last_member_query_count);

			ohpi_msec =
				PIM_IGMP_OHPI_DSEC(
					igmp->querier_robustness_variable,
					igmp->querier_query_interval,
					pim_ifp->gm_query_max_response_time_dsec) *
				100;

			qri_msec =
				pim_ifp->gm_query_max_response_time_dsec * 100;
			if (pim_ifp->pim_sock_fd >= 0)
				mloop = pim_socket_mcastloop_get(
					pim_ifp->pim_sock_fd);
			else
				mloop = 0;
			lmqc = pim_ifp->gm_last_member_query_count;

			if (uj) {
				json_row = json_object_new_object();
				json_object_pim_ifp_add(json_row, ifp);
				json_object_string_add(json_row, "upTime",
						       uptime);
				json_object_string_add(json_row, "querier",
						       igmp->t_igmp_query_timer
						       ? "local"
						       : "other");
				json_object_string_addf(json_row, "querierIp",
							"%pI4",
							&igmp->querier_addr);
				json_object_int_add(json_row, "queryStartCount",
						    igmp->startup_query_count);
				json_object_string_add(json_row,
						       "queryQueryTimer",
						       query_hhmmss);
				json_object_string_add(json_row,
						       "queryOtherTimer",
						       other_hhmmss);
				json_object_int_add(json_row, "version",
						    pim_ifp->igmp_version);
				json_object_int_add(
					json_row,
					"timerGroupMembershipIntervalMsec",
					gmi_msec);
				json_object_int_add(json_row,
						    "lastMemberQueryCount",
						    lmqc);
				json_object_int_add(json_row,
						    "timerLastMemberQueryMsec",
						    lmqt_msec);
				json_object_int_add(
					json_row,
					"timerOlderHostPresentIntervalMsec",
					ohpi_msec);
				json_object_int_add(
					json_row,
					"timerOtherQuerierPresentIntervalMsec",
					oqpi_msec);
				json_object_int_add(
					json_row, "timerQueryInterval",
					igmp->querier_query_interval);
				json_object_int_add(
					json_row,
					"timerQueryResponseIntervalMsec",
					qri_msec);
				json_object_int_add(
					json_row, "timerRobustnessVariable",
					igmp->querier_robustness_variable);
				json_object_int_add(json_row,
						    "timerStartupQueryInterval",
						    sqi);

				json_object_object_add(json, ifp->name,
						       json_row);

				if (igmp->mtrace_only) {
					json_object_boolean_true_add(
						json_row, "mtraceOnly");
				}
			} else {
				vty_out(vty, "Interface : %s\n", ifp->name);
				vty_out(vty, "State     : %s\n",
					if_is_up(ifp) ? (igmp->mtrace_only ?
							 "mtrace"
							 : "up")
					: "down");
				vty_out(vty, "Address   : %pI4\n",
					&pim_ifp->primary_address);
				vty_out(vty, "Uptime    : %s\n", uptime);
				vty_out(vty, "Version   : %d\n",
					pim_ifp->igmp_version);
				vty_out(vty, "\n");
				vty_out(vty, "\n");

				vty_out(vty, "Querier\n");
				vty_out(vty, "-------\n");
				vty_out(vty, "Querier     : %s\n",
					igmp->t_igmp_query_timer ? "local"
					: "other");
				vty_out(vty, "QuerierIp   : %pI4",
					&igmp->querier_addr);
				if (pim_ifp->primary_address.s_addr
				    == igmp->querier_addr.s_addr)
					vty_out(vty, " (this router)\n");
				else
					vty_out(vty, "\n");

				vty_out(vty, "Start Count : %d\n",
					igmp->startup_query_count);
				vty_out(vty, "Query Timer : %s\n",
					query_hhmmss);
				vty_out(vty, "Other Timer : %s\n",
					other_hhmmss);
				vty_out(vty, "\n");
				vty_out(vty, "\n");

				vty_out(vty, "Timers\n");
				vty_out(vty, "------\n");
				vty_out(vty,
					"Group Membership Interval      : %lis\n",
					gmi_msec / 1000);
				vty_out(vty,
					"Last Member Query Count        : %d\n",
					lmqc);
				vty_out(vty,
					"Last Member Query Time         : %lis\n",
					lmqt_msec / 1000);
				vty_out(vty,
					"Older Host Present Interval    : %lis\n",
					ohpi_msec / 1000);
				vty_out(vty,
					"Other Querier Present Interval : %lis\n",
					oqpi_msec / 1000);
				vty_out(vty,
					"Query Interval                 : %ds\n",
					igmp->querier_query_interval);
				vty_out(vty,
					"Query Response Interval        : %lis\n",
					qri_msec / 1000);
				vty_out(vty,
					"Robustness Variable            : %d\n",
					igmp->querier_robustness_variable);
				vty_out(vty,
					"Startup Query Interval         : %ds\n",
					sqi);
				vty_out(vty, "\n");
				vty_out(vty, "\n");

				pim_print_ifp_flags(vty, ifp, mloop);
			}
		}
	}

	if (uj)
		vty_json(vty, json);
	else if (!found_ifname)
		vty_out(vty, "%% No such interface\n");
}

static void igmp_show_interface_join(struct pim_instance *pim, struct vty *vty,
				     bool uj)
{
	struct interface *ifp;
	time_t now;
	json_object *json = NULL;
	json_object *json_iface = NULL;
	json_object *json_grp = NULL;
	json_object *json_grp_arr = NULL;

	now = pim_time_monotonic_sec();

	if (uj) {
		json = json_object_new_object();
		json_object_string_add(json, "vrf",
				       vrf_id_to_name(pim->vrf->vrf_id));
	} else {
		vty_out(vty,
			"Interface        Address         Source          Group           Socket Uptime  \n");
	}

	FOR_ALL_INTERFACES (pim->vrf, ifp) {
		struct pim_interface *pim_ifp;
		struct listnode *join_node;
		struct gm_join *ij;
		struct in_addr pri_addr;
		char pri_addr_str[INET_ADDRSTRLEN];

		pim_ifp = ifp->info;

		if (!pim_ifp)
			continue;

		if (!pim_ifp->gm_join_list)
			continue;

		pri_addr = pim_find_primary_addr(ifp);
		pim_inet4_dump("<pri?>", pri_addr, pri_addr_str,
			       sizeof(pri_addr_str));

		for (ALL_LIST_ELEMENTS_RO(pim_ifp->gm_join_list, join_node,
					  ij)) {
			char group_str[INET_ADDRSTRLEN];
			char source_str[INET_ADDRSTRLEN];
			char uptime[10];

			pim_time_uptime(uptime, sizeof(uptime),
					now - ij->sock_creation);
			pim_inet4_dump("<grp?>", ij->group_addr, group_str,
				       sizeof(group_str));
			pim_inet4_dump("<src?>", ij->source_addr, source_str,
				       sizeof(source_str));

			if (uj) {
				json_object_object_get_ex(json, ifp->name,
							  &json_iface);

				if (!json_iface) {
					json_iface = json_object_new_object();
					json_object_string_add(
						json_iface, "name", ifp->name);
					json_object_object_add(json, ifp->name,
							       json_iface);
					json_grp_arr = json_object_new_array();
					json_object_object_add(json_iface,
							       "groups",
							       json_grp_arr);
				}

				json_grp = json_object_new_object();
				json_object_string_add(json_grp, "source",
						       source_str);
				json_object_string_add(json_grp, "group",
						       group_str);
				json_object_string_add(json_grp, "primaryAddr",
						       pri_addr_str);
				json_object_int_add(json_grp, "sockFd",
						    ij->sock_fd);
				json_object_string_add(json_grp, "upTime",
						       uptime);
				json_object_array_add(json_grp_arr, json_grp);
			} else {
				vty_out(vty,
					"%-16s %-15s %-15s %-15s %6d %8s\n",
					ifp->name, pri_addr_str, source_str,
					group_str, ij->sock_fd, uptime);
			}
		} /* for (pim_ifp->gm_join_list) */

	} /* for (iflist) */

	if (uj)
		vty_json(vty, json);
}

static void pim_show_interfaces_single(struct pim_instance *pim,
				       struct vty *vty, const char *ifname,
				       bool mlag, bool uj)
{
	struct in_addr ifaddr;
	struct interface *ifp;
	struct listnode *neighnode;
	struct pim_interface *pim_ifp;
	struct pim_neighbor *neigh;
	struct pim_upstream *up;
	time_t now;
	char dr_str[INET_ADDRSTRLEN];
	char dr_uptime[10];
	char expire[10];
	char grp_str[INET_ADDRSTRLEN];
	char hello_period[10];
	char hello_timer[10];
	char neigh_src_str[INET_ADDRSTRLEN];
	char src_str[INET_ADDRSTRLEN];
	char stat_uptime[10];
	char uptime[10];
	int mloop = 0;
	int found_ifname = 0;
	int print_header;
	json_object *json = NULL;
	json_object *json_row = NULL;
	json_object *json_pim_neighbor = NULL;
	json_object *json_pim_neighbors = NULL;
	json_object *json_group = NULL;
	json_object *json_group_source = NULL;
	json_object *json_fhr_sources = NULL;
	struct pim_secondary_addr *sec_addr;
	struct listnode *sec_node;

	now = pim_time_monotonic_sec();

	if (uj)
		json = json_object_new_object();

	FOR_ALL_INTERFACES (pim->vrf, ifp) {
		pim_ifp = ifp->info;

		if (!pim_ifp)
			continue;

		if (mlag == true && pim_ifp->activeactive == false)
			continue;

		if (strcmp(ifname, "detail") && strcmp(ifname, ifp->name))
			continue;

		found_ifname = 1;
		ifaddr = pim_ifp->primary_address;
		pim_inet4_dump("<dr?>", pim_ifp->pim_dr_addr, dr_str,
			       sizeof(dr_str));
		pim_time_uptime_begin(dr_uptime, sizeof(dr_uptime), now,
				      pim_ifp->pim_dr_election_last);
		pim_time_timer_to_hhmmss(hello_timer, sizeof(hello_timer),
					 pim_ifp->t_pim_hello_timer);
		pim_time_mmss(hello_period, sizeof(hello_period),
			      pim_ifp->pim_hello_period);
		pim_time_uptime(stat_uptime, sizeof(stat_uptime),
				now - pim_ifp->pim_ifstat_start);
		if (pim_ifp->pim_sock_fd >= 0)
			mloop = pim_socket_mcastloop_get(pim_ifp->pim_sock_fd);
		else
			mloop = 0;

		if (uj) {
			char pbuf[PREFIX2STR_BUFFER];
			json_row = json_object_new_object();
			json_object_pim_ifp_add(json_row, ifp);

			if (pim_ifp->update_source.s_addr != INADDR_ANY) {
				json_object_string_addf(
					json_row, "useSource", "%pI4",
					&pim_ifp->update_source);
			}
			if (pim_ifp->sec_addr_list) {
				json_object *sec_list = NULL;

				sec_list = json_object_new_array();
				for (ALL_LIST_ELEMENTS_RO(
					     pim_ifp->sec_addr_list, sec_node,
					     sec_addr)) {
					json_object_array_add(
						sec_list,
						json_object_new_string(
							prefix2str(
								&sec_addr->addr,
								pbuf,
								sizeof(pbuf))));
				}
				json_object_object_add(json_row,
						       "secondaryAddressList",
						       sec_list);
			}

			// PIM neighbors
			if (pim_ifp->pim_neighbor_list->count) {
				json_pim_neighbors = json_object_new_object();

				for (ALL_LIST_ELEMENTS_RO(
					     pim_ifp->pim_neighbor_list,
					     neighnode, neigh)) {
					json_pim_neighbor =
						json_object_new_object();
					pim_inet4_dump("<src?>",
						       neigh->source_addr,
						       neigh_src_str,
						       sizeof(neigh_src_str));
					pim_time_uptime(uptime, sizeof(uptime),
							now - neigh->creation);
					pim_time_timer_to_hhmmss(
						expire, sizeof(expire),
						neigh->t_expire_timer);

					json_object_string_add(
						json_pim_neighbor, "address",
						neigh_src_str);
					json_object_string_add(
						json_pim_neighbor, "upTime",
						uptime);
					json_object_string_add(
						json_pim_neighbor, "holdtime",
						expire);

					json_object_object_add(
						json_pim_neighbors,
						neigh_src_str,
						json_pim_neighbor);
				}

				json_object_object_add(json_row, "neighbors",
						       json_pim_neighbors);
			}

			json_object_string_add(json_row, "drAddress", dr_str);
			json_object_int_add(json_row, "drPriority",
					    pim_ifp->pim_dr_priority);
			json_object_string_add(json_row, "drUptime", dr_uptime);
			json_object_int_add(json_row, "drElections",
					    pim_ifp->pim_dr_election_count);
			json_object_int_add(json_row, "drChanges",
					    pim_ifp->pim_dr_election_changes);

			// FHR
			frr_each (rb_pim_upstream, &pim->upstream_head, up) {
				if (ifp != up->rpf.source_nexthop.interface)
					continue;

				if (!(up->flags & PIM_UPSTREAM_FLAG_MASK_FHR))
					continue;

				if (!json_fhr_sources)
					json_fhr_sources =
						json_object_new_object();

				snprintfrr(grp_str, sizeof(grp_str), "%pPAs",
					   &up->sg.grp);
				snprintfrr(src_str, sizeof(src_str), "%pPAs",
					   &up->sg.src);
				pim_time_uptime(uptime, sizeof(uptime),
						now - up->state_transition);

				/*
				 * Does this group live in json_fhr_sources?
				 * If not create it.
				 */
				json_object_object_get_ex(json_fhr_sources,
							  grp_str, &json_group);

				if (!json_group) {
					json_group = json_object_new_object();
					json_object_object_add(json_fhr_sources,
							       grp_str,
							       json_group);
				}

				json_group_source = json_object_new_object();
				json_object_string_add(json_group_source,
						       "source", src_str);
				json_object_string_add(json_group_source,
						       "group", grp_str);
				json_object_string_add(json_group_source,
						       "upTime", uptime);
				json_object_object_add(json_group, src_str,
						       json_group_source);
			}

			if (json_fhr_sources) {
				json_object_object_add(json_row,
						       "firstHopRouter",
						       json_fhr_sources);
			}

			json_object_int_add(json_row, "helloPeriod",
					    pim_ifp->pim_hello_period);
			json_object_int_add(json_row, "holdTime",
					    PIM_IF_DEFAULT_HOLDTIME(pim_ifp));
			json_object_string_add(json_row, "helloTimer",
					       hello_timer);
			json_object_string_add(json_row, "helloStatStart",
					       stat_uptime);
			json_object_int_add(json_row, "helloReceived",
					    pim_ifp->pim_ifstat_hello_recv);
			json_object_int_add(json_row, "helloReceivedFailed",
					    pim_ifp->pim_ifstat_hello_recvfail);
			json_object_int_add(json_row, "helloSend",
					    pim_ifp->pim_ifstat_hello_sent);
			json_object_int_add(json_row, "hellosendFailed",
					    pim_ifp->pim_ifstat_hello_sendfail);
			json_object_int_add(json_row, "helloGenerationId",
					    pim_ifp->pim_generation_id);
			json_object_int_add(json_row, "flagMulticastLoop",
					    mloop);

			json_object_int_add(
				json_row, "effectivePropagationDelay",
				pim_if_effective_propagation_delay_msec(ifp));
			json_object_int_add(
				json_row, "effectiveOverrideInterval",
				pim_if_effective_override_interval_msec(ifp));
			json_object_int_add(
				json_row, "joinPruneOverrideInterval",
				pim_if_jp_override_interval_msec(ifp));

			json_object_int_add(
				json_row, "propagationDelay",
				pim_ifp->pim_propagation_delay_msec);
			json_object_int_add(
				json_row, "propagationDelayHighest",
				pim_ifp->pim_neighbors_highest_propagation_delay_msec);
			json_object_int_add(
				json_row, "overrideInterval",
				pim_ifp->pim_override_interval_msec);
			json_object_int_add(
				json_row, "overrideIntervalHighest",
				pim_ifp->pim_neighbors_highest_override_interval_msec);
			if (pim_ifp->bsm_enable)
				json_object_boolean_true_add(json_row,
							     "bsmEnabled");
			if (pim_ifp->ucast_bsm_accept)
				json_object_boolean_true_add(json_row,
							     "ucastBsmEnabled");
			json_object_object_add(json, ifp->name, json_row);

		} else {
			vty_out(vty, "Interface  : %s\n", ifp->name);
			vty_out(vty, "State      : %s\n",
				if_is_up(ifp) ? "up" : "down");
			if (pim_ifp->update_source.s_addr != INADDR_ANY) {
				vty_out(vty, "Use Source : %pI4\n",
					&pim_ifp->update_source);
			}
			if (pim_ifp->sec_addr_list) {
				vty_out(vty, "Address    : %pI4 (primary)\n",
					&ifaddr);
				for (ALL_LIST_ELEMENTS_RO(
					     pim_ifp->sec_addr_list, sec_node,
					     sec_addr))
					vty_out(vty, "             %pFX\n",
						&sec_addr->addr);
			} else {
				vty_out(vty, "Address    : %pI4\n",
					&ifaddr);
			}
			vty_out(vty, "\n");

			// PIM neighbors
			print_header = 1;

			for (ALL_LIST_ELEMENTS_RO(pim_ifp->pim_neighbor_list,
						  neighnode, neigh)) {

				if (print_header) {
					vty_out(vty, "PIM Neighbors\n");
					vty_out(vty, "-------------\n");
					print_header = 0;
				}

				pim_inet4_dump("<src?>", neigh->source_addr,
					       neigh_src_str,
					       sizeof(neigh_src_str));
				pim_time_uptime(uptime, sizeof(uptime),
						now - neigh->creation);
				pim_time_timer_to_hhmmss(expire, sizeof(expire),
							 neigh->t_expire_timer);
				vty_out(vty,
					"%-15s : up for %s, holdtime expires in %s\n",
					neigh_src_str, uptime, expire);
			}

			if (!print_header) {
				vty_out(vty, "\n");
				vty_out(vty, "\n");
			}

			vty_out(vty, "Designated Router\n");
			vty_out(vty, "-----------------\n");
			vty_out(vty, "Address   : %s\n", dr_str);
			vty_out(vty, "Priority  : %u(%d)\n",
				pim_ifp->pim_dr_priority,
				pim_ifp->pim_dr_num_nondrpri_neighbors);
			vty_out(vty, "Uptime    : %s\n", dr_uptime);
			vty_out(vty, "Elections : %d\n",
				pim_ifp->pim_dr_election_count);
			vty_out(vty, "Changes   : %d\n",
				pim_ifp->pim_dr_election_changes);
			vty_out(vty, "\n");
			vty_out(vty, "\n");

			// FHR
			print_header = 1;
			frr_each (rb_pim_upstream, &pim->upstream_head, up) {
				if (!up->rpf.source_nexthop.interface)
					continue;

				if (strcmp(ifp->name,
					   up->rpf.source_nexthop
					   .interface->name)
				    != 0)
					continue;

				if (!(up->flags & PIM_UPSTREAM_FLAG_MASK_FHR))
					continue;

				if (print_header) {
					vty_out(vty,
						"FHR - First Hop Router\n");
					vty_out(vty,
						"----------------------\n");
					print_header = 0;
				}

				pim_time_uptime(uptime, sizeof(uptime),
						now - up->state_transition);
				vty_out(vty,
					"%pPAs : %pPAs is a source, uptime is %s\n",
					&up->sg.grp, &up->sg.src, uptime);
			}

			if (!print_header) {
				vty_out(vty, "\n");
				vty_out(vty, "\n");
			}

			vty_out(vty, "Hellos\n");
			vty_out(vty, "------\n");
			vty_out(vty, "Period         : %d\n",
				pim_ifp->pim_hello_period);
			vty_out(vty, "HoldTime       : %d\n",
				PIM_IF_DEFAULT_HOLDTIME(pim_ifp));
			vty_out(vty, "Timer          : %s\n", hello_timer);
			vty_out(vty, "StatStart      : %s\n", stat_uptime);
			vty_out(vty, "Receive        : %d\n",
				pim_ifp->pim_ifstat_hello_recv);
			vty_out(vty, "Receive Failed : %d\n",
				pim_ifp->pim_ifstat_hello_recvfail);
			vty_out(vty, "Send           : %d\n",
				pim_ifp->pim_ifstat_hello_sent);
			vty_out(vty, "Send Failed    : %d\n",
				pim_ifp->pim_ifstat_hello_sendfail);
			vty_out(vty, "Generation ID  : %08x\n",
				pim_ifp->pim_generation_id);
			vty_out(vty, "\n");
			vty_out(vty, "\n");

			pim_print_ifp_flags(vty, ifp, mloop);

			vty_out(vty, "Join Prune Interval\n");
			vty_out(vty, "-------------------\n");
			vty_out(vty, "LAN Delay                    : %s\n",
				pim_if_lan_delay_enabled(ifp) ? "yes" : "no");
			vty_out(vty, "Effective Propagation Delay  : %d msec\n",
				pim_if_effective_propagation_delay_msec(ifp));
			vty_out(vty, "Effective Override Interval  : %d msec\n",
				pim_if_effective_override_interval_msec(ifp));
			vty_out(vty, "Join Prune Override Interval : %d msec\n",
				pim_if_jp_override_interval_msec(ifp));
			vty_out(vty, "\n");
			vty_out(vty, "\n");

			vty_out(vty, "LAN Prune Delay\n");
			vty_out(vty, "---------------\n");
			vty_out(vty, "Propagation Delay           : %d msec\n",
				pim_ifp->pim_propagation_delay_msec);
			vty_out(vty, "Propagation Delay (Highest) : %d msec\n",
				pim_ifp->pim_neighbors_highest_propagation_delay_msec);
			vty_out(vty, "Override Interval           : %d msec\n",
				pim_ifp->pim_override_interval_msec);
			vty_out(vty, "Override Interval (Highest) : %d msec\n",
				pim_ifp->pim_neighbors_highest_override_interval_msec);
			vty_out(vty, "\n");
			vty_out(vty, "\n");

			vty_out(vty, "BSM Status\n");
			vty_out(vty, "----------\n");
			vty_out(vty, "Bsm Enabled          : %s\n",
				pim_ifp->bsm_enable ? "yes" : "no");
			vty_out(vty, "Unicast Bsm Enabled  : %s\n",
				pim_ifp->ucast_bsm_accept ? "yes" : "no");
			vty_out(vty, "\n");
			vty_out(vty, "\n");
		}
	}

	if (uj)
		vty_json(vty, json);
	else if (!found_ifname)
		vty_out(vty, "%% No such interface\n");
}

static void igmp_show_statistics(struct pim_instance *pim, struct vty *vty,
				 const char *ifname, bool uj)
{
	struct interface *ifp;
	struct igmp_stats rx_stats;

	igmp_stats_init(&rx_stats);

	FOR_ALL_INTERFACES (pim->vrf, ifp) {
		struct pim_interface *pim_ifp;
		struct listnode *sock_node;
		struct gm_sock *igmp;

		pim_ifp = ifp->info;

		if (!pim_ifp)
			continue;

		if (ifname && strcmp(ifname, ifp->name))
			continue;

		for (ALL_LIST_ELEMENTS_RO(pim_ifp->gm_socket_list, sock_node,
					  igmp)) {
			igmp_stats_add(&rx_stats, &igmp->rx_stats);
		}
	}
	if (uj) {
		json_object *json = NULL;
		json_object *json_row = NULL;

		json = json_object_new_object();
		json_row = json_object_new_object();

		json_object_string_add(json_row, "name", ifname ? ifname :
				       "global");
		json_object_int_add(json_row, "queryV1", rx_stats.query_v1);
		json_object_int_add(json_row, "queryV2", rx_stats.query_v2);
		json_object_int_add(json_row, "queryV3", rx_stats.query_v3);
		json_object_int_add(json_row, "leaveV3", rx_stats.leave_v2);
		json_object_int_add(json_row, "reportV1", rx_stats.report_v1);
		json_object_int_add(json_row, "reportV2", rx_stats.report_v2);
		json_object_int_add(json_row, "reportV3", rx_stats.report_v3);
		json_object_int_add(json_row, "mtraceResponse",
				    rx_stats.mtrace_rsp);
		json_object_int_add(json_row, "mtraceRequest",
				    rx_stats.mtrace_req);
		json_object_int_add(json_row, "unsupported",
				    rx_stats.unsupported);
		json_object_object_add(json, ifname ? ifname : "global",
				       json_row);
		vty_json(vty, json);
	} else {
		vty_out(vty, "IGMP RX statistics\n");
		vty_out(vty, "Interface       : %s\n",
			ifname ? ifname : "global");
		vty_out(vty, "V1 query        : %u\n", rx_stats.query_v1);
		vty_out(vty, "V2 query        : %u\n", rx_stats.query_v2);
		vty_out(vty, "V3 query        : %u\n", rx_stats.query_v3);
		vty_out(vty, "V2 leave        : %u\n", rx_stats.leave_v2);
		vty_out(vty, "V1 report       : %u\n", rx_stats.report_v1);
		vty_out(vty, "V2 report       : %u\n", rx_stats.report_v2);
		vty_out(vty, "V3 report       : %u\n", rx_stats.report_v3);
		vty_out(vty, "mtrace response : %u\n", rx_stats.mtrace_rsp);
		vty_out(vty, "mtrace request  : %u\n", rx_stats.mtrace_req);
		vty_out(vty, "unsupported     : %u\n", rx_stats.unsupported);
	}
}

static void pim_show_interfaces(struct pim_instance *pim, struct vty *vty,
				bool mlag, bool uj)
{
	struct interface *ifp;
	struct pim_interface *pim_ifp;
	struct pim_upstream *up;
	int fhr = 0;
	int pim_nbrs = 0;
	int pim_ifchannels = 0;
	json_object *json = NULL;
	json_object *json_row = NULL;
	json_object *json_tmp;

	json = json_object_new_object();

	FOR_ALL_INTERFACES (pim->vrf, ifp) {
		pim_ifp = ifp->info;

		if (!pim_ifp)
			continue;

		if (mlag == true && pim_ifp->activeactive == false)
			continue;

		pim_nbrs = pim_ifp->pim_neighbor_list->count;
		pim_ifchannels = pim_if_ifchannel_count(pim_ifp);
		fhr = 0;

		frr_each (rb_pim_upstream, &pim->upstream_head, up)
			if (ifp == up->rpf.source_nexthop.interface)
				if (up->flags & PIM_UPSTREAM_FLAG_MASK_FHR)
					fhr++;

		json_row = json_object_new_object();
		json_object_pim_ifp_add(json_row, ifp);
		json_object_int_add(json_row, "pimNeighbors", pim_nbrs);
		json_object_int_add(json_row, "pimIfChannels", pim_ifchannels);
		json_object_int_add(json_row, "firstHopRouterCount", fhr);
		json_object_string_addf(json_row, "pimDesignatedRouter", "%pI4",
					&pim_ifp->pim_dr_addr);

		if (pim_ifp->pim_dr_addr.s_addr
		    == pim_ifp->primary_address.s_addr)
			json_object_boolean_true_add(
				json_row, "pimDesignatedRouterLocal");

		json_object_object_add(json, ifp->name, json_row);
	}

	if (uj) {
		vty_out(vty, "%s\n", json_object_to_json_string_ext(
				json, JSON_C_TO_STRING_PRETTY));
	} else {
		vty_out(vty,
			"Interface         State          Address  PIM Nbrs           PIM DR  FHR IfChannels\n");

		json_object_object_foreach(json, key, val)
		{
			vty_out(vty, "%-16s  ", key);

			json_object_object_get_ex(val, "state", &json_tmp);
			vty_out(vty, "%5s  ", json_object_get_string(json_tmp));

			json_object_object_get_ex(val, "address", &json_tmp);
			vty_out(vty, "%15s  ",
				json_object_get_string(json_tmp));

			json_object_object_get_ex(val, "pimNeighbors",
						  &json_tmp);
			vty_out(vty, "%8d  ", json_object_get_int(json_tmp));

			if (json_object_object_get_ex(
				    val, "pimDesignatedRouterLocal",
				    &json_tmp)) {
				vty_out(vty, "%15s  ", "local");
			} else {
				json_object_object_get_ex(
					val, "pimDesignatedRouter", &json_tmp);
				vty_out(vty, "%15s  ",
					json_object_get_string(json_tmp));
			}

			json_object_object_get_ex(val, "firstHopRouter",
						  &json_tmp);
			vty_out(vty, "%3d  ", json_object_get_int(json_tmp));

			json_object_object_get_ex(val, "pimIfChannels",
						  &json_tmp);
			vty_out(vty, "%9d\n", json_object_get_int(json_tmp));
		}
	}

	json_object_free(json);
}

static void pim_show_interface_traffic(struct pim_instance *pim,
				       struct vty *vty, bool uj)
{
	struct interface *ifp = NULL;
	struct pim_interface *pim_ifp = NULL;
	json_object *json = NULL;
	json_object *json_row = NULL;

	if (uj)
		json = json_object_new_object();
	else {
		vty_out(vty, "\n");
		vty_out(vty, "%-16s%-17s%-17s%-17s%-17s%-17s%-17s%-17s\n",
			"Interface", "       HELLO", "       JOIN",
			"      PRUNE", "   REGISTER", "REGISTER-STOP",
			"  ASSERT", "  BSM");
		vty_out(vty, "%-16s%-17s%-17s%-17s%-17s%-17s%-17s%-17s\n", "",
			"       Rx/Tx", "       Rx/Tx", "      Rx/Tx",
			"      Rx/Tx", "     Rx/Tx", "    Rx/Tx",
			"   Rx/Tx");
		vty_out(vty,
			"---------------------------------------------------------------------------------------------------------------\n");
	}

	FOR_ALL_INTERFACES (pim->vrf, ifp) {
		pim_ifp = ifp->info;

		if (!pim_ifp)
			continue;

		if (pim_ifp->pim_sock_fd < 0)
			continue;
		if (uj) {
			json_row = json_object_new_object();
			json_object_pim_ifp_add(json_row, ifp);
			json_object_int_add(json_row, "helloRx",
					    pim_ifp->pim_ifstat_hello_recv);
			json_object_int_add(json_row, "helloTx",
					    pim_ifp->pim_ifstat_hello_sent);
			json_object_int_add(json_row, "joinRx",
					    pim_ifp->pim_ifstat_join_recv);
			json_object_int_add(json_row, "joinTx",
					    pim_ifp->pim_ifstat_join_send);
			json_object_int_add(json_row, "pruneTx",
					    pim_ifp->pim_ifstat_prune_send);
			json_object_int_add(json_row, "pruneRx",
					    pim_ifp->pim_ifstat_prune_recv);
			json_object_int_add(json_row, "registerRx",
					    pim_ifp->pim_ifstat_reg_recv);
			json_object_int_add(json_row, "registerTx",
					    pim_ifp->pim_ifstat_reg_recv);
			json_object_int_add(json_row, "registerStopRx",
					    pim_ifp->pim_ifstat_reg_stop_recv);
			json_object_int_add(json_row, "registerStopTx",
					    pim_ifp->pim_ifstat_reg_stop_send);
			json_object_int_add(json_row, "assertRx",
					    pim_ifp->pim_ifstat_assert_recv);
			json_object_int_add(json_row, "assertTx",
					    pim_ifp->pim_ifstat_assert_send);
			json_object_int_add(json_row, "bsmRx",
					    pim_ifp->pim_ifstat_bsm_rx);
			json_object_int_add(json_row, "bsmTx",
					    pim_ifp->pim_ifstat_bsm_tx);
			json_object_object_add(json, ifp->name, json_row);
		} else {
			vty_out(vty,
				"%-16s %8u/%-8u %7u/%-7u %7u/%-7u %7u/%-7u %7u/%-7u %7u/%-7u %7" PRIu64 "/%-7" PRIu64 "\n",
				ifp->name, pim_ifp->pim_ifstat_hello_recv,
				pim_ifp->pim_ifstat_hello_sent,
				pim_ifp->pim_ifstat_join_recv,
				pim_ifp->pim_ifstat_join_send,
				pim_ifp->pim_ifstat_prune_recv,
				pim_ifp->pim_ifstat_prune_send,
				pim_ifp->pim_ifstat_reg_recv,
				pim_ifp->pim_ifstat_reg_send,
				pim_ifp->pim_ifstat_reg_stop_recv,
				pim_ifp->pim_ifstat_reg_stop_send,
				pim_ifp->pim_ifstat_assert_recv,
				pim_ifp->pim_ifstat_assert_send,
				pim_ifp->pim_ifstat_bsm_rx,
				pim_ifp->pim_ifstat_bsm_tx);
		}
	}
	if (uj)
		vty_json(vty, json);
}

static void pim_show_interface_traffic_single(struct pim_instance *pim,
					      struct vty *vty,
					      const char *ifname, bool uj)
{
	struct interface *ifp = NULL;
	struct pim_interface *pim_ifp = NULL;
	json_object *json = NULL;
	json_object *json_row = NULL;
	uint8_t found_ifname = 0;

	if (uj)
		json = json_object_new_object();
	else {
		vty_out(vty, "\n");
		vty_out(vty, "%-16s%-17s%-17s%-17s%-17s%-17s%-17s%-17s\n",
			"Interface", "    HELLO", "    JOIN", "   PRUNE",
			"   REGISTER", "  REGISTER-STOP", "  ASSERT",
			"    BSM");
		vty_out(vty, "%-14s%-18s%-17s%-17s%-17s%-17s%-17s%-17s\n", "",
			"      Rx/Tx", "     Rx/Tx", "    Rx/Tx", "    Rx/Tx",
			"     Rx/Tx", "    Rx/Tx", "    Rx/Tx");
		vty_out(vty,
			"-------------------------------------------------------------------------------------------------------------------------------\n");
	}

	FOR_ALL_INTERFACES (pim->vrf, ifp) {
		if (strcmp(ifname, ifp->name))
			continue;

		pim_ifp = ifp->info;

		if (!pim_ifp)
			continue;

		if (pim_ifp->pim_sock_fd < 0)
			continue;

		found_ifname = 1;
		if (uj) {
			json_row = json_object_new_object();
			json_object_pim_ifp_add(json_row, ifp);
			json_object_int_add(json_row, "helloRx",
					    pim_ifp->pim_ifstat_hello_recv);
			json_object_int_add(json_row, "helloTx",
					    pim_ifp->pim_ifstat_hello_sent);
			json_object_int_add(json_row, "joinRx",
					    pim_ifp->pim_ifstat_join_recv);
			json_object_int_add(json_row, "joinTx",
					    pim_ifp->pim_ifstat_join_send);
			json_object_int_add(json_row, "registerRx",
					    pim_ifp->pim_ifstat_reg_recv);
			json_object_int_add(json_row, "registerTx",
					    pim_ifp->pim_ifstat_reg_recv);
			json_object_int_add(json_row, "registerStopRx",
					    pim_ifp->pim_ifstat_reg_stop_recv);
			json_object_int_add(json_row, "registerStopTx",
					    pim_ifp->pim_ifstat_reg_stop_send);
			json_object_int_add(json_row, "assertRx",
					    pim_ifp->pim_ifstat_assert_recv);
			json_object_int_add(json_row, "assertTx",
					    pim_ifp->pim_ifstat_assert_send);
			json_object_int_add(json_row, "bsmRx",
					    pim_ifp->pim_ifstat_bsm_rx);
			json_object_int_add(json_row, "bsmTx",
					    pim_ifp->pim_ifstat_bsm_tx);

			json_object_object_add(json, ifp->name, json_row);
		} else {
			vty_out(vty,
				"%-16s %8u/%-8u %7u/%-7u %7u/%-7u %7u/%-7u %7u/%-7u %7u/%-7u %7" PRIu64 "/%-7" PRIu64 "\n",
				ifp->name, pim_ifp->pim_ifstat_hello_recv,
				pim_ifp->pim_ifstat_hello_sent,
				pim_ifp->pim_ifstat_join_recv,
				pim_ifp->pim_ifstat_join_send,
				pim_ifp->pim_ifstat_prune_recv,
				pim_ifp->pim_ifstat_prune_send,
				pim_ifp->pim_ifstat_reg_recv,
				pim_ifp->pim_ifstat_reg_send,
				pim_ifp->pim_ifstat_reg_stop_recv,
				pim_ifp->pim_ifstat_reg_stop_send,
				pim_ifp->pim_ifstat_assert_recv,
				pim_ifp->pim_ifstat_assert_send,
				pim_ifp->pim_ifstat_bsm_rx,
				pim_ifp->pim_ifstat_bsm_tx);
		}
	}
	if (uj)
		vty_json(vty, json);
	else if (!found_ifname)
		vty_out(vty, "%% No such interface\n");
}

static void pim_show_join_helper(struct vty *vty, struct pim_interface *pim_ifp,
				 struct pim_ifchannel *ch, json_object *json,
				 time_t now, bool uj)
{
	json_object *json_iface = NULL;
	json_object *json_row = NULL;
	json_object *json_grp = NULL;
	struct in_addr ifaddr;
	char uptime[10];
	char expire[10];
	char prune[10];
	char buf[PREFIX_STRLEN];

	ifaddr = pim_ifp->primary_address;

	pim_time_uptime_begin(uptime, sizeof(uptime), now, ch->ifjoin_creation);
	pim_time_timer_to_mmss(expire, sizeof(expire),
			       ch->t_ifjoin_expiry_timer);
	pim_time_timer_to_mmss(prune, sizeof(prune),
			       ch->t_ifjoin_prune_pending_timer);

	if (uj) {
		char ch_grp_str[PIM_ADDRSTRLEN];
		char ch_src_str[PIM_ADDRSTRLEN];

		snprintfrr(ch_grp_str, sizeof(ch_grp_str), "%pPAs",
			   &ch->sg.grp);
		snprintfrr(ch_src_str, sizeof(ch_src_str), "%pPAs",
			   &ch->sg.src);

		json_object_object_get_ex(json, ch->interface->name,
					  &json_iface);

		if (!json_iface) {
			json_iface = json_object_new_object();
			json_object_pim_ifp_add(json_iface, ch->interface);
			json_object_object_add(json, ch->interface->name,
					       json_iface);
		}

		json_row = json_object_new_object();
		json_object_string_add(json_row, "source", ch_src_str);
		json_object_string_add(json_row, "group", ch_grp_str);
		json_object_string_add(json_row, "upTime", uptime);
		json_object_string_add(json_row, "expire", expire);
		json_object_string_add(json_row, "prune", prune);
		json_object_string_add(
			json_row, "channelJoinName",
			pim_ifchannel_ifjoin_name(ch->ifjoin_state, ch->flags));
		if (PIM_IF_FLAG_TEST_S_G_RPT(ch->flags))
			json_object_int_add(json_row, "SGRpt", 1);
		if (PIM_IF_FLAG_TEST_PROTO_PIM(ch->flags))
			json_object_int_add(json_row, "protocolPim", 1);
		if (PIM_IF_FLAG_TEST_PROTO_IGMP(ch->flags))
			json_object_int_add(json_row, "protocolIgmp", 1);
		json_object_object_get_ex(json_iface, ch_grp_str, &json_grp);
		if (!json_grp) {
			json_grp = json_object_new_object();
			json_object_object_add(json_grp, ch_src_str, json_row);
			json_object_object_add(json_iface, ch_grp_str,
					       json_grp);
		} else
			json_object_object_add(json_grp, ch_src_str, json_row);
	} else {
		vty_out(vty, "%-16s %-15s %-15pPAs %-15pPAs %-10s %8s %-6s %5s\n",
			ch->interface->name,
			inet_ntop(AF_INET, &ifaddr, buf, sizeof(buf)),
			&ch->sg.src, &ch->sg.grp,
			pim_ifchannel_ifjoin_name(ch->ifjoin_state, ch->flags),
			uptime, expire, prune);
	}
}

static void pim_show_join(struct pim_instance *pim, struct vty *vty,
			  pim_sgaddr *sg, bool uj)
{
	struct pim_interface *pim_ifp;
	struct pim_ifchannel *ch;
	struct interface *ifp;
	time_t now;
	json_object *json = NULL;

	now = pim_time_monotonic_sec();

	if (uj)
		json = json_object_new_object();
	else
		vty_out(vty,
			"Interface        Address         Source          Group           State      Uptime   Expire Prune\n");

	FOR_ALL_INTERFACES (pim->vrf, ifp) {
		pim_ifp = ifp->info;
		if (!pim_ifp)
			continue;

		RB_FOREACH (ch, pim_ifchannel_rb, &pim_ifp->ifchannel_rb) {
			if (!pim_sgaddr_match(ch->sg, *sg))
				continue;
			pim_show_join_helper(vty, pim_ifp, ch, json, now, uj);
		} /* scan interface channels */
	}

	if (uj)
		vty_json(vty, json);
}

static void pim_show_neighbors_single(struct pim_instance *pim, struct vty *vty,
				      const char *neighbor, bool uj)
{
	struct listnode *neighnode;
	struct interface *ifp;
	struct pim_interface *pim_ifp;
	struct pim_neighbor *neigh;
	time_t now;
	int found_neighbor = 0;
	int option_address_list;
	int option_dr_priority;
	int option_generation_id;
	int option_holdtime;
	int option_lan_prune_delay;
	int option_t_bit;
	char uptime[10];
	char expire[10];
	char neigh_src_str[INET_ADDRSTRLEN];

	json_object *json = NULL;
	json_object *json_ifp = NULL;
	json_object *json_row = NULL;

	now = pim_time_monotonic_sec();

	if (uj)
		json = json_object_new_object();

	FOR_ALL_INTERFACES (pim->vrf, ifp) {
		pim_ifp = ifp->info;

		if (!pim_ifp)
			continue;

		if (pim_ifp->pim_sock_fd < 0)
			continue;

		for (ALL_LIST_ELEMENTS_RO(pim_ifp->pim_neighbor_list, neighnode,
					  neigh)) {
			pim_inet4_dump("<src?>", neigh->source_addr,
				       neigh_src_str, sizeof(neigh_src_str));

			/*
			 * The user can specify either the interface name or the
			 * PIM neighbor IP.
			 * If this pim_ifp matches neither then skip.
			 */
			if (strcmp(neighbor, "detail")
			    && strcmp(neighbor, ifp->name)
			    && strcmp(neighbor, neigh_src_str))
				continue;

			found_neighbor = 1;
			pim_time_uptime(uptime, sizeof(uptime),
					now - neigh->creation);
			pim_time_timer_to_hhmmss(expire, sizeof(expire),
						 neigh->t_expire_timer);

			option_address_list = 0;
			option_dr_priority = 0;
			option_generation_id = 0;
			option_holdtime = 0;
			option_lan_prune_delay = 0;
			option_t_bit = 0;

			if (PIM_OPTION_IS_SET(neigh->hello_options,
					      PIM_OPTION_MASK_ADDRESS_LIST))
				option_address_list = 1;

			if (PIM_OPTION_IS_SET(neigh->hello_options,
					      PIM_OPTION_MASK_DR_PRIORITY))
				option_dr_priority = 1;

			if (PIM_OPTION_IS_SET(neigh->hello_options,
					      PIM_OPTION_MASK_GENERATION_ID))
				option_generation_id = 1;

			if (PIM_OPTION_IS_SET(neigh->hello_options,
					      PIM_OPTION_MASK_HOLDTIME))
				option_holdtime = 1;

			if (PIM_OPTION_IS_SET(neigh->hello_options,
					      PIM_OPTION_MASK_LAN_PRUNE_DELAY))
				option_lan_prune_delay = 1;

			if (PIM_OPTION_IS_SET(
				    neigh->hello_options,
				    PIM_OPTION_MASK_CAN_DISABLE_JOIN_SUPPRESSION))
				option_t_bit = 1;

			if (uj) {

				/* Does this ifp live in json?  If not create
				 * it. */
				json_object_object_get_ex(json, ifp->name,
							  &json_ifp);

				if (!json_ifp) {
					json_ifp = json_object_new_object();
					json_object_pim_ifp_add(json_ifp, ifp);
					json_object_object_add(json, ifp->name,
							       json_ifp);
				}

				json_row = json_object_new_object();
				json_object_string_add(json_row, "interface",
						       ifp->name);
				json_object_string_add(json_row, "address",
						       neigh_src_str);
				json_object_string_add(json_row, "upTime",
						       uptime);
				json_object_string_add(json_row, "holdtime",
						       expire);
				json_object_int_add(json_row, "drPriority",
						    neigh->dr_priority);
				json_object_int_add(json_row, "generationId",
						    neigh->generation_id);

				if (option_address_list)
					json_object_boolean_true_add(
						json_row,
						"helloOptionAddressList");

				if (option_dr_priority)
					json_object_boolean_true_add(
						json_row,
						"helloOptionDrPriority");

				if (option_generation_id)
					json_object_boolean_true_add(
						json_row,
						"helloOptionGenerationId");

				if (option_holdtime)
					json_object_boolean_true_add(
						json_row,
						"helloOptionHoldtime");

				if (option_lan_prune_delay)
					json_object_boolean_true_add(
						json_row,
						"helloOptionLanPruneDelay");

				if (option_t_bit)
					json_object_boolean_true_add(
						json_row, "helloOptionTBit");

				json_object_object_add(json_ifp, neigh_src_str,
						       json_row);

			} else {
				vty_out(vty, "Interface : %s\n", ifp->name);
				vty_out(vty, "Neighbor  : %s\n", neigh_src_str);
				vty_out(vty,
					"    Uptime                         : %s\n",
					uptime);
				vty_out(vty,
					"    Holdtime                       : %s\n",
					expire);
				vty_out(vty,
					"    DR Priority                    : %d\n",
					neigh->dr_priority);
				vty_out(vty,
					"    Generation ID                  : %08x\n",
					neigh->generation_id);
				vty_out(vty,
					"    Override Interval (msec)       : %d\n",
					neigh->override_interval_msec);
				vty_out(vty,
					"    Propagation Delay (msec)       : %d\n",
					neigh->propagation_delay_msec);
				vty_out(vty,
					"    Hello Option - Address List    : %s\n",
					option_address_list ? "yes" : "no");
				vty_out(vty,
					"    Hello Option - DR Priority     : %s\n",
					option_dr_priority ? "yes" : "no");
				vty_out(vty,
					"    Hello Option - Generation ID   : %s\n",
					option_generation_id ? "yes" : "no");
				vty_out(vty,
					"    Hello Option - Holdtime        : %s\n",
					option_holdtime ? "yes" : "no");
				vty_out(vty,
					"    Hello Option - LAN Prune Delay : %s\n",
					option_lan_prune_delay ? "yes" : "no");
				vty_out(vty,
					"    Hello Option - T-bit           : %s\n",
					option_t_bit ? "yes" : "no");
				bfd_sess_show(vty, json_ifp,
					      neigh->bfd_session);
				vty_out(vty, "\n");
			}
		}
	}

	if (uj)
		vty_json(vty, json);
	else if (!found_neighbor)
		vty_out(vty, "%% No such interface or neighbor\n");
}

static void pim_show_state(struct pim_instance *pim, struct vty *vty,
			   const char *src_or_group, const char *group, bool uj)
{
	struct channel_oil *c_oil;
	json_object *json = NULL;
	json_object *json_group = NULL;
	json_object *json_ifp_in = NULL;
	json_object *json_ifp_out = NULL;
	json_object *json_source = NULL;
	time_t now;
	int first_oif;
	now = pim_time_monotonic_sec();

	if (uj) {
		json = json_object_new_object();
	} else {
		vty_out(vty,
			"Codes: J -> Pim Join, I -> IGMP Report, S -> Source, * -> Inherited from (*,G), V -> VxLAN, M -> Muted");
		vty_out(vty,
			"\nActive Source           Group            RPT  IIF               OIL\n");
	}

	frr_each (rb_pim_oil, &pim->channel_oil_head, c_oil) {
		char grp_str[INET_ADDRSTRLEN];
		char src_str[INET_ADDRSTRLEN];
		char in_ifname[INTERFACE_NAMSIZ + 1];
		char out_ifname[INTERFACE_NAMSIZ + 1];
		int oif_vif_index;
		struct interface *ifp_in;
		bool isRpt;
		first_oif = 1;

		if ((c_oil->up &&
		     PIM_UPSTREAM_FLAG_TEST_USE_RPT(c_oil->up->flags)) ||
		    c_oil->oil.mfcc_origin.s_addr == INADDR_ANY)
			isRpt = true;
		else
			isRpt = false;

		pim_inet4_dump("<group?>", c_oil->oil.mfcc_mcastgrp, grp_str,
			       sizeof(grp_str));
		pim_inet4_dump("<source?>", c_oil->oil.mfcc_origin, src_str,
			       sizeof(src_str));
		ifp_in = pim_if_find_by_vif_index(pim, c_oil->oil.mfcc_parent);

		if (ifp_in)
			strlcpy(in_ifname, ifp_in->name, sizeof(in_ifname));
		else
			strlcpy(in_ifname, "<iif?>", sizeof(in_ifname));

		if (src_or_group) {
			if (strcmp(src_or_group, src_str)
			    && strcmp(src_or_group, grp_str))
				continue;

			if (group && strcmp(group, grp_str))
				continue;
		}

		if (uj) {

			/* Find the group, create it if it doesn't exist */
			json_object_object_get_ex(json, grp_str, &json_group);

			if (!json_group) {
				json_group = json_object_new_object();
				json_object_object_add(json, grp_str,
						       json_group);
			}

			/* Find the source nested under the group, create it if
			 * it doesn't exist */
			json_object_object_get_ex(json_group, src_str,
						  &json_source);

			if (!json_source) {
				json_source = json_object_new_object();
				json_object_object_add(json_group, src_str,
						       json_source);
			}

			/* Find the inbound interface nested under the source,
			 * create it if it doesn't exist */
			json_object_object_get_ex(json_source, in_ifname,
						  &json_ifp_in);

			if (!json_ifp_in) {
				json_ifp_in = json_object_new_object();
				json_object_object_add(json_source, in_ifname,
						       json_ifp_in);
				json_object_int_add(json_source, "Installed",
						    c_oil->installed);
				if (isRpt)
					json_object_boolean_true_add(
						json_source, "isRpt");
				else
					json_object_boolean_false_add(
						json_source, "isRpt");
				json_object_int_add(json_source, "RefCount",
						    c_oil->oil_ref_count);
				json_object_int_add(json_source, "OilListSize",
						    c_oil->oil_size);
				json_object_int_add(
					json_source, "OilRescan",
					c_oil->oil_inherited_rescan);
				json_object_int_add(json_source, "LastUsed",
						    c_oil->cc.lastused);
				json_object_int_add(json_source, "PacketCount",
						    c_oil->cc.pktcnt);
				json_object_int_add(json_source, "ByteCount",
						    c_oil->cc.bytecnt);
				json_object_int_add(json_source,
						    "WrongInterface",
						    c_oil->cc.wrong_if);
			}
		} else {
			vty_out(vty, "%-6d %-15s  %-15s  %-3s  %-16s  ",
				c_oil->installed, src_str, grp_str,
				isRpt ? "y" : "n", in_ifname);
		}

		for (oif_vif_index = 0; oif_vif_index < MAXVIFS;
		     ++oif_vif_index) {
			struct interface *ifp_out;
			char oif_uptime[10];
			int ttl;

			ttl = c_oil->oil.mfcc_ttls[oif_vif_index];
			if (ttl < 1)
				continue;

			ifp_out = pim_if_find_by_vif_index(pim, oif_vif_index);
			pim_time_uptime(
				oif_uptime, sizeof(oif_uptime),
				now - c_oil->oif_creation[oif_vif_index]);

			if (ifp_out)
				strlcpy(out_ifname, ifp_out->name, sizeof(out_ifname));
			else
				strlcpy(out_ifname, "<oif?>", sizeof(out_ifname));

			if (uj) {
				json_ifp_out = json_object_new_object();
				json_object_string_add(json_ifp_out, "source",
						       src_str);
				json_object_string_add(json_ifp_out, "group",
						       grp_str);
				json_object_string_add(json_ifp_out,
						       "inboundInterface",
						       in_ifname);
				json_object_string_add(json_ifp_out,
						       "outboundInterface",
						       out_ifname);
				json_object_int_add(json_ifp_out, "installed",
						    c_oil->installed);

				json_object_object_add(json_ifp_in, out_ifname,
						       json_ifp_out);
			} else {
				if (first_oif) {
					first_oif = 0;
					vty_out(vty, "%s(%c%c%c%c%c)",
						out_ifname,
						(c_oil->oif_flags[oif_vif_index]
						 & PIM_OIF_FLAG_PROTO_IGMP)
						? 'I'
						: ' ',
						(c_oil->oif_flags[oif_vif_index]
						 & PIM_OIF_FLAG_PROTO_PIM)
						? 'J'
						: ' ',
						(c_oil->oif_flags[oif_vif_index]
						 & PIM_OIF_FLAG_PROTO_VXLAN)
						? 'V'
						: ' ',
						(c_oil->oif_flags[oif_vif_index]
						 & PIM_OIF_FLAG_PROTO_STAR)
						? '*'
						: ' ',
						(c_oil->oif_flags[oif_vif_index]
						 & PIM_OIF_FLAG_MUTE)
						? 'M'
						: ' ');
				} else
					vty_out(vty, ", %s(%c%c%c%c%c)",
						out_ifname,
						(c_oil->oif_flags[oif_vif_index]
						 & PIM_OIF_FLAG_PROTO_IGMP)
						? 'I'
						: ' ',
						(c_oil->oif_flags[oif_vif_index]
						 & PIM_OIF_FLAG_PROTO_PIM)
						? 'J'
						: ' ',
						(c_oil->oif_flags[oif_vif_index]
						 & PIM_OIF_FLAG_PROTO_VXLAN)
						? 'V'
						: ' ',
						(c_oil->oif_flags[oif_vif_index]
						 & PIM_OIF_FLAG_PROTO_STAR)
						? '*'
						: ' ',
						(c_oil->oif_flags[oif_vif_index]
						 & PIM_OIF_FLAG_MUTE)
						? 'M'
						: ' ');
			}
		}

		if (!uj)
			vty_out(vty, "\n");
	}


	if (uj)
		vty_json(vty, json);
	else
		vty_out(vty, "\n");
}

static void pim_show_neighbors(struct pim_instance *pim, struct vty *vty,
			       bool uj)
{
	struct listnode *neighnode;
	struct interface *ifp;
	struct pim_interface *pim_ifp;
	struct pim_neighbor *neigh;
	time_t now;
	char uptime[10];
	char expire[10];
	char neigh_src_str[INET_ADDRSTRLEN];
	json_object *json = NULL;
	json_object *json_ifp_rows = NULL;
	json_object *json_row = NULL;

	now = pim_time_monotonic_sec();

	if (uj) {
		json = json_object_new_object();
	} else {
		vty_out(vty,
			"Interface                Neighbor    Uptime  Holdtime  DR Pri\n");
	}

	FOR_ALL_INTERFACES (pim->vrf, ifp) {
		pim_ifp = ifp->info;

		if (!pim_ifp)
			continue;

		if (pim_ifp->pim_sock_fd < 0)
			continue;

		if (uj)
			json_ifp_rows = json_object_new_object();

		for (ALL_LIST_ELEMENTS_RO(pim_ifp->pim_neighbor_list, neighnode,
					  neigh)) {
			pim_inet4_dump("<src?>", neigh->source_addr,
				       neigh_src_str, sizeof(neigh_src_str));
			pim_time_uptime(uptime, sizeof(uptime),
					now - neigh->creation);
			pim_time_timer_to_hhmmss(expire, sizeof(expire),
						 neigh->t_expire_timer);

			if (uj) {
				json_row = json_object_new_object();
				json_object_string_add(json_row, "interface",
						       ifp->name);
				json_object_string_add(json_row, "neighbor",
						       neigh_src_str);
				json_object_string_add(json_row, "upTime",
						       uptime);
				json_object_string_add(json_row, "holdTime",
						       expire);
				json_object_int_add(json_row, "holdTimeMax",
						    neigh->holdtime);
				json_object_int_add(json_row, "drPriority",
						    neigh->dr_priority);
				json_object_object_add(json_ifp_rows,
						       neigh_src_str, json_row);

			} else {
				vty_out(vty, "%-16s  %15s  %8s  %8s  %6d\n",
					ifp->name, neigh_src_str, uptime,
					expire, neigh->dr_priority);
			}
		}

		if (uj) {
			json_object_object_add(json, ifp->name, json_ifp_rows);
			json_ifp_rows = NULL;
		}
	}

	if (uj)
		vty_json(vty, json);
}

static void pim_show_neighbors_secondary(struct pim_instance *pim,
					 struct vty *vty)
{
	struct interface *ifp;

	vty_out(vty,
		"Interface        Address         Neighbor        Secondary      \n");

	FOR_ALL_INTERFACES (pim->vrf, ifp) {
		struct pim_interface *pim_ifp;
		struct in_addr ifaddr;
		struct listnode *neighnode;
		struct pim_neighbor *neigh;
		char buf[PREFIX_STRLEN];

		pim_ifp = ifp->info;

		if (!pim_ifp)
			continue;

		if (pim_ifp->pim_sock_fd < 0)
			continue;

		ifaddr = pim_ifp->primary_address;

		for (ALL_LIST_ELEMENTS_RO(pim_ifp->pim_neighbor_list, neighnode,
					  neigh)) {
			char neigh_src_str[INET_ADDRSTRLEN];
			struct listnode *prefix_node;
			struct prefix *p;

			if (!neigh->prefix_list)
				continue;

			pim_inet4_dump("<src?>", neigh->source_addr,
				       neigh_src_str, sizeof(neigh_src_str));

			for (ALL_LIST_ELEMENTS_RO(neigh->prefix_list,
						  prefix_node, p))
				vty_out(vty, "%-16s %-15s %-15s %-15pFX\n",
					ifp->name,
					inet_ntop(AF_INET, &ifaddr,
						  buf, sizeof(buf)),
					neigh_src_str, p);
		}
	}
}

static void json_object_pim_upstream_add(json_object *json,
					 struct pim_upstream *up)
{
	if (up->flags & PIM_UPSTREAM_FLAG_MASK_DR_JOIN_DESIRED)
		json_object_boolean_true_add(json, "drJoinDesired");

	if (up->flags & PIM_UPSTREAM_FLAG_MASK_DR_JOIN_DESIRED_UPDATED)
		json_object_boolean_true_add(json, "drJoinDesiredUpdated");

	if (up->flags & PIM_UPSTREAM_FLAG_MASK_FHR)
		json_object_boolean_true_add(json, "firstHopRouter");

	if (up->flags & PIM_UPSTREAM_FLAG_MASK_SRC_IGMP)
		json_object_boolean_true_add(json, "sourceIgmp");

	if (up->flags & PIM_UPSTREAM_FLAG_MASK_SRC_PIM)
		json_object_boolean_true_add(json, "sourcePim");

	if (up->flags & PIM_UPSTREAM_FLAG_MASK_SRC_STREAM)
		json_object_boolean_true_add(json, "sourceStream");

	/* XXX: need to print ths flag in the plain text display as well */
	if (up->flags & PIM_UPSTREAM_FLAG_MASK_SRC_MSDP)
		json_object_boolean_true_add(json, "sourceMsdp");

	if (up->flags & PIM_UPSTREAM_FLAG_MASK_SEND_SG_RPT_PRUNE)
		json_object_boolean_true_add(json, "sendSGRptPrune");

	if (up->flags & PIM_UPSTREAM_FLAG_MASK_SRC_LHR)
		json_object_boolean_true_add(json, "lastHopRouter");

	if (up->flags & PIM_UPSTREAM_FLAG_MASK_DISABLE_KAT_EXPIRY)
		json_object_boolean_true_add(json, "disableKATExpiry");

	if (up->flags & PIM_UPSTREAM_FLAG_MASK_STATIC_IIF)
		json_object_boolean_true_add(json, "staticIncomingInterface");

	if (up->flags & PIM_UPSTREAM_FLAG_MASK_ALLOW_IIF_IN_OIL)
		json_object_boolean_true_add(json,
					     "allowIncomingInterfaceinOil");

	if (up->flags & PIM_UPSTREAM_FLAG_MASK_NO_PIMREG_DATA)
		json_object_boolean_true_add(json, "noPimRegistrationData");

	if (up->flags & PIM_UPSTREAM_FLAG_MASK_FORCE_PIMREG)
		json_object_boolean_true_add(json, "forcePimRegistration");

	if (up->flags & PIM_UPSTREAM_FLAG_MASK_SRC_VXLAN_ORIG)
		json_object_boolean_true_add(json, "sourceVxlanOrigination");

	if (up->flags & PIM_UPSTREAM_FLAG_MASK_SRC_VXLAN_TERM)
		json_object_boolean_true_add(json, "sourceVxlanTermination");

	if (up->flags & PIM_UPSTREAM_FLAG_MASK_MLAG_VXLAN)
		json_object_boolean_true_add(json, "mlagVxlan");

	if (up->flags & PIM_UPSTREAM_FLAG_MASK_MLAG_NON_DF)
		json_object_boolean_true_add(json,
					     "mlagNonDesignatedForwarder");
}

static const char *
pim_upstream_state2brief_str(enum pim_upstream_state join_state,
			     char *state_str, size_t state_str_len)
{
	switch (join_state) {
	case PIM_UPSTREAM_NOTJOINED:
		strlcpy(state_str, "NotJ", state_str_len);
		break;
	case PIM_UPSTREAM_JOINED:
		strlcpy(state_str, "J", state_str_len);
		break;
	default:
		strlcpy(state_str, "Unk", state_str_len);
	}
	return state_str;
}

static const char *pim_reg_state2brief_str(enum pim_reg_state reg_state,
					   char *state_str, size_t state_str_len)
{
	switch (reg_state) {
	case PIM_REG_NOINFO:
		strlcpy(state_str, "RegNI", state_str_len);
		break;
	case PIM_REG_JOIN:
		strlcpy(state_str, "RegJ", state_str_len);
		break;
	case PIM_REG_JOIN_PENDING:
	case PIM_REG_PRUNE:
		strlcpy(state_str, "RegP", state_str_len);
		break;
	}
	return state_str;
}

static void pim_show_upstream(struct pim_instance *pim, struct vty *vty,
			      pim_sgaddr *sg, bool uj)
{
	struct pim_upstream *up;
	time_t now;
	json_object *json = NULL;
	json_object *json_group = NULL;
	json_object *json_row = NULL;

	now = pim_time_monotonic_sec();

	if (uj)
		json = json_object_new_object();
	else
		vty_out(vty,
			"Iif             Source          Group           State       Uptime   JoinTimer RSTimer   KATimer   RefCnt\n");

	frr_each (rb_pim_upstream, &pim->upstream_head, up) {
		char uptime[10];
		char join_timer[10];
		char rs_timer[10];
		char ka_timer[10];
		char msdp_reg_timer[10];
		char state_str[PIM_REG_STATE_STR_LEN];

		if (!pim_sgaddr_match(up->sg, *sg))
			continue;

		pim_time_uptime(uptime, sizeof(uptime),
				now - up->state_transition);
		pim_time_timer_to_hhmmss(join_timer, sizeof(join_timer),
					 up->t_join_timer);

		/*
		 * If the upstream is not dummy and it has a J/P timer for the
		 * neighbor display that
		 */
		if (!up->t_join_timer && up->rpf.source_nexthop.interface) {
			struct pim_neighbor *nbr;

			nbr = pim_neighbor_find(
				up->rpf.source_nexthop.interface,
				up->rpf.rpf_addr.u.prefix4);
			if (nbr)
				pim_time_timer_to_hhmmss(join_timer,
							 sizeof(join_timer),
							 nbr->jp_timer);
		}

		pim_time_timer_to_hhmmss(rs_timer, sizeof(rs_timer),
					 up->t_rs_timer);
		pim_time_timer_to_hhmmss(ka_timer, sizeof(ka_timer),
					 up->t_ka_timer);
		pim_time_timer_to_hhmmss(msdp_reg_timer, sizeof(msdp_reg_timer),
					 up->t_msdp_reg_timer);

		pim_upstream_state2brief_str(up->join_state, state_str, sizeof(state_str));
		if (up->reg_state != PIM_REG_NOINFO) {
			char tmp_str[PIM_REG_STATE_STR_LEN];
			char tmp[sizeof(state_str) + 1];

			snprintf(tmp, sizeof(tmp), ",%s",
				 pim_reg_state2brief_str(up->reg_state, tmp_str,
							 sizeof(tmp_str)));
			strlcat(state_str, tmp, sizeof(state_str));
		}

		if (uj) {
			char grp_str[PIM_ADDRSTRLEN];
			char src_str[PIM_ADDRSTRLEN];

			snprintfrr(grp_str, sizeof(grp_str), "%pPAs",
				   &up->sg.grp);
			snprintfrr(src_str, sizeof(src_str), "%pPAs",
				   &up->sg.src);

			json_object_object_get_ex(json, grp_str, &json_group);

			if (!json_group) {
				json_group = json_object_new_object();
				json_object_object_add(json, grp_str,
						       json_group);
			}

			json_row = json_object_new_object();
			json_object_pim_upstream_add(json_row, up);
			json_object_string_add(
				json_row, "inboundInterface",
				up->rpf.source_nexthop.interface
				? up->rpf.source_nexthop.interface->name
				: "Unknown");

			/*
			 * The RPF address we use is slightly different
			 * based upon what we are looking up.
			 * If we have a S, list that unless
			 * we are the FHR, else we just put
			 * the RP as the rpfAddress
			 */
			if (up->flags & PIM_UPSTREAM_FLAG_MASK_FHR ||
			    pim_addr_is_any(up->sg.src)) {
				char rpf[PREFIX_STRLEN];
				struct pim_rpf *rpg;

				rpg = RP(pim, up->sg.grp);
				pim_inet4_dump("<rpf?>",
					       rpg->rpf_addr.u.prefix4, rpf,
					       sizeof(rpf));
				json_object_string_add(json_row, "rpfAddress",
						       rpf);
			} else {
				json_object_string_add(json_row, "rpfAddress",
						       src_str);
			}

			json_object_string_add(json_row, "source", src_str);
			json_object_string_add(json_row, "group", grp_str);
			json_object_string_add(json_row, "state", state_str);
			json_object_string_add(
				json_row, "joinState",
				pim_upstream_state2str(up->join_state));
			json_object_string_add(
				json_row, "regState",
				pim_reg_state2str(up->reg_state, state_str, sizeof(state_str)));
			json_object_string_add(json_row, "upTime", uptime);
			json_object_string_add(json_row, "joinTimer",
					       join_timer);
			json_object_string_add(json_row, "resetTimer",
					       rs_timer);
			json_object_string_add(json_row, "keepaliveTimer",
					       ka_timer);
			json_object_string_add(json_row, "msdpRegTimer",
					       msdp_reg_timer);
			json_object_int_add(json_row, "refCount",
					    up->ref_count);
			json_object_int_add(json_row, "sptBit", up->sptbit);
			json_object_object_add(json_group, src_str, json_row);
		} else {
			vty_out(vty,
				"%-16s%-15pPAs %-15pPAs %-11s %-8s %-9s %-9s %-9s %6d\n",
				up->rpf.source_nexthop.interface
				? up->rpf.source_nexthop.interface->name
				: "Unknown",
				&up->sg.src, &up->sg.grp, state_str, uptime,
				join_timer, rs_timer, ka_timer, up->ref_count);
		}
	}

	if (uj)
		vty_json(vty, json);
}

static void pim_show_channel_helper(struct pim_instance *pim,
				    struct vty *vty,
				    struct pim_interface *pim_ifp,
				    struct pim_ifchannel *ch,
				    json_object *json, bool uj)
{
	struct pim_upstream *up = ch->upstream;
	json_object *json_group = NULL;
	json_object *json_row = NULL;

	if (uj) {
		char grp_str[PIM_ADDRSTRLEN];
		char src_str[PIM_ADDRSTRLEN];

		snprintfrr(grp_str, sizeof(grp_str), "%pPAs", &up->sg.grp);
		snprintfrr(src_str, sizeof(src_str), "%pPAs", &up->sg.src);

		json_object_object_get_ex(json, grp_str, &json_group);

		if (!json_group) {
			json_group = json_object_new_object();
			json_object_object_add(json, grp_str, json_group);
		}

		json_row = json_object_new_object();
		json_object_pim_upstream_add(json_row, up);
		json_object_string_add(json_row, "interface",
				       ch->interface->name);
		json_object_string_add(json_row, "source", src_str);
		json_object_string_add(json_row, "group", grp_str);

		if (pim_macro_ch_lost_assert(ch))
			json_object_boolean_true_add(json_row, "lostAssert");

		if (pim_macro_chisin_joins(ch))
			json_object_boolean_true_add(json_row, "joins");

		if (pim_macro_chisin_pim_include(ch))
			json_object_boolean_true_add(json_row, "pimInclude");

		if (pim_upstream_evaluate_join_desired(pim, up))
			json_object_boolean_true_add(json_row,
						     "evaluateJoinDesired");

		json_object_object_add(json_group, src_str, json_row);

	} else {
		vty_out(vty, "%-16s %-15pPAs %-15pPAs %-10s %-5s %-10s %-11s %-6s\n",
			ch->interface->name, &up->sg.src, &up->sg.grp,
			pim_macro_ch_lost_assert(ch) ? "yes" : "no",
			pim_macro_chisin_joins(ch) ? "yes" : "no",
			pim_macro_chisin_pim_include(ch) ? "yes" : "no",
			PIM_UPSTREAM_FLAG_TEST_DR_JOIN_DESIRED(up->flags)
			? "yes"
			: "no",
			pim_upstream_evaluate_join_desired(pim, up) ? "yes"
			: "no");
	}
}

static void pim_show_channel(struct pim_instance *pim, struct vty *vty,
			     bool uj)
{
	struct pim_interface *pim_ifp;
	struct pim_ifchannel *ch;
	struct interface *ifp;

	json_object *json = NULL;

	if (uj)
		json = json_object_new_object();
	else
		vty_out(vty,
			"Interface        Source          Group           LostAssert Joins PimInclude JoinDesired EvalJD\n");

	/* scan per-interface (S,G) state */
	FOR_ALL_INTERFACES (pim->vrf, ifp) {
		pim_ifp = ifp->info;
		if (!pim_ifp)
			continue;


		RB_FOREACH (ch, pim_ifchannel_rb, &pim_ifp->ifchannel_rb) {
			/* scan all interfaces */
			pim_show_channel_helper(pim, vty, pim_ifp, ch,
						json, uj);
		}
	}

	if (uj)
		vty_json(vty, json);
}

static void pim_show_join_desired_helper(struct pim_instance *pim,
					 struct vty *vty,
					 struct pim_upstream *up,
					 json_object *json, bool uj)
{
	json_object *json_group = NULL;
	json_object *json_row = NULL;

	if (uj) {
		char grp_str[PIM_ADDRSTRLEN];
		char src_str[PIM_ADDRSTRLEN];

		snprintfrr(grp_str, sizeof(grp_str), "%pPAs", &up->sg.grp);
		snprintfrr(src_str, sizeof(src_str), "%pPAs", &up->sg.src);

		json_object_object_get_ex(json, grp_str, &json_group);

		if (!json_group) {
			json_group = json_object_new_object();
			json_object_object_add(json, grp_str, json_group);
		}

		json_row = json_object_new_object();
		json_object_pim_upstream_add(json_row, up);
		json_object_string_add(json_row, "source", src_str);
		json_object_string_add(json_row, "group", grp_str);

		if (pim_upstream_evaluate_join_desired(pim, up))
			json_object_boolean_true_add(json_row,
						     "evaluateJoinDesired");

		json_object_object_add(json_group, src_str, json_row);

	} else {
		vty_out(vty, "%-15pPAs %-15pPAs %-6s\n",
			&up->sg.src, &up->sg.grp,
			pim_upstream_evaluate_join_desired(pim, up) ? "yes"
			: "no");
	}
}

static void pim_show_join_desired(struct pim_instance *pim, struct vty *vty,
				  bool uj)
{
	struct pim_upstream *up;

	json_object *json = NULL;

	if (uj)
		json = json_object_new_object();
	else
		vty_out(vty,
			"Source          Group           EvalJD\n");

	frr_each (rb_pim_upstream, &pim->upstream_head, up) {
		/* scan all interfaces */
		pim_show_join_desired_helper(pim, vty, up,
					     json, uj);
	}

	if (uj)
		vty_json(vty, json);
}

static void pim_show_upstream_rpf(struct pim_instance *pim, struct vty *vty,
				  bool uj)
{
	struct pim_upstream *up;
	json_object *json = NULL;
	json_object *json_group = NULL;
	json_object *json_row = NULL;

	if (uj)
		json = json_object_new_object();
	else
		vty_out(vty,
			"Source          Group           RpfIface         RibNextHop      RpfAddress     \n");

	frr_each (rb_pim_upstream, &pim->upstream_head, up) {
		char rpf_nexthop_str[PREFIX_STRLEN];
		char rpf_addr_str[PREFIX_STRLEN];
		struct pim_rpf *rpf;
		const char *rpf_ifname;

		rpf = &up->rpf;

		pim_addr_dump("<nexthop?>",
			      &rpf->source_nexthop.mrib_nexthop_addr,
			      rpf_nexthop_str, sizeof(rpf_nexthop_str));
		pim_addr_dump("<rpf?>", &rpf->rpf_addr, rpf_addr_str,
			      sizeof(rpf_addr_str));

		rpf_ifname = rpf->source_nexthop.interface ? rpf->source_nexthop.interface->name : "<ifname?>";

		if (uj) {
			char grp_str[PIM_ADDRSTRLEN];
			char src_str[PIM_ADDRSTRLEN];

			snprintfrr(grp_str, sizeof(grp_str), "%pPAs",
				   &up->sg.grp);
			snprintfrr(src_str, sizeof(src_str), "%pPAs",
				   &up->sg.src);

			json_object_object_get_ex(json, grp_str, &json_group);

			if (!json_group) {
				json_group = json_object_new_object();
				json_object_object_add(json, grp_str,
						       json_group);
			}

			json_row = json_object_new_object();
			json_object_pim_upstream_add(json_row, up);
			json_object_string_add(json_row, "source", src_str);
			json_object_string_add(json_row, "group", grp_str);
			json_object_string_add(json_row, "rpfInterface",
					       rpf_ifname);
			json_object_string_add(json_row, "ribNexthop",
					       rpf_nexthop_str);
			json_object_string_add(json_row, "rpfAddress",
					       rpf_addr_str);
			json_object_object_add(json_group, src_str, json_row);
		} else {
			vty_out(vty, "%-15pPAs %-15pPAs %-16s %-15s %-15s\n",
				&up->sg.src, &up->sg.grp, rpf_ifname,
				rpf_nexthop_str, rpf_addr_str);
		}
	}

	if (uj)
		vty_json(vty, json);
}

static void show_rpf_refresh_stats(struct vty *vty, struct pim_instance *pim,
				   time_t now, json_object *json)
{
	char refresh_uptime[10];

	pim_time_uptime_begin(refresh_uptime, sizeof(refresh_uptime), now,
			      pim->rpf_cache_refresh_last);

	if (json) {
		json_object_int_add(json, "rpfCacheRefreshDelayMsecs",
				    router->rpf_cache_refresh_delay_msec);
		json_object_int_add(
			json, "rpfCacheRefreshTimer",
			pim_time_timer_remain_msec(pim->rpf_cache_refresher));
		json_object_int_add(json, "rpfCacheRefreshRequests",
				    pim->rpf_cache_refresh_requests);
		json_object_int_add(json, "rpfCacheRefreshEvents",
				    pim->rpf_cache_refresh_events);
		json_object_string_add(json, "rpfCacheRefreshLast",
				       refresh_uptime);
		json_object_int_add(json, "nexthopLookups",
				    pim->nexthop_lookups);
		json_object_int_add(json, "nexthopLookupsAvoided",
				    pim->nexthop_lookups_avoided);
	} else {
		vty_out(vty,
			"RPF Cache Refresh Delay:    %ld msecs\n"
			"RPF Cache Refresh Timer:    %ld msecs\n"
			"RPF Cache Refresh Requests: %lld\n"
			"RPF Cache Refresh Events:   %lld\n"
			"RPF Cache Refresh Last:     %s\n"
			"Nexthop Lookups:            %lld\n"
			"Nexthop Lookups Avoided:    %lld\n",
			router->rpf_cache_refresh_delay_msec,
			pim_time_timer_remain_msec(pim->rpf_cache_refresher),
			(long long)pim->rpf_cache_refresh_requests,
			(long long)pim->rpf_cache_refresh_events,
			refresh_uptime, (long long)pim->nexthop_lookups,
			(long long)pim->nexthop_lookups_avoided);
	}
}

static void show_scan_oil_stats(struct pim_instance *pim, struct vty *vty,
				time_t now)
{
	char uptime_scan_oil[10];
	char uptime_mroute_add[10];
	char uptime_mroute_del[10];

	pim_time_uptime_begin(uptime_scan_oil, sizeof(uptime_scan_oil), now,
			      pim->scan_oil_last);
	pim_time_uptime_begin(uptime_mroute_add, sizeof(uptime_mroute_add), now,
			      pim->mroute_add_last);
	pim_time_uptime_begin(uptime_mroute_del, sizeof(uptime_mroute_del), now,
			      pim->mroute_del_last);

	vty_out(vty,
		"Scan OIL - Last: %s  Events: %lld\n"
		"MFC Add  - Last: %s  Events: %lld\n"
		"MFC Del  - Last: %s  Events: %lld\n",
		uptime_scan_oil, (long long)pim->scan_oil_events,
		uptime_mroute_add, (long long)pim->mroute_add_events,
		uptime_mroute_del, (long long)pim->mroute_del_events);
}

static void pim_show_rpf(struct pim_instance *pim, struct vty *vty, bool uj)
{
	struct pim_upstream *up;
	time_t now = pim_time_monotonic_sec();
	json_object *json = NULL;
	json_object *json_group = NULL;
	json_object *json_row = NULL;

	if (uj) {
		json = json_object_new_object();
		show_rpf_refresh_stats(vty, pim, now, json);
	} else {
		show_rpf_refresh_stats(vty, pim, now, json);
		vty_out(vty, "\n");
		vty_out(vty,
			"Source          Group           RpfIface         RpfAddress      RibNextHop      Metric Pref\n");
	}

	frr_each (rb_pim_upstream, &pim->upstream_head, up) {
		char rpf_addr_str[PREFIX_STRLEN];
		char rib_nexthop_str[PREFIX_STRLEN];
		const char *rpf_ifname;
		struct pim_rpf *rpf = &up->rpf;

		pim_addr_dump("<rpf?>", &rpf->rpf_addr, rpf_addr_str,
			      sizeof(rpf_addr_str));
		pim_addr_dump("<nexthop?>",
			      &rpf->source_nexthop.mrib_nexthop_addr,
			      rib_nexthop_str, sizeof(rib_nexthop_str));

		rpf_ifname = rpf->source_nexthop.interface ? rpf->source_nexthop.interface->name : "<ifname?>";

		if (uj) {
			char grp_str[PIM_ADDRSTRLEN];
			char src_str[PIM_ADDRSTRLEN];

			snprintfrr(grp_str, sizeof(grp_str), "%pPAs",
				   &up->sg.grp);
			snprintfrr(src_str, sizeof(src_str), "%pPAs",
				   &up->sg.src);

			json_object_object_get_ex(json, grp_str, &json_group);

			if (!json_group) {
				json_group = json_object_new_object();
				json_object_object_add(json, grp_str,
						       json_group);
			}

			json_row = json_object_new_object();
			json_object_string_add(json_row, "source", src_str);
			json_object_string_add(json_row, "group", grp_str);
			json_object_string_add(json_row, "rpfInterface",
					       rpf_ifname);
			json_object_string_add(json_row, "rpfAddress",
					       rpf_addr_str);
			json_object_string_add(json_row, "ribNexthop",
					       rib_nexthop_str);
			json_object_int_add(
				json_row, "routeMetric",
				rpf->source_nexthop.mrib_route_metric);
			json_object_int_add(
				json_row, "routePreference",
				rpf->source_nexthop.mrib_metric_preference);
			json_object_object_add(json_group, src_str, json_row);

		} else {
			vty_out(vty, "%-15pPAs %-15pPAs %-16s %-15s %-15s %6d %4d\n",
				&up->sg.src, &up->sg.grp, rpf_ifname,
				rpf_addr_str, rib_nexthop_str,
				rpf->source_nexthop.mrib_route_metric,
				rpf->source_nexthop.mrib_metric_preference);
		}
	}

	if (uj)
		vty_json(vty, json);
}

struct pnc_cache_walk_data {
	struct vty *vty;
	struct pim_instance *pim;
};

static int pim_print_pnc_cache_walkcb(struct hash_bucket *bucket, void *arg)
{
	struct pim_nexthop_cache *pnc = bucket->data;
	struct pnc_cache_walk_data *cwd = arg;
	struct vty *vty = cwd->vty;
	struct pim_instance *pim = cwd->pim;
	struct nexthop *nh_node = NULL;
	ifindex_t first_ifindex;
	struct interface *ifp = NULL;
	char buf[PREFIX_STRLEN];

	for (nh_node = pnc->nexthop; nh_node; nh_node = nh_node->next) {
		first_ifindex = nh_node->ifindex;
		ifp = if_lookup_by_index(first_ifindex, pim->vrf->vrf_id);

		vty_out(vty, "%-15s ", inet_ntop(AF_INET,
						 &pnc->rpf.rpf_addr.u.prefix4,
						 buf, sizeof(buf)));
		vty_out(vty, "%-16s ", ifp ? ifp->name : "NULL");
		vty_out(vty, "%pI4 ", &nh_node->gate.ipv4);
		vty_out(vty, "\n");
	}
	return CMD_SUCCESS;
}

static void pim_show_nexthop(struct pim_instance *pim, struct vty *vty)
{
	struct pnc_cache_walk_data cwd;

	cwd.vty = vty;
	cwd.pim = pim;
	vty_out(vty, "Number of registered addresses: %lu\n",
		pim->rpf_hash->count);
	vty_out(vty, "Address         Interface        Nexthop\n");
	vty_out(vty, "---------------------------------------------\n");

	hash_walk(pim->rpf_hash, pim_print_pnc_cache_walkcb, &cwd);
}

/* Display the bsm database details */
static void pim_show_bsm_db(struct pim_instance *pim, struct vty *vty, bool uj)
{
	int count = 0;
	int fragment = 1;
	struct bsm_frag *bsfrag;
	json_object *json = NULL;
	json_object *json_group = NULL;
	json_object *json_row = NULL;

	count = bsm_frags_count(pim->global_scope.bsm_frags);

	if (uj) {
		json = json_object_new_object();
		json_object_int_add(json, "Number of the fragments", count);
	} else {
		vty_out(vty, "Scope Zone: Global\n");
		vty_out(vty, "Number of the fragments: %d\n", count);
		vty_out(vty, "\n");
	}

	frr_each (bsm_frags, pim->global_scope.bsm_frags, bsfrag) {
		char grp_str[PREFIX_STRLEN];
		char rp_str[INET_ADDRSTRLEN];
		char bsr_str[INET_ADDRSTRLEN];
		struct bsmmsg_grpinfo *group;
		struct bsmmsg_rpinfo *rpaddr;
		struct prefix grp;
		struct bsm_hdr *hdr;
		uint32_t offset = 0;
		uint8_t *buf;
		uint32_t len = 0;
		uint32_t frag_rp_cnt = 0;

		buf = bsfrag->data;
		len = bsfrag->size;

		/* skip pim header */
		buf += PIM_MSG_HEADER_LEN;
		len -= PIM_MSG_HEADER_LEN;

		hdr = (struct bsm_hdr *)buf;

		/* BSM starts with bsr header */
		buf += sizeof(struct bsm_hdr);
		len -= sizeof(struct bsm_hdr);

		pim_inet4_dump("<BSR Address?>", hdr->bsr_addr.addr, bsr_str,
			       sizeof(bsr_str));


		if (uj) {
			json_object_string_add(json, "BSR address", bsr_str);
			json_object_int_add(json, "BSR priority",
					    hdr->bsr_prio);
			json_object_int_add(json, "Hashmask Length",
					    hdr->hm_len);
			json_object_int_add(json, "Fragment Tag",
					    ntohs(hdr->frag_tag));
		} else {
			vty_out(vty, "BSM Fragment : %d\n", fragment);
			vty_out(vty, "------------------\n");
			vty_out(vty, "%-15s %-15s %-15s %-15s\n", "BSR-Address",
				"BSR-Priority", "Hashmask-len", "Fragment-Tag");
			vty_out(vty, "%-15s %-15d %-15d %-15d\n", bsr_str,
				hdr->bsr_prio, hdr->hm_len,
				ntohs(hdr->frag_tag));
		}

		vty_out(vty, "\n");

		while (offset < len) {
			group = (struct bsmmsg_grpinfo *)buf;

			if (group->group.family == PIM_MSG_ADDRESS_FAMILY_IPV4)
				grp.family = AF_INET;

			grp.prefixlen = group->group.mask;
			grp.u.prefix4.s_addr = group->group.addr.s_addr;

			prefix2str(&grp, grp_str, sizeof(grp_str));

			buf += sizeof(struct bsmmsg_grpinfo);
			offset += sizeof(struct bsmmsg_grpinfo);

			if (uj) {
				json_object_object_get_ex(json, grp_str,
							  &json_group);
				if (!json_group) {
					json_group = json_object_new_object();
					json_object_int_add(json_group,
							    "Rp Count",
							    group->rp_count);
					json_object_int_add(
						json_group, "Fragment Rp count",
						group->frag_rp_count);
					json_object_object_add(json, grp_str,
							       json_group);
				}
			} else {
				vty_out(vty, "Group : %s\n", grp_str);
				vty_out(vty, "-------------------\n");
				vty_out(vty, "Rp Count:%d\n", group->rp_count);
				vty_out(vty, "Fragment Rp Count : %d\n",
					group->frag_rp_count);
			}

			frag_rp_cnt = group->frag_rp_count;

			if (!frag_rp_cnt)
				continue;

			if (!uj)
				vty_out(vty,
					"RpAddress     HoldTime     Priority\n");

			while (frag_rp_cnt--) {
				rpaddr = (struct bsmmsg_rpinfo *)buf;

				buf += sizeof(struct bsmmsg_rpinfo);
				offset += sizeof(struct bsmmsg_rpinfo);

				pim_inet4_dump("<Rp addr?>",
					       rpaddr->rpaddr.addr, rp_str,
					       sizeof(rp_str));

				if (uj) {
					json_row = json_object_new_object();
					json_object_string_add(
						json_row, "Rp Address", rp_str);
					json_object_int_add(
						json_row, "Rp HoldTime",
						ntohs(rpaddr->rp_holdtime));
					json_object_int_add(json_row,
							    "Rp Priority",
							    rpaddr->rp_pri);
					json_object_object_add(
						json_group, rp_str, json_row);
				} else {
					vty_out(vty, "%-15s %-12d %d\n", rp_str,
						ntohs(rpaddr->rp_holdtime),
						rpaddr->rp_pri);
				}
			}
			vty_out(vty, "\n");
		}

		fragment++;
	}

	if (uj)
		vty_json(vty, json);
}

/*Display the group-rp mappings */
static void pim_show_group_rp_mappings_info(struct pim_instance *pim,
					    struct vty *vty, bool uj)
{
	struct bsgrp_node *bsgrp;
	struct bsm_rpinfo *bsm_rp;
	struct route_node *rn;
	char bsr_str[INET_ADDRSTRLEN];
	json_object *json = NULL;
	json_object *json_group = NULL;
	json_object *json_row = NULL;

	if (pim->global_scope.current_bsr.s_addr == INADDR_ANY)
		strlcpy(bsr_str, "0.0.0.0", sizeof(bsr_str));

	else
		pim_inet4_dump("<bsr?>", pim->global_scope.current_bsr, bsr_str,
			       sizeof(bsr_str));

	if (uj) {
		json = json_object_new_object();
		json_object_string_add(json, "BSR Address", bsr_str);
	} else {
		vty_out(vty, "BSR Address  %s\n", bsr_str);
	}

	for (rn = route_top(pim->global_scope.bsrp_table); rn;
	     rn = route_next(rn)) {
		bsgrp = (struct bsgrp_node *)rn->info;

		if (!bsgrp)
			continue;

		char grp_str[PREFIX_STRLEN];

		prefix2str(&bsgrp->group, grp_str, sizeof(grp_str));

		if (uj) {
			json_object_object_get_ex(json, grp_str, &json_group);
			if (!json_group) {
				json_group = json_object_new_object();
				json_object_object_add(json, grp_str,
						       json_group);
			}
		} else {
			vty_out(vty, "Group Address %pFX\n", &bsgrp->group);
			vty_out(vty, "--------------------------\n");
			vty_out(vty, "%-15s %-15s %-15s %-15s\n", "Rp Address",
				"priority", "Holdtime", "Hash");

			vty_out(vty, "(ACTIVE)\n");
		}

		frr_each (bsm_rpinfos, bsgrp->bsrp_list, bsm_rp) {
			char rp_str[INET_ADDRSTRLEN];

			pim_inet4_dump("<Rp Address?>", bsm_rp->rp_address,
				       rp_str, sizeof(rp_str));

			if (uj) {
				json_row = json_object_new_object();
				json_object_string_add(json_row, "Rp Address",
						       rp_str);
				json_object_int_add(json_row, "Rp HoldTime",
						    bsm_rp->rp_holdtime);
				json_object_int_add(json_row, "Rp Priority",
						    bsm_rp->rp_prio);
				json_object_int_add(json_row, "Hash Val",
						    bsm_rp->hash);
				json_object_object_add(json_group, rp_str,
						       json_row);

			} else {
				vty_out(vty, "%-15s %-15u %-15u %-15u\n",
					rp_str, bsm_rp->rp_prio,
					bsm_rp->rp_holdtime, bsm_rp->hash);
			}
		}
		if (!bsm_rpinfos_count(bsgrp->bsrp_list) && !uj)
			vty_out(vty, "Active List is empty.\n");

		if (uj) {
			json_object_int_add(json_group, "Pending RP count",
					    bsgrp->pend_rp_cnt);
		} else {
			vty_out(vty, "(PENDING)\n");
			vty_out(vty, "Pending RP count :%d\n",
				bsgrp->pend_rp_cnt);
			if (bsgrp->pend_rp_cnt)
				vty_out(vty, "%-15s %-15s %-15s %-15s\n",
					"Rp Address", "priority", "Holdtime",
					"Hash");
		}

		frr_each (bsm_rpinfos, bsgrp->partial_bsrp_list, bsm_rp) {
			char rp_str[INET_ADDRSTRLEN];

			pim_inet4_dump("<Rp Addr?>", bsm_rp->rp_address, rp_str,
				       sizeof(rp_str));

			if (uj) {
				json_row = json_object_new_object();
				json_object_string_add(json_row, "Rp Address",
						       rp_str);
				json_object_int_add(json_row, "Rp HoldTime",
						    bsm_rp->rp_holdtime);
				json_object_int_add(json_row, "Rp Priority",
						    bsm_rp->rp_prio);
				json_object_int_add(json_row, "Hash Val",
						    bsm_rp->hash);
				json_object_object_add(json_group, rp_str,
						       json_row);
			} else {
				vty_out(vty, "%-15s %-15u %-15u %-15u\n",
					rp_str, bsm_rp->rp_prio,
					bsm_rp->rp_holdtime, bsm_rp->hash);
			}
		}
		if (!bsm_rpinfos_count(bsgrp->partial_bsrp_list) && !uj)
			vty_out(vty, "Partial List is empty\n");

		if (!uj)
			vty_out(vty, "\n");
	}

	if (uj)
		vty_json(vty, json);
}

/* pim statistics - just adding only bsm related now.
 * We can continue to add all pim related stats here.
 */
static void pim_show_statistics(struct pim_instance *pim, struct vty *vty,
				const char *ifname, bool uj)
{
	json_object *json = NULL;
	struct interface *ifp;

	if (uj) {
		json = json_object_new_object();
		json_object_int_add(json, "bsmRx", pim->bsm_rcvd);
		json_object_int_add(json, "bsmTx", pim->bsm_sent);
		json_object_int_add(json, "bsmDropped", pim->bsm_dropped);
	} else {
		vty_out(vty, "BSM Statistics :\n");
		vty_out(vty, "----------------\n");
		vty_out(vty, "Number of Received BSMs : %" PRIu64 "\n",
			pim->bsm_rcvd);
		vty_out(vty, "Number of Forwared BSMs : %" PRIu64 "\n",
			pim->bsm_sent);
		vty_out(vty, "Number of Dropped BSMs  : %" PRIu64 "\n",
			pim->bsm_dropped);
	}

	vty_out(vty, "\n");

	/* scan interfaces */
	FOR_ALL_INTERFACES (pim->vrf, ifp) {
		struct pim_interface *pim_ifp = ifp->info;

		if (ifname && strcmp(ifname, ifp->name))
			continue;

		if (!pim_ifp)
			continue;

		if (!uj) {
			vty_out(vty, "Interface : %s\n", ifp->name);
			vty_out(vty, "-------------------\n");
			vty_out(vty,
				"Number of BSMs dropped due to config miss : %u\n",
				pim_ifp->pim_ifstat_bsm_cfg_miss);
			vty_out(vty, "Number of unicast BSMs dropped : %u\n",
				pim_ifp->pim_ifstat_ucast_bsm_cfg_miss);
			vty_out(vty,
				"Number of BSMs dropped due to invalid scope zone : %u\n",
				pim_ifp->pim_ifstat_bsm_invalid_sz);
		} else {

			json_object *json_row = NULL;

			json_row = json_object_new_object();

			json_object_string_add(json_row, "If Name", ifp->name);
			json_object_int_add(json_row, "bsmDroppedConfig",
					    pim_ifp->pim_ifstat_bsm_cfg_miss);
			json_object_int_add(
				json_row, "bsmDroppedUnicast",
				pim_ifp->pim_ifstat_ucast_bsm_cfg_miss);
			json_object_int_add(json_row,
					    "bsmDroppedInvalidScopeZone",
					    pim_ifp->pim_ifstat_bsm_invalid_sz);
			json_object_object_add(json, ifp->name, json_row);
		}
		vty_out(vty, "\n");
	}

	if (uj)
		vty_json(vty, json);
}

static void clear_pim_statistics(struct pim_instance *pim)
{
	struct interface *ifp;

	pim->bsm_rcvd = 0;
	pim->bsm_sent = 0;
	pim->bsm_dropped = 0;

	/* scan interfaces */
	FOR_ALL_INTERFACES (pim->vrf, ifp) {
		struct pim_interface *pim_ifp = ifp->info;

		if (!pim_ifp)
			continue;

		pim_ifp->pim_ifstat_bsm_cfg_miss = 0;
		pim_ifp->pim_ifstat_ucast_bsm_cfg_miss = 0;
		pim_ifp->pim_ifstat_bsm_invalid_sz = 0;
	}
}

static void igmp_show_groups(struct pim_instance *pim, struct vty *vty, bool uj)
{
	struct interface *ifp;
	time_t now;
	json_object *json = NULL;
	json_object *json_iface = NULL;
	json_object *json_group = NULL;
	json_object *json_groups = NULL;

	now = pim_time_monotonic_sec();

	if (uj) {
		json = json_object_new_object();
		json_object_int_add(json, "totalGroups", pim->igmp_group_count);
		json_object_int_add(json, "watermarkLimit",
				    pim->igmp_watermark_limit);
	} else {
		vty_out(vty, "Total IGMP groups: %u\n", pim->igmp_group_count);
		vty_out(vty, "Watermark warn limit(%s): %u\n",
			pim->igmp_watermark_limit ? "Set" : "Not Set",
			pim->igmp_watermark_limit);
		vty_out(vty,
			"Interface        Group           Mode Timer    Srcs V Uptime  \n");
	}

	/* scan interfaces */
	FOR_ALL_INTERFACES (pim->vrf, ifp) {
		struct pim_interface *pim_ifp = ifp->info;
		struct listnode *grpnode;
		struct gm_group *grp;

		if (!pim_ifp)
			continue;

		/* scan igmp groups */
		for (ALL_LIST_ELEMENTS_RO(pim_ifp->gm_group_list, grpnode,
					  grp)) {
			char group_str[INET_ADDRSTRLEN];
			char hhmmss[10];
			char uptime[10];

			pim_inet4_dump("<group?>", grp->group_addr, group_str,
				       sizeof(group_str));
			pim_time_timer_to_hhmmss(hhmmss, sizeof(hhmmss),
						 grp->t_group_timer);
			pim_time_uptime(uptime, sizeof(uptime),
					now - grp->group_creation);

			if (uj) {
				json_object_object_get_ex(json, ifp->name,
							  &json_iface);

				if (!json_iface) {
					json_iface = json_object_new_object();
					json_object_pim_ifp_add(json_iface,
								ifp);
					json_object_object_add(json, ifp->name,
							       json_iface);
					json_groups = json_object_new_array();
					json_object_object_add(json_iface,
							       "groups",
							       json_groups);
				}

				json_group = json_object_new_object();
				json_object_string_add(json_group, "group",
						       group_str);

				if (grp->igmp_version == 3)
					json_object_string_add(
						json_group, "mode",
						grp->group_filtermode_isexcl
							? "EXCLUDE"
							: "INCLUDE");

				json_object_string_add(json_group, "timer",
						       hhmmss);
				json_object_int_add(
					json_group, "sourcesCount",
					grp->group_source_list ? listcount(
						grp->group_source_list)
							       : 0);
				json_object_int_add(json_group, "version",
						    grp->igmp_version);
				json_object_string_add(json_group, "uptime",
						       uptime);
				json_object_array_add(json_groups, json_group);
			} else {
				vty_out(vty, "%-16s %-15s %4s %8s %4d %d %8s\n",
					ifp->name, group_str,
					grp->igmp_version == 3
						? (grp->group_filtermode_isexcl
							   ? "EXCL"
							   : "INCL")
						: "----",
					hhmmss,
					grp->group_source_list ? listcount(
						grp->group_source_list)
							       : 0,
					grp->igmp_version, uptime);
			}
		} /* scan igmp groups */
	}	  /* scan interfaces */

	if (uj)
		vty_json(vty, json);
}

static void igmp_show_group_retransmission(struct pim_instance *pim,
					   struct vty *vty)
{
	struct interface *ifp;

	vty_out(vty,
		"Interface        Group           RetTimer Counter RetSrcs\n");

	/* scan interfaces */
	FOR_ALL_INTERFACES (pim->vrf, ifp) {
		struct pim_interface *pim_ifp = ifp->info;
		struct listnode *grpnode;
		struct gm_group *grp;

		if (!pim_ifp)
			continue;

		/* scan igmp groups */
		for (ALL_LIST_ELEMENTS_RO(pim_ifp->gm_group_list, grpnode,
					  grp)) {
			char group_str[INET_ADDRSTRLEN];
			char grp_retr_mmss[10];
			struct listnode *src_node;
			struct gm_source *src;
			int grp_retr_sources = 0;

			pim_inet4_dump("<group?>", grp->group_addr, group_str,
				       sizeof(group_str));
			pim_time_timer_to_mmss(
				grp_retr_mmss, sizeof(grp_retr_mmss),
				grp->t_group_query_retransmit_timer);


			/* count group sources with retransmission state
			 */
			for (ALL_LIST_ELEMENTS_RO(grp->group_source_list,
						  src_node, src)) {
				if (src->source_query_retransmit_count > 0) {
					++grp_retr_sources;
				}
			}

			vty_out(vty, "%-16s %-15s %-8s %7d %7d\n", ifp->name,
				group_str, grp_retr_mmss,
				grp->group_specific_query_retransmit_count,
				grp_retr_sources);

		} /* scan igmp groups */
	}	  /* scan interfaces */
}

static void igmp_show_sources(struct pim_instance *pim, struct vty *vty)
{
	struct interface *ifp;
	time_t now;

	now = pim_time_monotonic_sec();

	vty_out(vty,
		"Interface        Group           Source          Timer Fwd Uptime  \n");

	/* scan interfaces */
	FOR_ALL_INTERFACES (pim->vrf, ifp) {
		struct pim_interface *pim_ifp = ifp->info;
		struct listnode *grpnode;
		struct gm_group *grp;

		if (!pim_ifp)
			continue;

		/* scan igmp groups */
		for (ALL_LIST_ELEMENTS_RO(pim_ifp->gm_group_list, grpnode,
					  grp)) {
			char group_str[INET_ADDRSTRLEN];
			struct listnode *srcnode;
			struct gm_source *src;

			pim_inet4_dump("<group?>", grp->group_addr, group_str,
				       sizeof(group_str));

			/* scan group sources */
			for (ALL_LIST_ELEMENTS_RO(grp->group_source_list,
						  srcnode, src)) {
				char source_str[INET_ADDRSTRLEN];
				char mmss[10];
				char uptime[10];

				pim_inet4_dump("<source?>", src->source_addr,
					       source_str, sizeof(source_str));

				pim_time_timer_to_mmss(mmss, sizeof(mmss),
						       src->t_source_timer);

				pim_time_uptime(uptime, sizeof(uptime),
						now - src->source_creation);

				vty_out(vty, "%-16s %-15s %-15s %5s %3s %8s\n",
					ifp->name, group_str, source_str, mmss,
					IGMP_SOURCE_TEST_FORWARDING(
						src->source_flags)
						? "Y"
						: "N",
					uptime);

			} /* scan group sources */
		}	 /* scan igmp groups */
	}		  /* scan interfaces */
}

static void igmp_show_source_retransmission(struct pim_instance *pim,
					    struct vty *vty)
{
	struct interface *ifp;

	vty_out(vty,
		"Interface        Group           Source          Counter\n");

	/* scan interfaces */
	FOR_ALL_INTERFACES (pim->vrf, ifp) {
		struct pim_interface *pim_ifp = ifp->info;
		struct listnode *grpnode;
		struct gm_group *grp;

		if (!pim_ifp)
			continue;

		/* scan igmp groups */
		for (ALL_LIST_ELEMENTS_RO(pim_ifp->gm_group_list, grpnode,
					  grp)) {
			char group_str[INET_ADDRSTRLEN];
			struct listnode *srcnode;
			struct gm_source *src;

			pim_inet4_dump("<group?>", grp->group_addr, group_str,
				       sizeof(group_str));

			/* scan group sources */
			for (ALL_LIST_ELEMENTS_RO(grp->group_source_list,
						  srcnode, src)) {
				char source_str[INET_ADDRSTRLEN];

				pim_inet4_dump("<source?>", src->source_addr,
					       source_str, sizeof(source_str));

				vty_out(vty, "%-16s %-15s %-15s %7d\n",
					ifp->name, group_str, source_str,
					src->source_query_retransmit_count);

			} /* scan group sources */
		}	 /* scan igmp groups */
	}		  /* scan interfaces */
}

static void pim_show_bsr(struct pim_instance *pim,
			 struct vty *vty,
			 bool uj)
{
	char uptime[10];
	char last_bsm_seen[10];
	time_t now;
	char bsr_state[20];
	char bsr_str[PREFIX_STRLEN];
	json_object *json = NULL;

	if (pim->global_scope.current_bsr.s_addr == INADDR_ANY) {
		strlcpy(bsr_str, "0.0.0.0", sizeof(bsr_str));
		pim_time_uptime(uptime, sizeof(uptime),
				pim->global_scope.current_bsr_first_ts);
		pim_time_uptime(last_bsm_seen, sizeof(last_bsm_seen),
				pim->global_scope.current_bsr_last_ts);
	}

	else {
		pim_inet4_dump("<bsr?>", pim->global_scope.current_bsr,
			       bsr_str, sizeof(bsr_str));
		now = pim_time_monotonic_sec();
		pim_time_uptime(uptime, sizeof(uptime),
				(now - pim->global_scope.current_bsr_first_ts));
		pim_time_uptime(last_bsm_seen, sizeof(last_bsm_seen),
				now - pim->global_scope.current_bsr_last_ts);
	}

	switch (pim->global_scope.state) {
	case NO_INFO:
		strlcpy(bsr_state, "NO_INFO", sizeof(bsr_state));
		break;
	case ACCEPT_ANY:
		strlcpy(bsr_state, "ACCEPT_ANY", sizeof(bsr_state));
		break;
	case ACCEPT_PREFERRED:
		strlcpy(bsr_state, "ACCEPT_PREFERRED", sizeof(bsr_state));
		break;
	default:
		strlcpy(bsr_state, "", sizeof(bsr_state));
	}

	if (uj) {
		json = json_object_new_object();
		json_object_string_add(json, "bsr", bsr_str);
		json_object_int_add(json, "priority",
				    pim->global_scope.current_bsr_prio);
		json_object_int_add(json, "fragmentTag",
				    pim->global_scope.bsm_frag_tag);
		json_object_string_add(json, "state", bsr_state);
		json_object_string_add(json, "upTime", uptime);
		json_object_string_add(json, "lastBsmSeen", last_bsm_seen);
	}

	else {
		vty_out(vty, "PIMv2 Bootstrap information\n");
		vty_out(vty, "Current preferred BSR address: %s\n", bsr_str);
		vty_out(vty,
			"Priority        Fragment-Tag       State           UpTime\n");
		vty_out(vty, "  %-12d    %-12d    %-13s    %7s\n",
			pim->global_scope.current_bsr_prio,
			pim->global_scope.bsm_frag_tag,
			bsr_state,
			uptime);
		vty_out(vty, "Last BSM seen: %s\n", last_bsm_seen);
	}

	if (uj)
		vty_json(vty, json);
}

static void clear_igmp_interfaces(struct pim_instance *pim)
{
	struct interface *ifp;

	FOR_ALL_INTERFACES (pim->vrf, ifp)
		pim_if_addr_del_all_igmp(ifp);

	FOR_ALL_INTERFACES (pim->vrf, ifp)
		pim_if_addr_add_all(ifp);
}

static void clear_pim_interfaces(struct pim_instance *pim)
{
	struct interface *ifp;

	FOR_ALL_INTERFACES (pim->vrf, ifp) {
		if (ifp->info) {
			pim_neighbor_delete_all(ifp, "interface cleared");
		}
	}
}

static void clear_interfaces(struct pim_instance *pim)
{
	clear_igmp_interfaces(pim);
	clear_pim_interfaces(pim);
}

#define PIM_GET_PIM_INTERFACE(pim_ifp, ifp)				\
	pim_ifp = ifp->info;						\
	if (!pim_ifp) {							\
		vty_out(vty,						\
			"%% Enable PIM and/or IGMP on this interface first\n"); \
		return CMD_WARNING_CONFIG_FAILED;			\
	}

/**
 * Get current node VRF name.
 *
 * NOTE:
 * In case of failure it will print error message to user.
 *
 * \returns name or NULL if failed to get VRF.
 */
static const char *pim_cli_get_vrf_name(struct vty *vty)
{
	const struct lyd_node *vrf_node;

	/* Not inside any VRF context. */
	if (vty->xpath_index == 0)
		return VRF_DEFAULT_NAME;

	vrf_node = yang_dnode_get(vty->candidate_config->dnode, VTY_CURR_XPATH);
	if (vrf_node == NULL) {
		vty_out(vty, "%% Failed to get vrf dnode in configuration\n");
		return NULL;
	}

	return yang_dnode_get_string(vrf_node, "./name");
}

#if PIM_IPV != 6
/**
 * Compatibility function to keep the legacy mesh group CLI behavior:
 * Delete group when there are no more configurations in it.
 *
 * NOTE:
 * Don't forget to call `nb_cli_apply_changes` after this.
 */
static void pim_cli_legacy_mesh_group_behavior(struct vty *vty,
					       const char *gname)
{
	const char *vrfname;
	char xpath_value[XPATH_MAXLEN];
	char xpath_member_value[XPATH_MAXLEN];
	const struct lyd_node *member_dnode;

	vrfname = pim_cli_get_vrf_name(vty);
	if (vrfname == NULL)
		return;

	/* Get mesh group base XPath. */
	snprintf(xpath_value, sizeof(xpath_value),
		 FRR_PIM_VRF_XPATH "/msdp-mesh-groups[name='%s']",
		 "frr-pim:pimd", "pim", vrfname, "frr-routing:ipv4", gname);
	/* Group must exists, otherwise just quit. */
	if (!yang_dnode_exists(vty->candidate_config->dnode, xpath_value))
		return;

	/* Group members check: */
	strlcpy(xpath_member_value, xpath_value, sizeof(xpath_member_value));
	strlcat(xpath_member_value, "/members", sizeof(xpath_member_value));
	if (yang_dnode_exists(vty->candidate_config->dnode,
			      xpath_member_value)) {
		member_dnode = yang_dnode_get(vty->candidate_config->dnode,
					      xpath_member_value);
		if (!member_dnode || !yang_is_last_list_dnode(member_dnode))
			return;
	}

	/* Source address check: */
	strlcpy(xpath_member_value, xpath_value, sizeof(xpath_member_value));
	strlcat(xpath_member_value, "/source", sizeof(xpath_member_value));
	if (yang_dnode_exists(vty->candidate_config->dnode, xpath_member_value))
		return;

	/* No configurations found: delete it. */
	nb_cli_enqueue_change(vty, xpath_value, NB_OP_DESTROY, NULL);
}
#endif /* PIM_IPV != 6 */

DEFUN (clear_ip_interfaces,
       clear_ip_interfaces_cmd,
       "clear ip interfaces [vrf NAME]",
       CLEAR_STR
       IP_STR
       "Reset interfaces\n"
       VRF_CMD_HELP_STR)
{
	int idx = 2;
	struct vrf *vrf = pim_cmd_lookup_vrf(vty, argv, argc, &idx);

	if (!vrf)
		return CMD_WARNING;

	clear_interfaces(vrf->info);

	return CMD_SUCCESS;
}

DEFUN (clear_ip_igmp_interfaces,
       clear_ip_igmp_interfaces_cmd,
       "clear ip igmp [vrf NAME] interfaces",
       CLEAR_STR
       IP_STR
       CLEAR_IP_IGMP_STR
       VRF_CMD_HELP_STR
       "Reset IGMP interfaces\n")
{
	int idx = 2;
	struct vrf *vrf = pim_cmd_lookup_vrf(vty, argv, argc, &idx);

	if (!vrf)
		return CMD_WARNING;

	clear_igmp_interfaces(vrf->info);

	return CMD_SUCCESS;
}

DEFUN (clear_ip_pim_statistics,
       clear_ip_pim_statistics_cmd,
       "clear ip pim statistics [vrf NAME]",
       CLEAR_STR
       IP_STR
       CLEAR_IP_PIM_STR
       VRF_CMD_HELP_STR
       "Reset PIM statistics\n")
{
	int idx = 2;
	struct vrf *vrf = pim_cmd_lookup_vrf(vty, argv, argc, &idx);

	if (!vrf)
		return CMD_WARNING;

	clear_pim_statistics(vrf->info);
	return CMD_SUCCESS;
}

static void clear_mroute(struct pim_instance *pim)
{
	struct pim_upstream *up;
	struct interface *ifp;

	/* scan interfaces */
	FOR_ALL_INTERFACES (pim->vrf, ifp) {
		struct pim_interface *pim_ifp = ifp->info;
		struct gm_group *grp;
		struct pim_ifchannel *ch;

		if (!pim_ifp)
			continue;

		/* deleting all ifchannels */
		while (!RB_EMPTY(pim_ifchannel_rb, &pim_ifp->ifchannel_rb)) {
			ch = RB_ROOT(pim_ifchannel_rb, &pim_ifp->ifchannel_rb);

			pim_ifchannel_delete(ch);
		}

		/* clean up all igmp groups */

		if (pim_ifp->gm_group_list) {
			while (pim_ifp->gm_group_list->count) {
				grp = listnode_head(pim_ifp->gm_group_list);
				igmp_group_delete(grp);
			}
		}
	}

	/* clean up all upstreams*/
	while ((up = rb_pim_upstream_first(&pim->upstream_head)))
		pim_upstream_del(pim, up, __func__);

}

DEFUN (clear_ip_mroute,
       clear_ip_mroute_cmd,
       "clear ip mroute [vrf NAME]",
       CLEAR_STR
       IP_STR
       "Reset multicast routes\n"
       VRF_CMD_HELP_STR)
{
	int idx = 2;
	struct vrf *vrf = pim_cmd_lookup_vrf(vty, argv, argc, &idx);

	if (!vrf)
		return CMD_WARNING;

	clear_mroute(vrf->info);

	return CMD_SUCCESS;
}

DEFUN (clear_ip_pim_interfaces,
       clear_ip_pim_interfaces_cmd,
       "clear ip pim [vrf NAME] interfaces",
       CLEAR_STR
       IP_STR
       CLEAR_IP_PIM_STR
       VRF_CMD_HELP_STR
       "Reset PIM interfaces\n")
{
	int idx = 2;
	struct vrf *vrf = pim_cmd_lookup_vrf(vty, argv, argc, &idx);

	if (!vrf)
		return CMD_WARNING;

	clear_pim_interfaces(vrf->info);

	return CMD_SUCCESS;
}

DEFUN (clear_ip_pim_interface_traffic,
       clear_ip_pim_interface_traffic_cmd,
       "clear ip pim [vrf NAME] interface traffic",
       "Reset functions\n"
       "IP information\n"
       "PIM clear commands\n"
       VRF_CMD_HELP_STR
       "Reset PIM interfaces\n"
       "Reset Protocol Packet counters\n")
{
	int idx = 2;
	struct vrf *vrf = pim_cmd_lookup_vrf(vty, argv, argc, &idx);
	struct interface *ifp = NULL;
	struct pim_interface *pim_ifp = NULL;

	if (!vrf)
		return CMD_WARNING;

	FOR_ALL_INTERFACES (vrf, ifp) {
		pim_ifp = ifp->info;

		if (!pim_ifp)
			continue;

		pim_ifp->pim_ifstat_hello_recv = 0;
		pim_ifp->pim_ifstat_hello_sent = 0;
		pim_ifp->pim_ifstat_join_recv = 0;
		pim_ifp->pim_ifstat_join_send = 0;
		pim_ifp->pim_ifstat_prune_recv = 0;
		pim_ifp->pim_ifstat_prune_send = 0;
		pim_ifp->pim_ifstat_reg_recv = 0;
		pim_ifp->pim_ifstat_reg_send = 0;
		pim_ifp->pim_ifstat_reg_stop_recv = 0;
		pim_ifp->pim_ifstat_reg_stop_send = 0;
		pim_ifp->pim_ifstat_assert_recv = 0;
		pim_ifp->pim_ifstat_assert_send = 0;
		pim_ifp->pim_ifstat_bsm_rx = 0;
		pim_ifp->pim_ifstat_bsm_tx = 0;
	}

	return CMD_SUCCESS;
}

DEFUN (clear_ip_pim_oil,
       clear_ip_pim_oil_cmd,
       "clear ip pim [vrf NAME] oil",
       CLEAR_STR
       IP_STR
       CLEAR_IP_PIM_STR
       VRF_CMD_HELP_STR
       "Rescan PIM OIL (output interface list)\n")
{
	int idx = 2;
	struct vrf *vrf = pim_cmd_lookup_vrf(vty, argv, argc, &idx);

	if (!vrf)
		return CMD_WARNING;

	pim_scan_oil(vrf->info);

	return CMD_SUCCESS;
}

DEFUN (clear_ip_pim_bsr_db,
       clear_ip_pim_bsr_db_cmd,
       "clear ip pim [vrf NAME] bsr-data",
       CLEAR_STR
       IP_STR
       CLEAR_IP_PIM_STR
       VRF_CMD_HELP_STR
       "Reset pim bsr data\n")
{
	int idx = 2;
	struct vrf *vrf = pim_cmd_lookup_vrf(vty, argv, argc, &idx);

	if (!vrf)
		return CMD_WARNING;

	pim_bsm_clear(vrf->info);

	return CMD_SUCCESS;
}

DEFUN (show_ip_igmp_interface,
       show_ip_igmp_interface_cmd,
       "show ip igmp [vrf NAME] interface [detail|WORD] [json]",
       SHOW_STR
       IP_STR
       IGMP_STR
       VRF_CMD_HELP_STR
       "IGMP interface information\n"
       "Detailed output\n"
       "interface name\n"
       JSON_STR)
{
	int idx = 2;
	struct vrf *vrf = pim_cmd_lookup_vrf(vty, argv, argc, &idx);
	bool uj = use_json(argc, argv);

	if (!vrf)
		return CMD_WARNING;

	if (argv_find(argv, argc, "detail", &idx)
	    || argv_find(argv, argc, "WORD", &idx))
		igmp_show_interfaces_single(vrf->info, vty, argv[idx]->arg, uj);
	else
		igmp_show_interfaces(vrf->info, vty, uj);

	return CMD_SUCCESS;
}

DEFUN (show_ip_igmp_interface_vrf_all,
       show_ip_igmp_interface_vrf_all_cmd,
       "show ip igmp vrf all interface [detail|WORD] [json]",
       SHOW_STR
       IP_STR
       IGMP_STR
       VRF_CMD_HELP_STR
       "IGMP interface information\n"
       "Detailed output\n"
       "interface name\n"
       JSON_STR)
{
	int idx = 2;
	bool uj = use_json(argc, argv);
	struct vrf *vrf;
	bool first = true;

	if (uj)
		vty_out(vty, "{ ");
	RB_FOREACH (vrf, vrf_name_head, &vrfs_by_name) {
		if (uj) {
			if (!first)
				vty_out(vty, ", ");
			vty_out(vty, " \"%s\": ", vrf->name);
			first = false;
		} else
			vty_out(vty, "VRF: %s\n", vrf->name);
		if (argv_find(argv, argc, "detail", &idx)
		    || argv_find(argv, argc, "WORD", &idx))
			igmp_show_interfaces_single(vrf->info, vty,
						    argv[idx]->arg, uj);
		else
			igmp_show_interfaces(vrf->info, vty, uj);
	}
	if (uj)
		vty_out(vty, "}\n");

	return CMD_SUCCESS;
}

DEFUN (show_ip_igmp_join,
       show_ip_igmp_join_cmd,
       "show ip igmp [vrf NAME] join [json]",
       SHOW_STR
       IP_STR
       IGMP_STR
       VRF_CMD_HELP_STR
       "IGMP static join information\n"
       JSON_STR)
{
	int idx = 2;
	struct vrf *vrf = pim_cmd_lookup_vrf(vty, argv, argc, &idx);
	bool uj = use_json(argc, argv);

	if (!vrf)
		return CMD_WARNING;

	igmp_show_interface_join(vrf->info, vty, uj);

	return CMD_SUCCESS;
}

DEFUN (show_ip_igmp_join_vrf_all,
       show_ip_igmp_join_vrf_all_cmd,
       "show ip igmp vrf all join [json]",
       SHOW_STR
       IP_STR
       IGMP_STR
       VRF_CMD_HELP_STR
       "IGMP static join information\n"
       JSON_STR)
{
	bool uj = use_json(argc, argv);
	struct vrf *vrf;
	bool first = true;

	if (uj)
		vty_out(vty, "{ ");
	RB_FOREACH (vrf, vrf_name_head, &vrfs_by_name) {
		if (uj) {
			if (!first)
				vty_out(vty, ", ");
			vty_out(vty, " \"%s\": ", vrf->name);
			first = false;
		} else
			vty_out(vty, "VRF: %s\n", vrf->name);
		igmp_show_interface_join(vrf->info, vty, uj);
	}
	if (uj)
		vty_out(vty, "}\n");

	return CMD_SUCCESS;
}

DEFUN (show_ip_igmp_groups,
       show_ip_igmp_groups_cmd,
       "show ip igmp [vrf NAME] groups [json]",
       SHOW_STR
       IP_STR
       IGMP_STR
       VRF_CMD_HELP_STR
       IGMP_GROUP_STR
       JSON_STR)
{
	int idx = 2;
	struct vrf *vrf = pim_cmd_lookup_vrf(vty, argv, argc, &idx);
	bool uj = use_json(argc, argv);

	if (!vrf)
		return CMD_WARNING;

	igmp_show_groups(vrf->info, vty, uj);

	return CMD_SUCCESS;
}

DEFUN (show_ip_igmp_groups_vrf_all,
       show_ip_igmp_groups_vrf_all_cmd,
       "show ip igmp vrf all groups [json]",
       SHOW_STR
       IP_STR
       IGMP_STR
       VRF_CMD_HELP_STR
       IGMP_GROUP_STR
       JSON_STR)
{
	bool uj = use_json(argc, argv);
	struct vrf *vrf;
	bool first = true;

	if (uj)
		vty_out(vty, "{ ");
	RB_FOREACH (vrf, vrf_name_head, &vrfs_by_name) {
		if (uj) {
			if (!first)
				vty_out(vty, ", ");
			vty_out(vty, " \"%s\": ", vrf->name);
			first = false;
		} else
			vty_out(vty, "VRF: %s\n", vrf->name);
		igmp_show_groups(vrf->info, vty, uj);
	}
	if (uj)
		vty_out(vty, "}\n");

	return CMD_SUCCESS;
}

DEFUN (show_ip_igmp_groups_retransmissions,
       show_ip_igmp_groups_retransmissions_cmd,
       "show ip igmp [vrf NAME] groups retransmissions",
       SHOW_STR
       IP_STR
       IGMP_STR
       VRF_CMD_HELP_STR
       IGMP_GROUP_STR
       "IGMP group retransmissions\n")
{
	int idx = 2;
	struct vrf *vrf = pim_cmd_lookup_vrf(vty, argv, argc, &idx);

	if (!vrf)
		return CMD_WARNING;

	igmp_show_group_retransmission(vrf->info, vty);

	return CMD_SUCCESS;
}

DEFUN (show_ip_igmp_sources,
       show_ip_igmp_sources_cmd,
       "show ip igmp [vrf NAME] sources",
       SHOW_STR
       IP_STR
       IGMP_STR
       VRF_CMD_HELP_STR
       IGMP_SOURCE_STR)
{
	int idx = 2;
	struct vrf *vrf = pim_cmd_lookup_vrf(vty, argv, argc, &idx);

	if (!vrf)
		return CMD_WARNING;

	igmp_show_sources(vrf->info, vty);

	return CMD_SUCCESS;
}

DEFUN (show_ip_igmp_sources_retransmissions,
       show_ip_igmp_sources_retransmissions_cmd,
       "show ip igmp [vrf NAME] sources retransmissions",
       SHOW_STR
       IP_STR
       IGMP_STR
       VRF_CMD_HELP_STR
       IGMP_SOURCE_STR
       "IGMP source retransmissions\n")
{
	int idx = 2;
	struct vrf *vrf = pim_cmd_lookup_vrf(vty, argv, argc, &idx);

	if (!vrf)
		return CMD_WARNING;

	igmp_show_source_retransmission(vrf->info, vty);

	return CMD_SUCCESS;
}

DEFUN (show_ip_igmp_statistics,
       show_ip_igmp_statistics_cmd,
       "show ip igmp [vrf NAME] statistics [interface WORD] [json]",
       SHOW_STR
       IP_STR
       IGMP_STR
       VRF_CMD_HELP_STR
       "IGMP statistics\n"
       "interface\n"
       "IGMP interface\n"
       JSON_STR)
{
	int idx = 2;
	struct vrf *vrf = pim_cmd_lookup_vrf(vty, argv, argc, &idx);
	bool uj = use_json(argc, argv);

	if (!vrf)
		return CMD_WARNING;

	if (argv_find(argv, argc, "WORD", &idx))
		igmp_show_statistics(vrf->info, vty, argv[idx]->arg, uj);
	else
		igmp_show_statistics(vrf->info, vty, NULL, uj);

	return CMD_SUCCESS;
}

DEFUN (show_ip_pim_mlag_summary,
       show_ip_pim_mlag_summary_cmd,
       "show ip pim mlag summary [json]",
       SHOW_STR
       IP_STR
       PIM_STR
       "MLAG\n"
       "status and stats\n"
       JSON_STR)
{
	bool uj = use_json(argc, argv);
	char role_buf[MLAG_ROLE_STRSIZE];
	char addr_buf[INET_ADDRSTRLEN];

	if (uj) {
		json_object *json = NULL;
		json_object *json_stat = NULL;

		json = json_object_new_object();
		if (router->mlag_flags & PIM_MLAGF_LOCAL_CONN_UP)
			json_object_boolean_true_add(json, "mlagConnUp");
		if (router->mlag_flags & PIM_MLAGF_PEER_CONN_UP)
			json_object_boolean_true_add(json, "mlagPeerConnUp");
		if (router->mlag_flags & PIM_MLAGF_PEER_ZEBRA_UP)
			json_object_boolean_true_add(json, "mlagPeerZebraUp");
		json_object_string_add(json, "mlagRole",
				       mlag_role2str(router->mlag_role,
						     role_buf, sizeof(role_buf)));
		inet_ntop(AF_INET, &router->local_vtep_ip,
			  addr_buf, INET_ADDRSTRLEN);
		json_object_string_add(json, "localVtepIp", addr_buf);
		inet_ntop(AF_INET, &router->anycast_vtep_ip,
			  addr_buf, INET_ADDRSTRLEN);
		json_object_string_add(json, "anycastVtepIp", addr_buf);
		json_object_string_add(json, "peerlinkRif",
				       router->peerlink_rif);

		json_stat = json_object_new_object();
		json_object_int_add(json_stat, "mlagConnFlaps",
				    router->mlag_stats.mlagd_session_downs);
		json_object_int_add(json_stat, "mlagPeerConnFlaps",
				    router->mlag_stats.peer_session_downs);
		json_object_int_add(json_stat, "mlagPeerZebraFlaps",
				    router->mlag_stats.peer_zebra_downs);
		json_object_int_add(json_stat, "mrouteAddRx",
				    router->mlag_stats.msg.mroute_add_rx);
		json_object_int_add(json_stat, "mrouteAddTx",
				    router->mlag_stats.msg.mroute_add_tx);
		json_object_int_add(json_stat, "mrouteDelRx",
				    router->mlag_stats.msg.mroute_del_rx);
		json_object_int_add(json_stat, "mrouteDelTx",
				    router->mlag_stats.msg.mroute_del_tx);
		json_object_int_add(json_stat, "mlagStatusUpdates",
				    router->mlag_stats.msg.mlag_status_updates);
		json_object_int_add(json_stat, "peerZebraStatusUpdates",
				    router->mlag_stats.msg.peer_zebra_status_updates);
		json_object_int_add(json_stat, "pimStatusUpdates",
				    router->mlag_stats.msg.pim_status_updates);
		json_object_int_add(json_stat, "vxlanUpdates",
				    router->mlag_stats.msg.vxlan_updates);
		json_object_object_add(json, "connStats", json_stat);

		vty_json(vty, json);
		return CMD_SUCCESS;
	}

	vty_out(vty, "MLAG daemon connection: %s\n",
		(router->mlag_flags & PIM_MLAGF_LOCAL_CONN_UP)
		? "up" : "down");
	vty_out(vty, "MLAG peer state: %s\n",
		(router->mlag_flags & PIM_MLAGF_PEER_CONN_UP)
		? "up" : "down");
	vty_out(vty, "Zebra peer state: %s\n",
		(router->mlag_flags & PIM_MLAGF_PEER_ZEBRA_UP)
		? "up" : "down");
	vty_out(vty, "MLAG role: %s\n",
		mlag_role2str(router->mlag_role, role_buf, sizeof(role_buf)));
	inet_ntop(AF_INET, &router->local_vtep_ip,
		  addr_buf, INET_ADDRSTRLEN);
	vty_out(vty, "Local VTEP IP: %s\n", addr_buf);
	inet_ntop(AF_INET, &router->anycast_vtep_ip,
		  addr_buf, INET_ADDRSTRLEN);
	vty_out(vty, "Anycast VTEP IP: %s\n", addr_buf);
	vty_out(vty, "Peerlink: %s\n", router->peerlink_rif);
	vty_out(vty, "Session flaps: mlagd: %d mlag-peer: %d zebra-peer: %d\n",
		router->mlag_stats.mlagd_session_downs,
		router->mlag_stats.peer_session_downs,
		router->mlag_stats.peer_zebra_downs);
	vty_out(vty, "Message Statistics:\n");
	vty_out(vty, "  mroute adds: rx: %d, tx: %d\n",
		router->mlag_stats.msg.mroute_add_rx,
		router->mlag_stats.msg.mroute_add_tx);
	vty_out(vty, "  mroute dels: rx: %d, tx: %d\n",
		router->mlag_stats.msg.mroute_del_rx,
		router->mlag_stats.msg.mroute_del_tx);
	vty_out(vty, "  peer zebra status updates: %d\n",
		router->mlag_stats.msg.peer_zebra_status_updates);
	vty_out(vty, "  PIM status updates: %d\n",
		router->mlag_stats.msg.pim_status_updates);
	vty_out(vty, "  VxLAN updates: %d\n",
		router->mlag_stats.msg.vxlan_updates);

	return CMD_SUCCESS;
}

DEFUN (show_ip_pim_assert,
       show_ip_pim_assert_cmd,
       "show ip pim [vrf NAME] assert",
       SHOW_STR
       IP_STR
       PIM_STR
       VRF_CMD_HELP_STR
       "PIM interface assert\n")
{
	int idx = 2;
	struct vrf *vrf = pim_cmd_lookup_vrf(vty, argv, argc, &idx);

	if (!vrf)
		return CMD_WARNING;

	pim_show_assert(vrf->info, vty);

	return CMD_SUCCESS;
}

DEFUN (show_ip_pim_assert_internal,
       show_ip_pim_assert_internal_cmd,
       "show ip pim [vrf NAME] assert-internal",
       SHOW_STR
       IP_STR
       PIM_STR
       VRF_CMD_HELP_STR
       "PIM interface internal assert state\n")
{
	int idx = 2;
	struct vrf *vrf = pim_cmd_lookup_vrf(vty, argv, argc, &idx);

	if (!vrf)
		return CMD_WARNING;

	pim_show_assert_internal(vrf->info, vty);

	return CMD_SUCCESS;
}

DEFUN (show_ip_pim_assert_metric,
       show_ip_pim_assert_metric_cmd,
       "show ip pim [vrf NAME] assert-metric",
       SHOW_STR
       IP_STR
       PIM_STR
       VRF_CMD_HELP_STR
       "PIM interface assert metric\n")
{
	int idx = 2;
	struct vrf *vrf = pim_cmd_lookup_vrf(vty, argv, argc, &idx);

	if (!vrf)
		return CMD_WARNING;

	pim_show_assert_metric(vrf->info, vty);

	return CMD_SUCCESS;
}

DEFUN (show_ip_pim_assert_winner_metric,
       show_ip_pim_assert_winner_metric_cmd,
       "show ip pim [vrf NAME] assert-winner-metric",
       SHOW_STR
       IP_STR
       PIM_STR
       VRF_CMD_HELP_STR
       "PIM interface assert winner metric\n")
{
	int idx = 2;
	struct vrf *vrf = pim_cmd_lookup_vrf(vty, argv, argc, &idx);

	if (!vrf)
		return CMD_WARNING;

	pim_show_assert_winner_metric(vrf->info, vty);

	return CMD_SUCCESS;
}

DEFUN (show_ip_pim_interface,
       show_ip_pim_interface_cmd,
       "show ip pim [mlag] [vrf NAME] interface [detail|WORD] [json]",
       SHOW_STR
       IP_STR
       PIM_STR
       "MLAG\n"
       VRF_CMD_HELP_STR
       "PIM interface information\n"
       "Detailed output\n"
       "interface name\n"
       JSON_STR)
{
	int idx = 2;
	struct vrf *vrf = pim_cmd_lookup_vrf(vty, argv, argc, &idx);
	bool uj = use_json(argc, argv);
	bool mlag = false;

	if (!vrf)
		return CMD_WARNING;

	if (argv_find(argv, argc, "mlag", &idx))
		mlag = true;

	if (argv_find(argv, argc, "WORD", &idx)
	    || argv_find(argv, argc, "detail", &idx))
		pim_show_interfaces_single(vrf->info, vty, argv[idx]->arg, mlag,
					   uj);
	else
		pim_show_interfaces(vrf->info, vty, mlag, uj);

	return CMD_SUCCESS;
}

DEFUN (show_ip_pim_interface_vrf_all,
       show_ip_pim_interface_vrf_all_cmd,
       "show ip pim [mlag] vrf all interface [detail|WORD] [json]",
       SHOW_STR
       IP_STR
       PIM_STR
       "MLAG\n"
       VRF_CMD_HELP_STR
       "PIM interface information\n"
       "Detailed output\n"
       "interface name\n"
       JSON_STR)
{
	int idx = 2;
	bool uj = use_json(argc, argv);
	struct vrf *vrf;
	bool first = true;
	bool mlag = false;

	if (argv_find(argv, argc, "mlag", &idx))
		mlag = true;

	idx = 6;
	if (uj)
		vty_out(vty, "{ ");
	RB_FOREACH (vrf, vrf_name_head, &vrfs_by_name) {
		if (uj) {
			if (!first)
				vty_out(vty, ", ");
			vty_out(vty, " \"%s\": ", vrf->name);
			first = false;
		} else
			vty_out(vty, "VRF: %s\n", vrf->name);
		if (argv_find(argv, argc, "WORD", &idx)
		    || argv_find(argv, argc, "detail", &idx))
			pim_show_interfaces_single(vrf->info, vty,
						   argv[idx]->arg, mlag, uj);
		else
			pim_show_interfaces(vrf->info, vty, mlag, uj);
	}
	if (uj)
		vty_out(vty, "}\n");

	return CMD_SUCCESS;
}

DEFPY (show_ip_pim_join,
       show_ip_pim_join_cmd,
       "show ip pim [vrf NAME] join [A.B.C.D$s_or_g [A.B.C.D$g]] [json$json]",
       SHOW_STR
       IP_STR
       PIM_STR
       VRF_CMD_HELP_STR
       "PIM interface join information\n"
       "The Source or Group\n"
       "The Group\n"
       JSON_STR)
{
	pim_sgaddr sg = {0};
	struct vrf *v;
	bool uj = !!json;
	struct pim_instance *pim;

	v = vrf_lookup_by_name(vrf ? vrf : VRF_DEFAULT_NAME);

	if (!v) {
		vty_out(vty, "%% Vrf specified: %s does not exist\n", vrf);
		return CMD_WARNING;
	}
	pim = pim_get_pim_instance(v->vrf_id);

	if (!pim) {
		vty_out(vty, "%% Unable to find pim instance\n");
		return CMD_WARNING;
	}

	if (s_or_g.s_addr != INADDR_ANY) {
		if (g.s_addr != INADDR_ANY) {
			sg.src = s_or_g;
			sg.grp = g;
		} else
			sg.grp = s_or_g;
	}

	pim_show_join(pim, vty, &sg, uj);

	return CMD_SUCCESS;
}

DEFUN (show_ip_pim_join_vrf_all,
       show_ip_pim_join_vrf_all_cmd,
       "show ip pim vrf all join [json]",
       SHOW_STR
       IP_STR
       PIM_STR
       VRF_CMD_HELP_STR
       "PIM interface join information\n"
       JSON_STR)
{
	pim_sgaddr sg = {0};
	bool uj = use_json(argc, argv);
	struct vrf *vrf;
	bool first = true;

	if (uj)
		vty_out(vty, "{ ");
	RB_FOREACH (vrf, vrf_name_head, &vrfs_by_name) {
		if (uj) {
			if (!first)
				vty_out(vty, ", ");
			vty_out(vty, " \"%s\": ", vrf->name);
			first = false;
		} else
			vty_out(vty, "VRF: %s\n", vrf->name);
		pim_show_join(vrf->info, vty, &sg, uj);
	}
	if (uj)
		vty_out(vty, "}\n");

	return CMD_WARNING;
}

static void pim_show_jp_agg_helper(struct vty *vty,
				   struct interface *ifp,
				   struct pim_neighbor *neigh,
				   struct pim_upstream *up,
				   int is_join)
{
	char rpf_str[INET_ADDRSTRLEN];

	/* pius->address.s_addr */
	pim_inet4_dump("<rpf?>", neigh->source_addr, rpf_str, sizeof(rpf_str));

	vty_out(vty, "%-16s %-15s %-15pPAs %-15pPAs %5s\n", ifp->name, rpf_str,
		&up->sg.src, &up->sg.grp, is_join ? "J" : "P");
}

static void pim_show_jp_agg_list(struct pim_instance *pim, struct vty *vty)
{
	struct interface *ifp;
	struct pim_interface *pim_ifp;
	struct listnode *n_node;
	struct pim_neighbor *neigh;
	struct listnode *jag_node;
	struct pim_jp_agg_group *jag;
	struct listnode *js_node;
	struct pim_jp_sources *js;

	vty_out(vty,
		"Interface        RPF Nbr         Source          Group           State\n");

	FOR_ALL_INTERFACES (pim->vrf, ifp) {
		pim_ifp = ifp->info;
		if (!pim_ifp)
			continue;

		for (ALL_LIST_ELEMENTS_RO(pim_ifp->pim_neighbor_list,
					  n_node, neigh)) {
			for (ALL_LIST_ELEMENTS_RO(neigh->upstream_jp_agg,
						  jag_node, jag)) {
				for (ALL_LIST_ELEMENTS_RO(jag->sources,
							  js_node, js)) {
					pim_show_jp_agg_helper(vty,
							       ifp, neigh, js->up,
							       js->is_join);
				}
			}
		}
	}
}

DEFPY (show_ip_pim_jp_agg,
       show_ip_pim_jp_agg_cmd,
       "show ip pim [vrf NAME] jp-agg",
       SHOW_STR
       IP_STR
       PIM_STR
       VRF_CMD_HELP_STR
       "join prune aggregation list\n")
{
	struct vrf *v;
	struct pim_instance *pim;

	v = vrf_lookup_by_name(vrf ? vrf : VRF_DEFAULT_NAME);

	if (!v) {
		vty_out(vty, "%% Vrf specified: %s does not exist\n", vrf);
		return CMD_WARNING;
	}
	pim = pim_get_pim_instance(v->vrf_id);

	if (!pim) {
		vty_out(vty, "%% Unable to find pim instance\n");
		return CMD_WARNING;
	}

	pim_show_jp_agg_list(pim, vty);

	return CMD_SUCCESS;
}

DEFUN (show_ip_pim_local_membership,
       show_ip_pim_local_membership_cmd,
       "show ip pim [vrf NAME] local-membership [json]",
       SHOW_STR
       IP_STR
       PIM_STR
       VRF_CMD_HELP_STR
       "PIM interface local-membership\n"
       JSON_STR)
{
	int idx = 2;
	struct vrf *vrf = pim_cmd_lookup_vrf(vty, argv, argc, &idx);
	bool uj = use_json(argc, argv);

	if (!vrf)
		return CMD_WARNING;

	pim_show_membership(vrf->info, vty, uj);

	return CMD_SUCCESS;
}

static void pim_show_mlag_up_entry_detail(struct vrf *vrf,
					  struct vty *vty,
					  struct pim_upstream *up,
					  char *src_str, char *grp_str,
					  json_object *json)
{
	if (json) {
		json_object *json_row = NULL;
		json_object *own_list = NULL;
		json_object *json_group = NULL;


		json_object_object_get_ex(json, grp_str, &json_group);
		if (!json_group) {
			json_group = json_object_new_object();
			json_object_object_add(json, grp_str,
					       json_group);
		}

		json_row = json_object_new_object();
		json_object_string_add(json_row, "source", src_str);
		json_object_string_add(json_row, "group", grp_str);

		own_list = json_object_new_array();
		if (pim_up_mlag_is_local(up))
			json_object_array_add(own_list,
					      json_object_new_string("local"));
		if (up->flags & (PIM_UPSTREAM_FLAG_MASK_MLAG_PEER))
			json_object_array_add(own_list,
					      json_object_new_string("peer"));
		if (up->flags & (PIM_UPSTREAM_FLAG_MASK_MLAG_INTERFACE))
			json_object_array_add(
				own_list, json_object_new_string("Interface"));
		json_object_object_add(json_row, "owners", own_list);

		json_object_int_add(json_row, "localCost",
				    pim_up_mlag_local_cost(up));
		json_object_int_add(json_row, "peerCost",
				    pim_up_mlag_peer_cost(up));
		if (PIM_UPSTREAM_FLAG_TEST_MLAG_NON_DF(up->flags))
			json_object_boolean_false_add(json_row, "df");
		else
			json_object_boolean_true_add(json_row, "df");
		json_object_object_add(json_group, src_str, json_row);
	} else {
		char own_str[6];

		own_str[0] = '\0';
		if (pim_up_mlag_is_local(up))
			strlcat(own_str, "L", sizeof(own_str));
		if (up->flags & (PIM_UPSTREAM_FLAG_MASK_MLAG_PEER))
			strlcat(own_str, "P", sizeof(own_str));
		if (up->flags & (PIM_UPSTREAM_FLAG_MASK_MLAG_INTERFACE))
			strlcat(own_str, "I", sizeof(own_str));
		/* XXX - fixup, print paragraph output */
		vty_out(vty,
			"%-15s %-15s %-6s %-11u %-10d %2s\n",
			src_str, grp_str, own_str,
			pim_up_mlag_local_cost(up),
			pim_up_mlag_peer_cost(up),
			PIM_UPSTREAM_FLAG_TEST_MLAG_NON_DF(up->flags)
			? "n" : "y");
	}
}

static void pim_show_mlag_up_detail(struct vrf *vrf,
				    struct vty *vty, const char *src_or_group,
				    const char *group, bool uj)
{
	char src_str[PIM_ADDRSTRLEN];
	char grp_str[PIM_ADDRSTRLEN];
	struct pim_upstream *up;
	struct pim_instance *pim = vrf->info;
	json_object *json = NULL;

	if (uj)
		json = json_object_new_object();
	else
		vty_out(vty,
			"Source          Group           Owner  Local-cost  Peer-cost  DF\n");

	frr_each (rb_pim_upstream, &pim->upstream_head, up) {
		if (!(up->flags & PIM_UPSTREAM_FLAG_MASK_MLAG_PEER)
		    && !(up->flags & PIM_UPSTREAM_FLAG_MASK_MLAG_INTERFACE)
		    && !pim_up_mlag_is_local(up))
			continue;

		snprintfrr(grp_str, sizeof(grp_str), "%pPAs", &up->sg.grp);
		snprintfrr(src_str, sizeof(src_str), "%pPAs", &up->sg.src);

		/* XXX: strcmps are clearly inefficient. we should do uint comps
		 * here instead.
		 */
		if (group) {
			if (strcmp(src_str, src_or_group) ||
			    strcmp(grp_str, group))
				continue;
		} else {
			if (strcmp(src_str, src_or_group) &&
			    strcmp(grp_str, src_or_group))
				continue;
		}
		pim_show_mlag_up_entry_detail(vrf, vty, up,
					      src_str, grp_str, json);
	}

	if (uj)
		vty_json(vty, json);
}

static void pim_show_mlag_up_vrf(struct vrf *vrf, struct vty *vty, bool uj)
{
	json_object *json = NULL;
	json_object *json_row;
	struct pim_upstream *up;
	struct pim_instance *pim = vrf->info;
	json_object *json_group = NULL;

	if (uj) {
		json = json_object_new_object();
	} else {
		vty_out(vty,
			"Source          Group           Owner  Local-cost  Peer-cost  DF\n");
	}

	frr_each (rb_pim_upstream, &pim->upstream_head, up) {
		if (!(up->flags & PIM_UPSTREAM_FLAG_MASK_MLAG_PEER)
		    && !(up->flags & PIM_UPSTREAM_FLAG_MASK_MLAG_INTERFACE)
		    && !pim_up_mlag_is_local(up))
			continue;
		if (uj) {
			char src_str[PIM_ADDRSTRLEN];
			char grp_str[PIM_ADDRSTRLEN];
			json_object *own_list = NULL;

			snprintfrr(grp_str, sizeof(grp_str), "%pPAs",
				   &up->sg.grp);
			snprintfrr(src_str, sizeof(src_str), "%pPAs",
				   &up->sg.src);

			json_object_object_get_ex(json, grp_str, &json_group);
			if (!json_group) {
				json_group = json_object_new_object();
				json_object_object_add(json, grp_str,
						       json_group);
			}

			json_row = json_object_new_object();
			json_object_string_add(json_row, "vrf", vrf->name);
			json_object_string_add(json_row, "source", src_str);
			json_object_string_add(json_row, "group", grp_str);

			own_list = json_object_new_array();
			if (pim_up_mlag_is_local(up)) {

				json_object_array_add(own_list,
						      json_object_new_string(
							      "local"));
			}
			if (up->flags & (PIM_UPSTREAM_FLAG_MASK_MLAG_PEER)) {
				json_object_array_add(own_list,
						      json_object_new_string(
							      "peer"));
			}
			json_object_object_add(json_row, "owners", own_list);

			json_object_int_add(json_row, "localCost",
					    pim_up_mlag_local_cost(up));
			json_object_int_add(json_row, "peerCost",
					    pim_up_mlag_peer_cost(up));
			if (PIM_UPSTREAM_FLAG_TEST_MLAG_NON_DF(up->flags))
				json_object_boolean_false_add(json_row, "df");
			else
				json_object_boolean_true_add(json_row, "df");
			json_object_object_add(json_group, src_str, json_row);
		} else {
			char own_str[6];

			own_str[0] = '\0';
			if (pim_up_mlag_is_local(up))
				strlcat(own_str, "L", sizeof(own_str));
			if (up->flags & (PIM_UPSTREAM_FLAG_MASK_MLAG_PEER))
				strlcat(own_str, "P", sizeof(own_str));
			if (up->flags & (PIM_UPSTREAM_FLAG_MASK_MLAG_INTERFACE))
				strlcat(own_str, "I", sizeof(own_str));
			vty_out(vty,
				"%-15pPAs %-15pPAs %-6s %-11u %-10u %2s\n",
				&up->sg.src, &up->sg.grp, own_str,
				pim_up_mlag_local_cost(up),
				pim_up_mlag_peer_cost(up),
				PIM_UPSTREAM_FLAG_TEST_MLAG_NON_DF(up->flags)
				? "n" : "y");
		}
	}
	if (uj)
		vty_json(vty, json);
}

static void pim_show_mlag_help_string(struct vty *vty, bool uj)
{
	if (!uj) {
		vty_out(vty, "Owner codes:\n");
		vty_out(vty,
			"L: EVPN-MLAG Entry, I:PIM-MLAG Entry, P: Peer Entry\n");
	}
}


DEFUN(show_ip_pim_mlag_up, show_ip_pim_mlag_up_cmd,
      "show ip pim [vrf NAME] mlag upstream [A.B.C.D [A.B.C.D]] [json]",
      SHOW_STR
      IP_STR
      PIM_STR
      VRF_CMD_HELP_STR
      "MLAG\n"
      "upstream\n"
      "Unicast or Multicast address\n"
      "Multicast address\n" JSON_STR)
{
	const char *src_or_group = NULL;
	const char *group = NULL;
	int idx = 2;
	struct vrf *vrf = pim_cmd_lookup_vrf(vty, argv, argc, &idx);
	bool uj = use_json(argc, argv);

	if (!vrf || !vrf->info) {
		vty_out(vty, "%s: VRF or Info missing\n", __func__);
		return CMD_WARNING;
	}

	if (uj)
		argc--;

	if (argv_find(argv, argc, "A.B.C.D", &idx)) {
		src_or_group = argv[idx]->arg;
		if (idx + 1 < argc)
			group = argv[idx + 1]->arg;
	}

	pim_show_mlag_help_string(vty, uj);

	if (src_or_group)
		pim_show_mlag_up_detail(vrf, vty, src_or_group, group, uj);
	else
		pim_show_mlag_up_vrf(vrf, vty, uj);

	return CMD_SUCCESS;
}


DEFUN(show_ip_pim_mlag_up_vrf_all, show_ip_pim_mlag_up_vrf_all_cmd,
      "show ip pim vrf all mlag upstream [json]",
      SHOW_STR IP_STR PIM_STR VRF_CMD_HELP_STR
      "MLAG\n"
      "upstream\n" JSON_STR)
{
	struct vrf *vrf;
	bool uj = use_json(argc, argv);

	pim_show_mlag_help_string(vty, uj);
	RB_FOREACH (vrf, vrf_name_head, &vrfs_by_name) {
		pim_show_mlag_up_vrf(vrf, vty, uj);
	}

	return CMD_SUCCESS;
}

DEFUN (show_ip_pim_neighbor,
       show_ip_pim_neighbor_cmd,
       "show ip pim [vrf NAME] neighbor [detail|WORD] [json]",
       SHOW_STR
       IP_STR
       PIM_STR
       VRF_CMD_HELP_STR
       "PIM neighbor information\n"
       "Detailed output\n"
       "Name of interface or neighbor\n"
       JSON_STR)
{
	int idx = 2;
	struct vrf *vrf = pim_cmd_lookup_vrf(vty, argv, argc, &idx);
	bool uj = use_json(argc, argv);

	if (!vrf)
		return CMD_WARNING;

	if (argv_find(argv, argc, "detail", &idx)
	    || argv_find(argv, argc, "WORD", &idx))
		pim_show_neighbors_single(vrf->info, vty, argv[idx]->arg, uj);
	else
		pim_show_neighbors(vrf->info, vty, uj);

	return CMD_SUCCESS;
}

DEFUN (show_ip_pim_neighbor_vrf_all,
       show_ip_pim_neighbor_vrf_all_cmd,
       "show ip pim vrf all neighbor [detail|WORD] [json]",
       SHOW_STR
       IP_STR
       PIM_STR
       VRF_CMD_HELP_STR
       "PIM neighbor information\n"
       "Detailed output\n"
       "Name of interface or neighbor\n"
       JSON_STR)
{
	int idx = 2;
	bool uj = use_json(argc, argv);
	struct vrf *vrf;
	bool first = true;

	if (uj)
		vty_out(vty, "{ ");
	RB_FOREACH (vrf, vrf_name_head, &vrfs_by_name) {
		if (uj) {
			if (!first)
				vty_out(vty, ", ");
			vty_out(vty, " \"%s\": ", vrf->name);
			first = false;
		} else
			vty_out(vty, "VRF: %s\n", vrf->name);
		if (argv_find(argv, argc, "detail", &idx)
		    || argv_find(argv, argc, "WORD", &idx))
			pim_show_neighbors_single(vrf->info, vty,
						  argv[idx]->arg, uj);
		else
			pim_show_neighbors(vrf->info, vty, uj);
	}
	if (uj)
		vty_out(vty, "}\n");

	return CMD_SUCCESS;
}

DEFUN (show_ip_pim_secondary,
       show_ip_pim_secondary_cmd,
       "show ip pim [vrf NAME] secondary",
       SHOW_STR
       IP_STR
       PIM_STR
       VRF_CMD_HELP_STR
       "PIM neighbor addresses\n")
{
	int idx = 2;
	struct vrf *vrf = pim_cmd_lookup_vrf(vty, argv, argc, &idx);

	if (!vrf)
		return CMD_WARNING;

	pim_show_neighbors_secondary(vrf->info, vty);

	return CMD_SUCCESS;
}

DEFUN (show_ip_pim_state,
       show_ip_pim_state_cmd,
       "show ip pim [vrf NAME] state [A.B.C.D [A.B.C.D]] [json]",
       SHOW_STR
       IP_STR
       PIM_STR
       VRF_CMD_HELP_STR
       "PIM state information\n"
       "Unicast or Multicast address\n"
       "Multicast address\n"
       JSON_STR)
{
	const char *src_or_group = NULL;
	const char *group = NULL;
	int idx = 2;
	struct vrf *vrf = pim_cmd_lookup_vrf(vty, argv, argc, &idx);
	bool uj = use_json(argc, argv);

	if (!vrf)
		return CMD_WARNING;

	if (uj)
		argc--;

	if (argv_find(argv, argc, "A.B.C.D", &idx)) {
		src_or_group = argv[idx]->arg;
		if (idx + 1 < argc)
			group = argv[idx + 1]->arg;
	}

	pim_show_state(vrf->info, vty, src_or_group, group, uj);

	return CMD_SUCCESS;
}

DEFUN (show_ip_pim_state_vrf_all,
       show_ip_pim_state_vrf_all_cmd,
       "show ip pim vrf all state [A.B.C.D [A.B.C.D]] [json]",
       SHOW_STR
       IP_STR
       PIM_STR
       VRF_CMD_HELP_STR
       "PIM state information\n"
       "Unicast or Multicast address\n"
       "Multicast address\n"
       JSON_STR)
{
	const char *src_or_group = NULL;
	const char *group = NULL;
	int idx = 2;
	bool uj = use_json(argc, argv);
	struct vrf *vrf;
	bool first = true;

	if (uj) {
		vty_out(vty, "{ ");
		argc--;
	}

	if (argv_find(argv, argc, "A.B.C.D", &idx)) {
		src_or_group = argv[idx]->arg;
		if (idx + 1 < argc)
			group = argv[idx + 1]->arg;
	}

	RB_FOREACH (vrf, vrf_name_head, &vrfs_by_name) {
		if (uj) {
			if (!first)
				vty_out(vty, ", ");
			vty_out(vty, " \"%s\": ", vrf->name);
			first = false;
		} else
			vty_out(vty, "VRF: %s\n", vrf->name);
		pim_show_state(vrf->info, vty, src_or_group, group, uj);
	}
	if (uj)
		vty_out(vty, "}\n");

	return CMD_SUCCESS;
}

DEFPY (show_ip_pim_upstream,
       show_ip_pim_upstream_cmd,
       "show ip pim [vrf NAME] upstream [A.B.C.D$s_or_g [A.B.C.D$g]] [json$json]",
       SHOW_STR
       IP_STR
       PIM_STR
       VRF_CMD_HELP_STR
       "PIM upstream information\n"
       "The Source or Group\n"
       "The Group\n"
       JSON_STR)
{
	pim_sgaddr sg = {0};
	struct vrf *v;
	bool uj = !!json;
	struct pim_instance *pim;

	v = vrf_lookup_by_name(vrf ? vrf : VRF_DEFAULT_NAME);

	if (!v) {
		vty_out(vty, "%% Vrf specified: %s does not exist\n", vrf);
		return CMD_WARNING;
	}
	pim = pim_get_pim_instance(v->vrf_id);

	if (!pim) {
		vty_out(vty, "%% Unable to find pim instance\n");
		return CMD_WARNING;
	}

	if (s_or_g.s_addr != INADDR_ANY) {
		if (g.s_addr != INADDR_ANY) {
			sg.src = s_or_g;
			sg.grp = g;
		} else
			sg.grp = s_or_g;
	}
	pim_show_upstream(pim, vty, &sg, uj);

	return CMD_SUCCESS;
}

DEFUN (show_ip_pim_upstream_vrf_all,
       show_ip_pim_upstream_vrf_all_cmd,
       "show ip pim vrf all upstream [json]",
       SHOW_STR
       IP_STR
       PIM_STR
       VRF_CMD_HELP_STR
       "PIM upstream information\n"
       JSON_STR)
{
	pim_sgaddr sg = {0};
	bool uj = use_json(argc, argv);
	struct vrf *vrf;
	bool first = true;

	if (uj)
		vty_out(vty, "{ ");
	RB_FOREACH (vrf, vrf_name_head, &vrfs_by_name) {
		if (uj) {
			if (!first)
				vty_out(vty, ", ");
			vty_out(vty, " \"%s\": ", vrf->name);
			first = false;
		} else
			vty_out(vty, "VRF: %s\n", vrf->name);
		pim_show_upstream(vrf->info, vty, &sg, uj);
	}

	return CMD_SUCCESS;
}

DEFUN (show_ip_pim_channel,
       show_ip_pim_channel_cmd,
       "show ip pim [vrf NAME] channel [json]",
       SHOW_STR
       IP_STR
       PIM_STR
       VRF_CMD_HELP_STR
       "PIM downstream channel info\n"
       JSON_STR)
{
	int idx = 2;
	struct vrf *vrf = pim_cmd_lookup_vrf(vty, argv, argc, &idx);
	bool uj = use_json(argc, argv);

	if (!vrf)
		return CMD_WARNING;

	pim_show_channel(vrf->info, vty, uj);

	return CMD_SUCCESS;
}

DEFUN (show_ip_pim_upstream_join_desired,
       show_ip_pim_upstream_join_desired_cmd,
       "show ip pim [vrf NAME] upstream-join-desired [json]",
       SHOW_STR
       IP_STR
       PIM_STR
       VRF_CMD_HELP_STR
       "PIM upstream join-desired\n"
       JSON_STR)
{
	int idx = 2;
	struct vrf *vrf = pim_cmd_lookup_vrf(vty, argv, argc, &idx);
	bool uj = use_json(argc, argv);

	if (!vrf)
		return CMD_WARNING;

	pim_show_join_desired(vrf->info, vty, uj);

	return CMD_SUCCESS;
}

DEFUN (show_ip_pim_upstream_rpf,
       show_ip_pim_upstream_rpf_cmd,
       "show ip pim [vrf NAME] upstream-rpf [json]",
       SHOW_STR
       IP_STR
       PIM_STR
       VRF_CMD_HELP_STR
       "PIM upstream source rpf\n"
       JSON_STR)
{
	int idx = 2;
	struct vrf *vrf = pim_cmd_lookup_vrf(vty, argv, argc, &idx);
	bool uj = use_json(argc, argv);

	if (!vrf)
		return CMD_WARNING;

	pim_show_upstream_rpf(vrf->info, vty, uj);

	return CMD_SUCCESS;
}

DEFUN (show_ip_pim_rp,
       show_ip_pim_rp_cmd,
       "show ip pim [vrf NAME] rp-info [json]",
       SHOW_STR
       IP_STR
       PIM_STR
       VRF_CMD_HELP_STR
       "PIM RP information\n"
       JSON_STR)
{
	int idx = 2;
	struct vrf *vrf = pim_cmd_lookup_vrf(vty, argv, argc, &idx);
	bool uj = use_json(argc, argv);

	if (!vrf)
		return CMD_WARNING;

	pim_rp_show_information(vrf->info, vty, uj);

	return CMD_SUCCESS;
}

DEFUN (show_ip_pim_rp_vrf_all,
       show_ip_pim_rp_vrf_all_cmd,
       "show ip pim vrf all rp-info [json]",
       SHOW_STR
       IP_STR
       PIM_STR
       VRF_CMD_HELP_STR
       "PIM RP information\n"
       JSON_STR)
{
	bool uj = use_json(argc, argv);
	struct vrf *vrf;
	bool first = true;

	if (uj)
		vty_out(vty, "{ ");
	RB_FOREACH (vrf, vrf_name_head, &vrfs_by_name) {
		if (uj) {
			if (!first)
				vty_out(vty, ", ");
			vty_out(vty, " \"%s\": ", vrf->name);
			first = false;
		} else
			vty_out(vty, "VRF: %s\n", vrf->name);
		pim_rp_show_information(vrf->info, vty, uj);
	}
	if (uj)
		vty_out(vty, "}\n");

	return CMD_SUCCESS;
}

DEFUN (show_ip_pim_rpf,
       show_ip_pim_rpf_cmd,
       "show ip pim [vrf NAME] rpf [json]",
       SHOW_STR
       IP_STR
       PIM_STR
       VRF_CMD_HELP_STR
       "PIM cached source rpf information\n"
       JSON_STR)
{
	int idx = 2;
	struct vrf *vrf = pim_cmd_lookup_vrf(vty, argv, argc, &idx);
	bool uj = use_json(argc, argv);

	if (!vrf)
		return CMD_WARNING;

	pim_show_rpf(vrf->info, vty, uj);

	return CMD_SUCCESS;
}

DEFUN (show_ip_pim_rpf_vrf_all,
       show_ip_pim_rpf_vrf_all_cmd,
       "show ip pim vrf all rpf [json]",
       SHOW_STR
       IP_STR
       PIM_STR
       VRF_CMD_HELP_STR
       "PIM cached source rpf information\n"
       JSON_STR)
{
	bool uj = use_json(argc, argv);
	struct vrf *vrf;
	bool first = true;

	if (uj)
		vty_out(vty, "{ ");
	RB_FOREACH (vrf, vrf_name_head, &vrfs_by_name) {
		if (uj) {
			if (!first)
				vty_out(vty, ", ");
			vty_out(vty, " \"%s\": ", vrf->name);
			first = false;
		} else
			vty_out(vty, "VRF: %s\n", vrf->name);
		pim_show_rpf(vrf->info, vty, uj);
	}
	if (uj)
		vty_out(vty, "}\n");

	return CMD_SUCCESS;
}

DEFUN (show_ip_pim_nexthop,
       show_ip_pim_nexthop_cmd,
       "show ip pim [vrf NAME] nexthop",
       SHOW_STR
       IP_STR
       PIM_STR
       VRF_CMD_HELP_STR
       "PIM cached nexthop rpf information\n")
{
	int idx = 2;
	struct vrf *vrf = pim_cmd_lookup_vrf(vty, argv, argc, &idx);

	if (!vrf)
		return CMD_WARNING;

	pim_show_nexthop(vrf->info, vty);

	return CMD_SUCCESS;
}

DEFUN (show_ip_pim_nexthop_lookup,
       show_ip_pim_nexthop_lookup_cmd,
       "show ip pim [vrf NAME] nexthop-lookup A.B.C.D A.B.C.D",
       SHOW_STR
       IP_STR
       PIM_STR
       VRF_CMD_HELP_STR
       "PIM cached nexthop rpf lookup\n"
       "Source/RP address\n"
       "Multicast Group address\n")
{
	struct prefix nht_p;
	int result = 0;
	struct in_addr src_addr, grp_addr;
	struct in_addr vif_source;
	const char *addr_str, *addr_str1;
	struct prefix grp;
	struct pim_nexthop nexthop;
	char nexthop_addr_str[PREFIX_STRLEN];
	char grp_str[PREFIX_STRLEN];
	int idx = 2;
	struct vrf *vrf = pim_cmd_lookup_vrf(vty, argv, argc, &idx);

	if (!vrf)
		return CMD_WARNING;

	argv_find(argv, argc, "A.B.C.D", &idx);
	addr_str = argv[idx]->arg;
	result = inet_pton(AF_INET, addr_str, &src_addr);
	if (result <= 0) {
		vty_out(vty, "Bad unicast address %s: errno=%d: %s\n", addr_str,
			errno, safe_strerror(errno));
		return CMD_WARNING;
	}

	if (pim_is_group_224_4(src_addr)) {
		vty_out(vty,
			"Invalid argument. Expected Valid Source Address.\n");
		return CMD_WARNING;
	}

	addr_str1 = argv[idx + 1]->arg;
	result = inet_pton(AF_INET, addr_str1, &grp_addr);
	if (result <= 0) {
		vty_out(vty, "Bad unicast address %s: errno=%d: %s\n", addr_str,
			errno, safe_strerror(errno));
		return CMD_WARNING;
	}

	if (!pim_is_group_224_4(grp_addr)) {
		vty_out(vty,
			"Invalid argument. Expected Valid Multicast Group Address.\n");
		return CMD_WARNING;
	}

	if (!pim_rp_set_upstream_addr(vrf->info, &vif_source, src_addr,
				      grp_addr))
		return CMD_SUCCESS;

	nht_p.family = AF_INET;
	nht_p.prefixlen = IPV4_MAX_BITLEN;
	nht_p.u.prefix4 = vif_source;
	grp.family = AF_INET;
	grp.prefixlen = IPV4_MAX_BITLEN;
	grp.u.prefix4 = grp_addr;
	memset(&nexthop, 0, sizeof(nexthop));

	result = pim_ecmp_nexthop_lookup(vrf->info, &nexthop, &nht_p, &grp, 0);

	if (!result) {
		vty_out(vty,
			"Nexthop Lookup failed, no usable routes returned.\n");
		return CMD_SUCCESS;
	}

	pim_addr_dump("<grp?>", &grp, grp_str, sizeof(grp_str));
	pim_addr_dump("<nexthop?>", &nexthop.mrib_nexthop_addr,
		      nexthop_addr_str, sizeof(nexthop_addr_str));
	vty_out(vty, "Group %s --- Nexthop %s Interface %s \n", grp_str,
		nexthop_addr_str, nexthop.interface->name);

	return CMD_SUCCESS;
}

DEFUN (show_ip_pim_interface_traffic,
       show_ip_pim_interface_traffic_cmd,
       "show ip pim [vrf NAME] interface traffic [WORD] [json]",
       SHOW_STR
       IP_STR
       PIM_STR
       VRF_CMD_HELP_STR
       "PIM interface information\n"
       "Protocol Packet counters\n"
       "Interface name\n"
       JSON_STR)
{
	int idx = 2;
	struct vrf *vrf = pim_cmd_lookup_vrf(vty, argv, argc, &idx);
	bool uj = use_json(argc, argv);

	if (!vrf)
		return CMD_WARNING;

	if (argv_find(argv, argc, "WORD", &idx))
		pim_show_interface_traffic_single(vrf->info, vty,
						  argv[idx]->arg, uj);
	else
		pim_show_interface_traffic(vrf->info, vty, uj);

	return CMD_SUCCESS;
}

DEFUN (show_ip_pim_bsm_db,
       show_ip_pim_bsm_db_cmd,
       "show ip pim bsm-database [vrf NAME] [json]",
       SHOW_STR
       IP_STR
       PIM_STR
       "PIM cached bsm packets information\n"
       VRF_CMD_HELP_STR
       JSON_STR)
{
	int idx = 2;
	struct vrf *vrf = pim_cmd_lookup_vrf(vty, argv, argc, &idx);
	bool uj = use_json(argc, argv);

	if (!vrf)
		return CMD_WARNING;

	pim_show_bsm_db(vrf->info, vty, uj);
	return CMD_SUCCESS;
}

DEFUN (show_ip_pim_bsrp,
       show_ip_pim_bsrp_cmd,
       "show ip pim bsrp-info [vrf NAME] [json]",
       SHOW_STR
       IP_STR
       PIM_STR
       "PIM cached group-rp mappings information\n"
       VRF_CMD_HELP_STR
       JSON_STR)
{
	int idx = 2;
	struct vrf *vrf = pim_cmd_lookup_vrf(vty, argv, argc, &idx);
	bool uj = use_json(argc, argv);

	if (!vrf)
		return CMD_WARNING;

	pim_show_group_rp_mappings_info(vrf->info, vty, uj);

	return CMD_SUCCESS;
}

DEFUN (show_ip_pim_statistics,
       show_ip_pim_statistics_cmd,
       "show ip pim [vrf NAME] statistics [interface WORD] [json]",
       SHOW_STR
       IP_STR
       PIM_STR
       VRF_CMD_HELP_STR
       "PIM statistics\n"
       INTERFACE_STR
       "PIM interface\n"
       JSON_STR)
{
	int idx = 2;
	struct vrf *vrf = pim_cmd_lookup_vrf(vty, argv, argc, &idx);
	bool uj = use_json(argc, argv);

	if (!vrf)
		return CMD_WARNING;

	if (argv_find(argv, argc, "WORD", &idx))
		pim_show_statistics(vrf->info, vty, argv[idx]->arg, uj);
	else
		pim_show_statistics(vrf->info, vty, NULL, uj);

	return CMD_SUCCESS;
}

static void show_multicast_interfaces(struct pim_instance *pim, struct vty *vty,
				      bool uj)
{
	struct interface *ifp;
	char buf[PREFIX_STRLEN];
	json_object *json = NULL;
	json_object *json_row = NULL;

	vty_out(vty, "\n");

	if (uj)
		json = json_object_new_object();
	else
		vty_out(vty,
			"Interface        Address            ifi Vif  PktsIn PktsOut    BytesIn   BytesOut\n");

	FOR_ALL_INTERFACES (pim->vrf, ifp) {
		struct pim_interface *pim_ifp;
		struct in_addr ifaddr;
		struct sioc_vif_req vreq;

		pim_ifp = ifp->info;

		if (!pim_ifp)
			continue;

		memset(&vreq, 0, sizeof(vreq));
		vreq.vifi = pim_ifp->mroute_vif_index;

		if (ioctl(pim->mroute_socket, SIOCGETVIFCNT, &vreq)) {
			zlog_warn(
				"ioctl(SIOCGETVIFCNT=%lu) failure for interface %s vif_index=%d: errno=%d: %s",
				(unsigned long)SIOCGETVIFCNT, ifp->name,
				pim_ifp->mroute_vif_index, errno,
				safe_strerror(errno));
		}

		ifaddr = pim_ifp->primary_address;
		if (uj) {
			json_row = json_object_new_object();
			json_object_string_add(json_row, "name", ifp->name);
			json_object_string_add(json_row, "state",
					       if_is_up(ifp) ? "up" : "down");
			json_object_string_addf(json_row, "address", "%pI4",
						&pim_ifp->primary_address);
			json_object_int_add(json_row, "ifIndex", ifp->ifindex);
			json_object_int_add(json_row, "vif",
					    pim_ifp->mroute_vif_index);
			json_object_int_add(json_row, "pktsIn",
					    (unsigned long)vreq.icount);
			json_object_int_add(json_row, "pktsOut",
					    (unsigned long)vreq.ocount);
			json_object_int_add(json_row, "bytesIn",
					    (unsigned long)vreq.ibytes);
			json_object_int_add(json_row, "bytesOut",
					    (unsigned long)vreq.obytes);
			json_object_object_add(json, ifp->name, json_row);
		} else {
			vty_out(vty,
				"%-16s %-15s %3d %3d %7lu %7lu %10lu %10lu\n",
				ifp->name,
				inet_ntop(AF_INET, &ifaddr, buf, sizeof(buf)),
				ifp->ifindex, pim_ifp->mroute_vif_index,
				(unsigned long)vreq.icount,
				(unsigned long)vreq.ocount,
				(unsigned long)vreq.ibytes,
				(unsigned long)vreq.obytes);
		}
	}

	if (uj)
		vty_json(vty, json);
}

static void pim_cmd_show_ip_multicast_helper(struct pim_instance *pim,
					     struct vty *vty)
{
	struct vrf *vrf = pim->vrf;
	time_t now = pim_time_monotonic_sec();
	char uptime[10];
	char mlag_role[80];

	pim = vrf->info;

	vty_out(vty, "Router MLAG Role: %s\n",
		mlag_role2str(router->mlag_role, mlag_role, sizeof(mlag_role)));
	vty_out(vty, "Mroute socket descriptor:");

	vty_out(vty, " %d(%s)\n", pim->mroute_socket, vrf->name);

	pim_time_uptime(uptime, sizeof(uptime),
			now - pim->mroute_socket_creation);
	vty_out(vty, "Mroute socket uptime: %s\n", uptime);

	vty_out(vty, "\n");

	pim_zebra_zclient_update(vty);
	pim_zlookup_show_ip_multicast(vty);

	vty_out(vty, "\n");
	vty_out(vty, "Maximum highest VifIndex: %d\n", PIM_MAX_USABLE_VIFS);

	vty_out(vty, "\n");
	vty_out(vty, "Upstream Join Timer: %d secs\n", router->t_periodic);
	vty_out(vty, "Join/Prune Holdtime: %d secs\n", PIM_JP_HOLDTIME);
	vty_out(vty, "PIM ECMP: %s\n", pim->ecmp_enable ? "Enable" : "Disable");
	vty_out(vty, "PIM ECMP Rebalance: %s\n",
		pim->ecmp_rebalance_enable ? "Enable" : "Disable");

	vty_out(vty, "\n");

	show_rpf_refresh_stats(vty, pim, now, NULL);

	vty_out(vty, "\n");

	show_scan_oil_stats(pim, vty, now);

	show_multicast_interfaces(pim, vty, false);
}

DEFUN (show_ip_multicast,
       show_ip_multicast_cmd,
       "show ip multicast [vrf NAME]",
       SHOW_STR
       IP_STR
       VRF_CMD_HELP_STR
       "Multicast global information\n")
{
	int idx = 2;
	struct vrf *vrf = pim_cmd_lookup_vrf(vty, argv, argc, &idx);

	if (!vrf)
		return CMD_WARNING;

	pim_cmd_show_ip_multicast_helper(vrf->info, vty);

	return CMD_SUCCESS;
}

DEFUN (show_ip_multicast_vrf_all,
       show_ip_multicast_vrf_all_cmd,
       "show ip multicast vrf all",
       SHOW_STR
       IP_STR
       VRF_CMD_HELP_STR
       "Multicast global information\n")
{
	bool uj = use_json(argc, argv);
	struct vrf *vrf;
	bool first = true;

	if (uj)
		vty_out(vty, "{ ");
	RB_FOREACH (vrf, vrf_name_head, &vrfs_by_name) {
		if (uj) {
			if (!first)
				vty_out(vty, ", ");
			vty_out(vty, " \"%s\": ", vrf->name);
			first = false;
		} else
			vty_out(vty, "VRF: %s\n", vrf->name);
		pim_cmd_show_ip_multicast_helper(vrf->info, vty);
	}
	if (uj)
		vty_out(vty, "}\n");

	return CMD_SUCCESS;
}

DEFUN(show_ip_multicast_count,
      show_ip_multicast_count_cmd,
      "show ip multicast count [vrf NAME] [json]",
      SHOW_STR IP_STR
      "Multicast global information\n"
      "Data packet count\n"
      VRF_CMD_HELP_STR JSON_STR)
{
	int idx = 3;
	struct vrf *vrf = pim_cmd_lookup_vrf(vty, argv, argc, &idx);
	bool uj = use_json(argc, argv);

	if (!vrf)
		return CMD_WARNING;

	show_multicast_interfaces(vrf->info, vty, uj);

	return CMD_SUCCESS;
}

DEFUN(show_ip_multicast_count_vrf_all,
      show_ip_multicast_count_vrf_all_cmd,
      "show ip multicast count vrf all [json]",
      SHOW_STR IP_STR
      "Multicast global information\n"
      "Data packet count\n"
      VRF_CMD_HELP_STR JSON_STR)
{
	bool uj = use_json(argc, argv);
	struct vrf *vrf;
	bool first = true;

	if (uj)
		vty_out(vty, "{ ");

	RB_FOREACH (vrf, vrf_name_head, &vrfs_by_name) {
		if (uj) {
			if (!first)
				vty_out(vty, ", ");

			vty_out(vty, " \"%s\": ", vrf->name);
			first = false;
		} else
			vty_out(vty, "VRF: %s\n", vrf->name);

		show_multicast_interfaces(vrf->info, vty, uj);
	}

	if (uj)
		vty_out(vty, "}\n");

	return CMD_SUCCESS;
}

static void show_mroute(struct pim_instance *pim, struct vty *vty,
			pim_sgaddr *sg, bool fill, bool uj)
{
	struct listnode *node;
	struct channel_oil *c_oil;
	struct static_route *s_route;
	time_t now;
	json_object *json = NULL;
	json_object *json_group = NULL;
	json_object *json_source = NULL;
	json_object *json_oil = NULL;
	json_object *json_ifp_out = NULL;
	int found_oif;
	int first;
	char grp_str[INET_ADDRSTRLEN];
	char src_str[INET_ADDRSTRLEN];
	char in_ifname[INTERFACE_NAMSIZ + 1];
	char out_ifname[INTERFACE_NAMSIZ + 1];
	int oif_vif_index;
	struct interface *ifp_in;
	char proto[100];
	char state_str[PIM_REG_STATE_STR_LEN];
	char mroute_uptime[10];

	if (uj) {
		json = json_object_new_object();
	} else {
		vty_out(vty, "IP Multicast Routing Table\n");
		vty_out(vty, "Flags: S - Sparse, C - Connected, P - Pruned\n");
		vty_out(vty,
			"       R - SGRpt Pruned, F - Register flag, T - SPT-bit set\n");
		vty_out(vty,
			"\nSource          Group           Flags    Proto  Input            Output           TTL  Uptime\n");
	}

	now = pim_time_monotonic_sec();

	/* print list of PIM and IGMP routes */
	frr_each (rb_pim_oil, &pim->channel_oil_head, c_oil) {
		found_oif = 0;
		first = 1;
		if (!c_oil->installed)
			continue;

		if (!pim_addr_is_any(sg->grp) &&
		    pim_addr_cmp(sg->grp, c_oil->oil.mfcc_mcastgrp))
			continue;
		if (!pim_addr_is_any(sg->src) &&
		    pim_addr_cmp(sg->src, c_oil->oil.mfcc_origin))
			continue;

		pim_inet4_dump("<group?>", c_oil->oil.mfcc_mcastgrp, grp_str,
			       sizeof(grp_str));
		pim_inet4_dump("<source?>", c_oil->oil.mfcc_origin, src_str,
			       sizeof(src_str));

		strlcpy(state_str, "S", sizeof(state_str));
		/* When a non DR receives a igmp join, it creates a (*,G)
		 * channel_oil without any upstream creation */
		if (c_oil->up) {
			if (PIM_UPSTREAM_FLAG_TEST_SRC_IGMP(c_oil->up->flags))
				strlcat(state_str, "C", sizeof(state_str));
			if (pim_upstream_is_sg_rpt(c_oil->up))
				strlcat(state_str, "R", sizeof(state_str));
			if (PIM_UPSTREAM_FLAG_TEST_FHR(c_oil->up->flags))
				strlcat(state_str, "F", sizeof(state_str));
			if (c_oil->up->sptbit == PIM_UPSTREAM_SPTBIT_TRUE)
				strlcat(state_str, "T", sizeof(state_str));
		}
		if (pim_channel_oil_empty(c_oil))
			strlcat(state_str, "P", sizeof(state_str));

		ifp_in = pim_if_find_by_vif_index(pim, c_oil->oil.mfcc_parent);

		if (ifp_in)
			strlcpy(in_ifname, ifp_in->name, sizeof(in_ifname));
		else
			strlcpy(in_ifname, "<iif?>", sizeof(in_ifname));


		pim_time_uptime(mroute_uptime, sizeof(mroute_uptime),
				now - c_oil->mroute_creation);

		if (uj) {

			/* Find the group, create it if it doesn't exist */
			json_object_object_get_ex(json, grp_str, &json_group);

			if (!json_group) {
				json_group = json_object_new_object();
				json_object_object_add(json, grp_str,
						       json_group);
			}

			/* Find the source nested under the group, create it if
			 * it doesn't exist
			 */
			json_object_object_get_ex(json_group, src_str,
						  &json_source);

			if (!json_source) {
				json_source = json_object_new_object();
				json_object_object_add(json_group, src_str,
						       json_source);
			}

			/* Find the inbound interface nested under the source,
			 * create it if it doesn't exist */
			json_object_int_add(json_source, "installed",
					    c_oil->installed);
			json_object_int_add(json_source, "refCount",
					    c_oil->oil_ref_count);
			json_object_int_add(json_source, "oilSize",
					    c_oil->oil_size);
			json_object_int_add(json_source, "OilInheritedRescan",
					    c_oil->oil_inherited_rescan);
			json_object_string_add(json_source, "iif", in_ifname);
			json_object_string_add(json_source, "upTime",
					       mroute_uptime);
			json_oil = NULL;
		}

		for (oif_vif_index = 0; oif_vif_index < MAXVIFS;
		     ++oif_vif_index) {
			struct interface *ifp_out;
			int ttl;

			ttl = c_oil->oil.mfcc_ttls[oif_vif_index];
			if (ttl < 1)
				continue;

			/* do not display muted OIFs */
			if (c_oil->oif_flags[oif_vif_index]
			    & PIM_OIF_FLAG_MUTE)
				continue;

			if (c_oil->oil.mfcc_parent == oif_vif_index &&
			    !pim_mroute_allow_iif_in_oil(c_oil,
							 oif_vif_index))
				continue;

			ifp_out = pim_if_find_by_vif_index(pim, oif_vif_index);
			found_oif = 1;

			if (ifp_out)
				strlcpy(out_ifname, ifp_out->name, sizeof(out_ifname));
			else
				strlcpy(out_ifname, "<oif?>", sizeof(out_ifname));

			if (uj) {
				json_ifp_out = json_object_new_object();
				json_object_string_add(json_ifp_out, "source",
						       src_str);
				json_object_string_add(json_ifp_out, "group",
						       grp_str);

				if (c_oil->oif_flags[oif_vif_index]
				    & PIM_OIF_FLAG_PROTO_PIM)
					json_object_boolean_true_add(
						json_ifp_out, "protocolPim");

				if (c_oil->oif_flags[oif_vif_index]
				    & PIM_OIF_FLAG_PROTO_IGMP)
					json_object_boolean_true_add(
						json_ifp_out, "protocolIgmp");

				if (c_oil->oif_flags[oif_vif_index]
				    & PIM_OIF_FLAG_PROTO_VXLAN)
					json_object_boolean_true_add(
						json_ifp_out, "protocolVxlan");

				if (c_oil->oif_flags[oif_vif_index]
				    & PIM_OIF_FLAG_PROTO_STAR)
					json_object_boolean_true_add(
						json_ifp_out,
						"protocolInherited");

				json_object_string_add(json_ifp_out,
						       "inboundInterface",
						       in_ifname);
				json_object_int_add(json_ifp_out, "iVifI",
						    c_oil->oil.mfcc_parent);
				json_object_string_add(json_ifp_out,
						       "outboundInterface",
						       out_ifname);
				json_object_int_add(json_ifp_out, "oVifI",
						    oif_vif_index);
				json_object_int_add(json_ifp_out, "ttl", ttl);
				json_object_string_add(json_ifp_out, "upTime",
						       mroute_uptime);
				json_object_string_add(json_source, "flags",
						       state_str);
				if (!json_oil) {
					json_oil = json_object_new_object();
					json_object_object_add(json_source,
							       "oil", json_oil);
				}
				json_object_object_add(json_oil, out_ifname,
						       json_ifp_out);
			} else {
				proto[0] = '\0';
				if (c_oil->oif_flags[oif_vif_index]
				    & PIM_OIF_FLAG_PROTO_PIM) {
					strlcpy(proto, "PIM", sizeof(proto));
				}

				if (c_oil->oif_flags[oif_vif_index]
				    & PIM_OIF_FLAG_PROTO_IGMP) {
					strlcpy(proto, "IGMP", sizeof(proto));
				}

				if (c_oil->oif_flags[oif_vif_index]
				    & PIM_OIF_FLAG_PROTO_VXLAN) {
					strlcpy(proto, "VxLAN", sizeof(proto));
				}

				if (c_oil->oif_flags[oif_vif_index]
				    & PIM_OIF_FLAG_PROTO_STAR) {
					strlcpy(proto, "STAR", sizeof(proto));
				}

				vty_out(vty,
					"%-15s %-15s %-8s %-6s %-16s %-16s %-3d  %8s\n",
					src_str, grp_str, state_str, proto,
					in_ifname, out_ifname, ttl,
					mroute_uptime);

				if (first) {
					src_str[0] = '\0';
					grp_str[0] = '\0';
					in_ifname[0] = '\0';
					state_str[0] = '\0';
					mroute_uptime[0] = '\0';
					first = 0;
				}
			}
		}

		if (!uj && !found_oif) {
			vty_out(vty,
				"%-15s %-15s %-8s %-6s %-16s %-16s %-3d  %8s\n",
				src_str, grp_str, state_str, "none", in_ifname,
				"none", 0, "--:--:--");
		}
	}

	/* Print list of static routes */
	for (ALL_LIST_ELEMENTS_RO(pim->static_routes, node, s_route)) {
		first = 1;

		if (!s_route->c_oil.installed)
			continue;

		pim_inet4_dump("<group?>", s_route->group, grp_str,
			       sizeof(grp_str));
		pim_inet4_dump("<source?>", s_route->source, src_str,
			       sizeof(src_str));
		ifp_in = pim_if_find_by_vif_index(pim, s_route->iif);
		found_oif = 0;

		if (ifp_in)
			strlcpy(in_ifname, ifp_in->name, sizeof(in_ifname));
		else
			strlcpy(in_ifname, "<iif?>", sizeof(in_ifname));

		if (uj) {

			/* Find the group, create it if it doesn't exist */
			json_object_object_get_ex(json, grp_str, &json_group);

			if (!json_group) {
				json_group = json_object_new_object();
				json_object_object_add(json, grp_str,
						       json_group);
			}

			/* Find the source nested under the group, create it if
			 * it doesn't exist */
			json_object_object_get_ex(json_group, src_str,
						  &json_source);

			if (!json_source) {
				json_source = json_object_new_object();
				json_object_object_add(json_group, src_str,
						       json_source);
			}

			json_object_string_add(json_source, "iif", in_ifname);
			json_oil = NULL;
		} else {
			strlcpy(proto, "STATIC", sizeof(proto));
		}

		for (oif_vif_index = 0; oif_vif_index < MAXVIFS;
		     ++oif_vif_index) {
			struct interface *ifp_out;
			char oif_uptime[10];
			int ttl;

			ttl = s_route->oif_ttls[oif_vif_index];
			if (ttl < 1)
				continue;

			ifp_out = pim_if_find_by_vif_index(pim, oif_vif_index);
			pim_time_uptime(
				oif_uptime, sizeof(oif_uptime),
				now
				- s_route->c_oil
				.oif_creation[oif_vif_index]);
			found_oif = 1;

			if (ifp_out)
				strlcpy(out_ifname, ifp_out->name, sizeof(out_ifname));
			else
				strlcpy(out_ifname, "<oif?>", sizeof(out_ifname));

			if (uj) {
				json_ifp_out = json_object_new_object();
				json_object_string_add(json_ifp_out, "source",
						       src_str);
				json_object_string_add(json_ifp_out, "group",
						       grp_str);
				json_object_boolean_true_add(json_ifp_out,
							     "protocolStatic");
				json_object_string_add(json_ifp_out,
						       "inboundInterface",
						       in_ifname);
				json_object_int_add(
					json_ifp_out, "iVifI",
					s_route->c_oil.oil.mfcc_parent);
				json_object_string_add(json_ifp_out,
						       "outboundInterface",
						       out_ifname);
				json_object_int_add(json_ifp_out, "oVifI",
						    oif_vif_index);
				json_object_int_add(json_ifp_out, "ttl", ttl);
				json_object_string_add(json_ifp_out, "upTime",
						       oif_uptime);
				if (!json_oil) {
					json_oil = json_object_new_object();
					json_object_object_add(json_source,
							       "oil", json_oil);
				}
				json_object_object_add(json_oil, out_ifname,
						       json_ifp_out);
			} else {
				vty_out(vty,
					"%-15s %-15s %-8s %-6s %-16s %-16s %-3d  %8s\n",
					src_str, grp_str, "-", proto, in_ifname,
					out_ifname, ttl, oif_uptime);
				if (first && !fill) {
					src_str[0] = '\0';
					grp_str[0] = '\0';
					in_ifname[0] = '\0';
					first = 0;
				}
			}
		}

		if (!uj && !found_oif) {
			vty_out(vty,
				"%-15s %-15s %-8s %-6s %-16s %-16s %-3d  %8s\n",
				src_str, grp_str, "-", proto, in_ifname, "none",
				0, "--:--:--");
		}
	}

	if (uj)
		vty_json(vty, json);
}

DEFPY (show_ip_mroute,
       show_ip_mroute_cmd,
       "show ip mroute [vrf NAME] [A.B.C.D$s_or_g [A.B.C.D$g]] [fill$fill] [json$json]",
       SHOW_STR
       IP_STR
       MROUTE_STR
       VRF_CMD_HELP_STR
       "The Source or Group\n"
       "The Group\n"
       "Fill in Assumed data\n"
       JSON_STR)
{
	pim_sgaddr sg = {0};
	struct pim_instance *pim;
	struct vrf *v;

	v = vrf_lookup_by_name(vrf ? vrf : VRF_DEFAULT_NAME);

	if (!v) {
		vty_out(vty, "%% Vrf specified: %s does not exist\n", vrf);
		return CMD_WARNING;
	}
	pim = pim_get_pim_instance(v->vrf_id);

	if (!pim) {
		vty_out(vty, "%% Unable to find pim instance\n");
		return CMD_WARNING;
	}

	if (s_or_g.s_addr != INADDR_ANY) {
		if (g.s_addr != INADDR_ANY) {
			sg.src = s_or_g;
			sg.grp = g;
		} else
			sg.grp = s_or_g;
	}
	show_mroute(pim, vty, &sg, !!fill, !!json);
	return CMD_SUCCESS;
}

DEFUN (show_ip_mroute_vrf_all,
       show_ip_mroute_vrf_all_cmd,
       "show ip mroute vrf all [fill] [json]",
       SHOW_STR
       IP_STR
       MROUTE_STR
       VRF_CMD_HELP_STR
       "Fill in Assumed data\n"
       JSON_STR)
{
	pim_sgaddr sg = {0};
	bool uj = use_json(argc, argv);
	int idx = 4;
	struct vrf *vrf;
	bool first = true;
	bool fill = false;

	if (argv_find(argv, argc, "fill", &idx))
		fill = true;

	if (uj)
		vty_out(vty, "{ ");
	RB_FOREACH (vrf, vrf_name_head, &vrfs_by_name) {
		if (uj) {
			if (!first)
				vty_out(vty, ", ");
			vty_out(vty, " \"%s\": ", vrf->name);
			first = false;
		} else
			vty_out(vty, "VRF: %s\n", vrf->name);
		show_mroute(vrf->info, vty, &sg, fill, uj);
	}
	if (uj)
		vty_out(vty, "}\n");

	return CMD_SUCCESS;
}

DEFUN (clear_ip_mroute_count,
       clear_ip_mroute_count_cmd,
       "clear ip mroute [vrf NAME] count",
       CLEAR_STR
       IP_STR
       MROUTE_STR
       VRF_CMD_HELP_STR
       "Route and packet count data\n")
{
	int idx = 2;
	struct listnode *node;
	struct channel_oil *c_oil;
	struct static_route *sr;
	struct vrf *vrf = pim_cmd_lookup_vrf(vty, argv, argc, &idx);
	struct pim_instance *pim;

	if (!vrf)
		return CMD_WARNING;

	pim = vrf->info;
	frr_each(rb_pim_oil, &pim->channel_oil_head, c_oil) {
		if (!c_oil->installed)
			continue;

		pim_mroute_update_counters(c_oil);
		c_oil->cc.origpktcnt = c_oil->cc.pktcnt;
		c_oil->cc.origbytecnt = c_oil->cc.bytecnt;
		c_oil->cc.origwrong_if = c_oil->cc.wrong_if;
	}

	for (ALL_LIST_ELEMENTS_RO(pim->static_routes, node, sr)) {
		if (!sr->c_oil.installed)
			continue;

		pim_mroute_update_counters(&sr->c_oil);

		sr->c_oil.cc.origpktcnt = sr->c_oil.cc.pktcnt;
		sr->c_oil.cc.origbytecnt = sr->c_oil.cc.bytecnt;
		sr->c_oil.cc.origwrong_if = sr->c_oil.cc.wrong_if;
	}
	return CMD_SUCCESS;
}

static void show_mroute_count_per_channel_oil(struct channel_oil *c_oil,
					      json_object *json,
					      struct vty *vty)
{
	char group_str[INET_ADDRSTRLEN];
	char source_str[INET_ADDRSTRLEN];
	json_object *json_group = NULL;
	json_object *json_source = NULL;

	if (!c_oil->installed)
		return;

	pim_mroute_update_counters(c_oil);

	pim_inet4_dump("<group?>", c_oil->oil.mfcc_mcastgrp, group_str,
		       sizeof(group_str));
	pim_inet4_dump("<source?>", c_oil->oil.mfcc_origin, source_str,
		       sizeof(source_str));

	if (json) {
		json_object_object_get_ex(json, group_str, &json_group);

		if (!json_group) {
			json_group = json_object_new_object();
			json_object_object_add(json, group_str, json_group);
		}

		json_source = json_object_new_object();
		json_object_object_add(json_group, source_str, json_source);
		json_object_int_add(json_source, "lastUsed",
				    c_oil->cc.lastused / 100);
		json_object_int_add(json_source, "packets", c_oil->cc.pktcnt);
		json_object_int_add(json_source, "bytes", c_oil->cc.bytecnt);
		json_object_int_add(json_source, "wrongIf", c_oil->cc.wrong_if);

	} else {
		vty_out(vty, "%-15s %-15s %-8llu %-7ld %-10ld %-7ld\n",
			source_str, group_str, c_oil->cc.lastused / 100,
			c_oil->cc.pktcnt - c_oil->cc.origpktcnt,
			c_oil->cc.bytecnt - c_oil->cc.origbytecnt,
			c_oil->cc.wrong_if - c_oil->cc.origwrong_if);
	}
}

static void show_mroute_count(struct pim_instance *pim, struct vty *vty,
			      bool uj)
{
	struct listnode *node;
	struct channel_oil *c_oil;
	struct static_route *sr;
	json_object *json = NULL;

	if (uj)
		json = json_object_new_object();
	else {
		vty_out(vty, "\n");

		vty_out(vty,
			"Source          Group           LastUsed Packets Bytes WrongIf  \n");
	}

	/* Print PIM and IGMP route counts */
	frr_each (rb_pim_oil, &pim->channel_oil_head, c_oil)
		show_mroute_count_per_channel_oil(c_oil, json, vty);

	for (ALL_LIST_ELEMENTS_RO(pim->static_routes, node, sr))
		show_mroute_count_per_channel_oil(&sr->c_oil, json, vty);

	if (uj)
		vty_json(vty, json);
}

DEFUN (show_ip_mroute_count,
       show_ip_mroute_count_cmd,
       "show ip mroute [vrf NAME] count [json]",
       SHOW_STR
       IP_STR
       MROUTE_STR
       VRF_CMD_HELP_STR
       "Route and packet count data\n"
       JSON_STR)
{
	int idx = 2;
	bool uj = use_json(argc, argv);
	struct vrf *vrf = pim_cmd_lookup_vrf(vty, argv, argc, &idx);

	if (!vrf)
		return CMD_WARNING;

	show_mroute_count(vrf->info, vty, uj);
	return CMD_SUCCESS;
}

DEFUN (show_ip_mroute_count_vrf_all,
       show_ip_mroute_count_vrf_all_cmd,
       "show ip mroute vrf all count [json]",
       SHOW_STR
       IP_STR
       MROUTE_STR
       VRF_CMD_HELP_STR
       "Route and packet count data\n"
       JSON_STR)
{
	bool uj = use_json(argc, argv);
	struct vrf *vrf;
	bool first = true;

	if (uj)
		vty_out(vty, "{ ");
	RB_FOREACH (vrf, vrf_name_head, &vrfs_by_name) {
		if (uj) {
			if (!first)
				vty_out(vty, ", ");
			vty_out(vty, " \"%s\": ", vrf->name);
			first = false;
		} else
			vty_out(vty, "VRF: %s\n", vrf->name);
		show_mroute_count(vrf->info, vty, uj);
	}
	if (uj)
		vty_out(vty, "}\n");

	return CMD_SUCCESS;
}

static void show_mroute_summary(struct pim_instance *pim, struct vty *vty,
				json_object *json)
{
	struct listnode *node;
	struct channel_oil *c_oil;
	struct static_route *s_route;
	uint32_t starg_sw_mroute_cnt = 0;
	uint32_t sg_sw_mroute_cnt = 0;
	uint32_t starg_hw_mroute_cnt = 0;
	uint32_t sg_hw_mroute_cnt = 0;
	json_object *json_starg = NULL;
	json_object *json_sg = NULL;

	if (!json)
		vty_out(vty, "Mroute Type    Installed/Total\n");

	frr_each (rb_pim_oil, &pim->channel_oil_head, c_oil) {
		if (!c_oil->installed) {
			if (c_oil->oil.mfcc_origin.s_addr == INADDR_ANY)
				starg_sw_mroute_cnt++;
			else
				sg_sw_mroute_cnt++;
		} else {
			if (c_oil->oil.mfcc_origin.s_addr == INADDR_ANY)
				starg_hw_mroute_cnt++;
			else
				sg_hw_mroute_cnt++;
		}
	}

	for (ALL_LIST_ELEMENTS_RO(pim->static_routes, node, s_route)) {
		if (!s_route->c_oil.installed) {
			if (s_route->c_oil.oil.mfcc_origin.s_addr == INADDR_ANY)
				starg_sw_mroute_cnt++;
			else
				sg_sw_mroute_cnt++;
		} else {
			if (s_route->c_oil.oil.mfcc_origin.s_addr == INADDR_ANY)
				starg_hw_mroute_cnt++;
			else
				sg_hw_mroute_cnt++;
		}
	}

	if (!json) {
		vty_out(vty, "%-20s %u/%u\n", "(*, G)", starg_hw_mroute_cnt,
			starg_sw_mroute_cnt + starg_hw_mroute_cnt);
		vty_out(vty, "%-20s %u/%u\n", "(S, G)", sg_hw_mroute_cnt,
			sg_sw_mroute_cnt + sg_hw_mroute_cnt);
		vty_out(vty, "------\n");
		vty_out(vty, "%-20s %u/%u\n", "Total",
			(starg_hw_mroute_cnt + sg_hw_mroute_cnt),
			(starg_sw_mroute_cnt + starg_hw_mroute_cnt
			 + sg_sw_mroute_cnt + sg_hw_mroute_cnt));
	} else {
		/* (*,G) route details */
		json_starg = json_object_new_object();
		json_object_object_add(json, "wildcardGroup", json_starg);

		json_object_int_add(json_starg, "installed",
				    starg_hw_mroute_cnt);
		json_object_int_add(json_starg, "total",
				    starg_sw_mroute_cnt + starg_hw_mroute_cnt);

		/* (S, G) route details */
		json_sg = json_object_new_object();
		json_object_object_add(json, "sourceGroup", json_sg);

		json_object_int_add(json_sg, "installed", sg_hw_mroute_cnt);
		json_object_int_add(json_sg, "total",
				    sg_sw_mroute_cnt + sg_hw_mroute_cnt);

		json_object_int_add(json, "totalNumOfInstalledMroutes",
				    starg_hw_mroute_cnt + sg_hw_mroute_cnt);
		json_object_int_add(json, "totalNumOfMroutes",
				    starg_sw_mroute_cnt + starg_hw_mroute_cnt
				    + sg_sw_mroute_cnt
				    + sg_hw_mroute_cnt);
	}
}

DEFUN (show_ip_mroute_summary,
       show_ip_mroute_summary_cmd,
       "show ip mroute [vrf NAME] summary [json]",
       SHOW_STR
       IP_STR
       MROUTE_STR
       VRF_CMD_HELP_STR
       "Summary of all mroutes\n"
       JSON_STR)
{
	int idx = 2;
	bool uj = use_json(argc, argv);
	struct vrf *vrf = pim_cmd_lookup_vrf(vty, argv, argc, &idx);
	json_object *json = NULL;

	if (uj)
		json = json_object_new_object();

	if (!vrf)
		return CMD_WARNING;

	show_mroute_summary(vrf->info, vty, json);

	if (uj)
		vty_json(vty, json);
	return CMD_SUCCESS;
}

DEFUN (show_ip_mroute_summary_vrf_all,
       show_ip_mroute_summary_vrf_all_cmd,
       "show ip mroute vrf all summary [json]",
       SHOW_STR
       IP_STR
       MROUTE_STR
       VRF_CMD_HELP_STR
       "Summary of all mroutes\n"
       JSON_STR)
{
	struct vrf *vrf;
	bool uj = use_json(argc, argv);
	json_object *json = NULL;
	json_object *json_vrf = NULL;

	if (uj)
		json = json_object_new_object();

	RB_FOREACH (vrf, vrf_name_head, &vrfs_by_name) {
		if (uj)
			json_vrf = json_object_new_object();
		else
			vty_out(vty, "VRF: %s\n", vrf->name);

		show_mroute_summary(vrf->info, vty, json_vrf);

		if (uj)
			json_object_object_add(json, vrf->name, json_vrf);
	}

	if (uj)
		vty_json(vty, json);

	return CMD_SUCCESS;
}

DEFUN (show_ip_rib,
       show_ip_rib_cmd,
       "show ip rib [vrf NAME] A.B.C.D",
       SHOW_STR
       IP_STR
       RIB_STR
       VRF_CMD_HELP_STR
       "Unicast address\n")
{
	int idx = 2;
	struct vrf *vrf = pim_cmd_lookup_vrf(vty, argv, argc, &idx);
	struct in_addr addr;
	const char *addr_str;
	struct pim_nexthop nexthop;
	char nexthop_addr_str[PREFIX_STRLEN];
	int result;

	if (!vrf)
		return CMD_WARNING;

	memset(&nexthop, 0, sizeof(nexthop));
	argv_find(argv, argc, "A.B.C.D", &idx);
	addr_str = argv[idx]->arg;
	result = inet_pton(AF_INET, addr_str, &addr);
	if (result <= 0) {
		vty_out(vty, "Bad unicast address %s: errno=%d: %s\n", addr_str,
			errno, safe_strerror(errno));
		return CMD_WARNING;
	}

	if (!pim_nexthop_lookup(vrf->info, &nexthop, addr, 0)) {
		vty_out(vty,
			"Failure querying RIB nexthop for unicast address %s\n",
			addr_str);
		return CMD_WARNING;
	}

	vty_out(vty,
		"Address         NextHop         Interface Metric Preference\n");

	pim_addr_dump("<nexthop?>", &nexthop.mrib_nexthop_addr,
		      nexthop_addr_str, sizeof(nexthop_addr_str));

	vty_out(vty, "%-15s %-15s %-9s %6d %10d\n", addr_str, nexthop_addr_str,
		nexthop.interface ? nexthop.interface->name : "<ifname?>",
		nexthop.mrib_route_metric, nexthop.mrib_metric_preference);

	return CMD_SUCCESS;
}

static void show_ssmpingd(struct pim_instance *pim, struct vty *vty)
{
	struct listnode *node;
	struct ssmpingd_sock *ss;
	time_t now;

	vty_out(vty,
		"Source          Socket Address          Port Uptime   Requests\n");

	if (!pim->ssmpingd_list)
		return;

	now = pim_time_monotonic_sec();

	for (ALL_LIST_ELEMENTS_RO(pim->ssmpingd_list, node, ss)) {
		char source_str[INET_ADDRSTRLEN];
		char ss_uptime[10];
		struct sockaddr_in bind_addr;
		socklen_t len = sizeof(bind_addr);
		char bind_addr_str[INET_ADDRSTRLEN];

		pim_inet4_dump("<src?>", ss->source_addr, source_str,
			       sizeof(source_str));

		if (pim_socket_getsockname(
			    ss->sock_fd, (struct sockaddr *)&bind_addr, &len)) {
			vty_out(vty,
				"%% Failure reading socket name for ssmpingd source %s on fd=%d\n",
				source_str, ss->sock_fd);
		}

		pim_inet4_dump("<addr?>", bind_addr.sin_addr, bind_addr_str,
			       sizeof(bind_addr_str));
		pim_time_uptime(ss_uptime, sizeof(ss_uptime),
				now - ss->creation);

		vty_out(vty, "%-15s %6d %-15s %5d %8s %8lld\n", source_str,
			ss->sock_fd, bind_addr_str, ntohs(bind_addr.sin_port),
			ss_uptime, (long long)ss->requests);
	}
}

DEFUN (show_ip_ssmpingd,
       show_ip_ssmpingd_cmd,
       "show ip ssmpingd [vrf NAME]",
       SHOW_STR
       IP_STR
       SHOW_SSMPINGD_STR
       VRF_CMD_HELP_STR)
{
	int idx = 2;
	struct vrf *vrf = pim_cmd_lookup_vrf(vty, argv, argc, &idx);

	if (!vrf)
		return CMD_WARNING;

	show_ssmpingd(vrf->info, vty);
	return CMD_SUCCESS;
}

DEFUN (ip_pim_spt_switchover_infinity,
       ip_pim_spt_switchover_infinity_cmd,
       "ip pim spt-switchover infinity-and-beyond",
       IP_STR
       PIM_STR
       "SPT-Switchover\n"
       "Never switch to SPT Tree\n")
{
	const char *vrfname;
	char spt_plist_xpath[XPATH_MAXLEN];
	char spt_action_xpath[XPATH_MAXLEN];

	vrfname = pim_cli_get_vrf_name(vty);
	if (vrfname == NULL)
		return CMD_WARNING_CONFIG_FAILED;

	snprintf(spt_plist_xpath, sizeof(spt_plist_xpath),
		 FRR_PIM_VRF_XPATH, "frr-pim:pimd", "pim", vrfname,
		 "frr-routing:ipv4");
	strlcat(spt_plist_xpath, "/spt-switchover/spt-infinity-prefix-list",
		sizeof(spt_plist_xpath));

	snprintf(spt_action_xpath, sizeof(spt_action_xpath),
		 FRR_PIM_VRF_XPATH, "frr-pim:pimd", "pim", vrfname,
		 "frr-routing:ipv4");
	strlcat(spt_action_xpath, "/spt-switchover/spt-action",
		sizeof(spt_action_xpath));

	if (yang_dnode_exists(vty->candidate_config->dnode, spt_plist_xpath))
		nb_cli_enqueue_change(vty, spt_plist_xpath, NB_OP_DESTROY,
				      NULL);
	nb_cli_enqueue_change(vty, spt_action_xpath, NB_OP_MODIFY,
			      "PIM_SPT_INFINITY");

	return nb_cli_apply_changes(vty, NULL);
}

DEFUN (ip_pim_spt_switchover_infinity_plist,
       ip_pim_spt_switchover_infinity_plist_cmd,
       "ip pim spt-switchover infinity-and-beyond prefix-list WORD",
       IP_STR
       PIM_STR
       "SPT-Switchover\n"
       "Never switch to SPT Tree\n"
       "Prefix-List to control which groups to switch\n"
       "Prefix-List name\n")
{
	const char *vrfname;
	char spt_plist_xpath[XPATH_MAXLEN];
	char spt_action_xpath[XPATH_MAXLEN];

	vrfname = pim_cli_get_vrf_name(vty);
	if (vrfname == NULL)
		return CMD_WARNING_CONFIG_FAILED;

	snprintf(spt_plist_xpath, sizeof(spt_plist_xpath),
		 FRR_PIM_VRF_XPATH, "frr-pim:pimd", "pim", vrfname,
		 "frr-routing:ipv4");
	strlcat(spt_plist_xpath, "/spt-switchover/spt-infinity-prefix-list",
		sizeof(spt_plist_xpath));

	snprintf(spt_action_xpath, sizeof(spt_action_xpath),
		 FRR_PIM_VRF_XPATH, "frr-pim:pimd", "pim", vrfname,
		 "frr-routing:ipv4");
	strlcat(spt_action_xpath, "/spt-switchover/spt-action",
		sizeof(spt_action_xpath));

	nb_cli_enqueue_change(vty, spt_action_xpath, NB_OP_MODIFY,
			      "PIM_SPT_INFINITY");
	nb_cli_enqueue_change(vty, spt_plist_xpath, NB_OP_MODIFY,
			      argv[5]->arg);

	return nb_cli_apply_changes(vty, NULL);
}

DEFUN (no_ip_pim_spt_switchover_infinity,
       no_ip_pim_spt_switchover_infinity_cmd,
       "no ip pim spt-switchover infinity-and-beyond",
       NO_STR
       IP_STR
       PIM_STR
       "SPT_Switchover\n"
       "Never switch to SPT Tree\n")
{
	const char *vrfname;
	char spt_plist_xpath[XPATH_MAXLEN];
	char spt_action_xpath[XPATH_MAXLEN];

	vrfname = pim_cli_get_vrf_name(vty);
	if (vrfname == NULL)
		return CMD_WARNING_CONFIG_FAILED;

	snprintf(spt_plist_xpath, sizeof(spt_plist_xpath),
		 FRR_PIM_VRF_XPATH, "frr-pim:pimd", "pim", vrfname,
		 "frr-routing:ipv4");
	strlcat(spt_plist_xpath, "/spt-switchover/spt-infinity-prefix-list",
		sizeof(spt_plist_xpath));

	snprintf(spt_action_xpath, sizeof(spt_action_xpath),
		 FRR_PIM_VRF_XPATH, "frr-pim:pimd", "pim", vrfname,
		 "frr-routing:ipv4");
	strlcat(spt_action_xpath, "/spt-switchover/spt-action",
		sizeof(spt_action_xpath));

	nb_cli_enqueue_change(vty, spt_plist_xpath, NB_OP_DESTROY, NULL);
	nb_cli_enqueue_change(vty, spt_action_xpath, NB_OP_MODIFY,
			      "PIM_SPT_IMMEDIATE");

	return nb_cli_apply_changes(vty, NULL);
}

DEFUN (no_ip_pim_spt_switchover_infinity_plist,
       no_ip_pim_spt_switchover_infinity_plist_cmd,
       "no ip pim spt-switchover infinity-and-beyond prefix-list WORD",
       NO_STR
       IP_STR
       PIM_STR
       "SPT_Switchover\n"
       "Never switch to SPT Tree\n"
       "Prefix-List to control which groups to switch\n"
       "Prefix-List name\n")
{
	const char *vrfname;
	char spt_plist_xpath[XPATH_MAXLEN];
	char spt_action_xpath[XPATH_MAXLEN];

	vrfname = pim_cli_get_vrf_name(vty);
	if (vrfname == NULL)
		return CMD_WARNING_CONFIG_FAILED;

	snprintf(spt_plist_xpath, sizeof(spt_plist_xpath),
		 FRR_PIM_VRF_XPATH, "frr-pim:pimd", "pim", vrfname,
		 "frr-routing:ipv4");
	strlcat(spt_plist_xpath, "/spt-switchover/spt-infinity-prefix-list",
		sizeof(spt_plist_xpath));

	snprintf(spt_action_xpath, sizeof(spt_action_xpath),
		 FRR_PIM_VRF_XPATH, "frr-pim:pimd", "pim", vrfname,
		 "frr-routing:ipv4");
	strlcat(spt_action_xpath, "/spt-switchover/spt-action",
		sizeof(spt_action_xpath));

	nb_cli_enqueue_change(vty, spt_plist_xpath, NB_OP_DESTROY, NULL);
	nb_cli_enqueue_change(vty, spt_action_xpath, NB_OP_MODIFY,
			      "PIM_SPT_IMMEDIATE");

	return nb_cli_apply_changes(vty, NULL);
}

DEFPY (pim_register_accept_list,
       pim_register_accept_list_cmd,
       "[no] ip pim register-accept-list WORD$word",
       NO_STR
       IP_STR
       PIM_STR
       "Only accept registers from a specific source prefix list\n"
       "Prefix-List name\n")
{
	const char *vrfname;
	char reg_alist_xpath[XPATH_MAXLEN];

	vrfname = pim_cli_get_vrf_name(vty);
	if (vrfname == NULL)
		return CMD_WARNING_CONFIG_FAILED;

	snprintf(reg_alist_xpath, sizeof(reg_alist_xpath),
		 FRR_PIM_VRF_XPATH, "frr-pim:pimd", "pim", vrfname,
		 "frr-routing:ipv4");
	strlcat(reg_alist_xpath, "/register-accept-list",
		sizeof(reg_alist_xpath));

	if (no)
		nb_cli_enqueue_change(vty, reg_alist_xpath,
				      NB_OP_DESTROY, NULL);
	else
		nb_cli_enqueue_change(vty, reg_alist_xpath,
				      NB_OP_MODIFY, word);

	return nb_cli_apply_changes(vty, NULL);
}

DEFUN (ip_pim_joinprune_time,
       ip_pim_joinprune_time_cmd,
       "ip pim join-prune-interval (1-65535)",
       IP_STR
       "pim multicast routing\n"
       "Join Prune Send Interval\n"
       "Seconds\n")
{
	char xpath[XPATH_MAXLEN];

	snprintf(xpath, sizeof(xpath), FRR_PIM_ROUTER_XPATH,
		 "frr-routing:ipv4");
	strlcat(xpath, "/join-prune-interval", sizeof(xpath));

	nb_cli_enqueue_change(vty, xpath, NB_OP_MODIFY, argv[3]->arg);

	return nb_cli_apply_changes(vty, NULL);
}

DEFUN (no_ip_pim_joinprune_time,
       no_ip_pim_joinprune_time_cmd,
       "no ip pim join-prune-interval [(1-65535)]",
       NO_STR
       IP_STR
       "pim multicast routing\n"
       "Join Prune Send Interval\n"
       IGNORED_IN_NO_STR)
{
	char xpath[XPATH_MAXLEN];

	snprintf(xpath, sizeof(xpath), FRR_PIM_ROUTER_XPATH,
		 "frr-routing:ipv4");
	strlcat(xpath, "/join-prune-interval", sizeof(xpath));

	nb_cli_enqueue_change(vty, xpath, NB_OP_DESTROY, NULL);

	return nb_cli_apply_changes(vty, NULL);
}

DEFUN (ip_pim_register_suppress,
       ip_pim_register_suppress_cmd,
       "ip pim register-suppress-time (1-65535)",
       IP_STR
       "pim multicast routing\n"
       "Register Suppress Timer\n"
       "Seconds\n")
{
	char xpath[XPATH_MAXLEN];

	snprintf(xpath, sizeof(xpath), FRR_PIM_ROUTER_XPATH,
		 "frr-routing:ipv4");
	strlcat(xpath, "/register-suppress-time", sizeof(xpath));

	nb_cli_enqueue_change(vty, xpath, NB_OP_MODIFY, argv[3]->arg);

	return nb_cli_apply_changes(vty, NULL);
}

DEFUN (no_ip_pim_register_suppress,
       no_ip_pim_register_suppress_cmd,
       "no ip pim register-suppress-time [(1-65535)]",
       NO_STR
       IP_STR
       "pim multicast routing\n"
       "Register Suppress Timer\n"
       IGNORED_IN_NO_STR)
{
	char xpath[XPATH_MAXLEN];

	snprintf(xpath, sizeof(xpath), FRR_PIM_ROUTER_XPATH,
		 "frr-routing:ipv4");
	strlcat(xpath, "/register-suppress-time", sizeof(xpath));

	nb_cli_enqueue_change(vty, xpath, NB_OP_DESTROY, NULL);

	return nb_cli_apply_changes(vty, NULL);
}

DEFUN (ip_pim_rp_keep_alive,
       ip_pim_rp_keep_alive_cmd,
       "ip pim rp keep-alive-timer (1-65535)",
       IP_STR
       "pim multicast routing\n"
       "Rendevous Point\n"
       "Keep alive Timer\n"
       "Seconds\n")
{
	const char *vrfname;
	char rp_ka_timer_xpath[XPATH_MAXLEN];

	vrfname = pim_cli_get_vrf_name(vty);
	if (vrfname == NULL)
		return CMD_WARNING_CONFIG_FAILED;

	snprintf(rp_ka_timer_xpath, sizeof(rp_ka_timer_xpath),
		 FRR_PIM_VRF_XPATH, "frr-pim:pimd", "pim", vrfname,
		 "frr-routing:ipv4");
	strlcat(rp_ka_timer_xpath, "/rp-keep-alive-timer",
		sizeof(rp_ka_timer_xpath));

	nb_cli_enqueue_change(vty, rp_ka_timer_xpath, NB_OP_MODIFY,
			      argv[4]->arg);

	return nb_cli_apply_changes(vty, NULL);
}

DEFUN (no_ip_pim_rp_keep_alive,
       no_ip_pim_rp_keep_alive_cmd,
       "no ip pim rp keep-alive-timer [(1-65535)]",
       NO_STR
       IP_STR
       "pim multicast routing\n"
       "Rendevous Point\n"
       "Keep alive Timer\n"
       IGNORED_IN_NO_STR)
{
	const char *vrfname;
	char rp_ka_timer[6];
	char rp_ka_timer_xpath[XPATH_MAXLEN];
	uint v;
	char rs_timer_xpath[XPATH_MAXLEN];

	snprintf(rs_timer_xpath, sizeof(rs_timer_xpath),
		 FRR_PIM_ROUTER_XPATH, "frr-routing:ipv4");
	strlcat(rs_timer_xpath, "/register-suppress-time",
		sizeof(rs_timer_xpath));

	/* RFC4601 */
	v = yang_dnode_get_uint16(vty->candidate_config->dnode,
				  rs_timer_xpath);
	v = 3 * v + PIM_REGISTER_PROBE_TIME_DEFAULT;
	if (v > UINT16_MAX)
		v = UINT16_MAX;
	snprintf(rp_ka_timer, sizeof(rp_ka_timer), "%u", v);

	vrfname = pim_cli_get_vrf_name(vty);
	if (vrfname == NULL)
		return CMD_WARNING_CONFIG_FAILED;

	snprintf(rp_ka_timer_xpath, sizeof(rp_ka_timer_xpath),
		 FRR_PIM_VRF_XPATH, "frr-pim:pimd", "pim", vrfname,
		 "frr-routing:ipv4");
	strlcat(rp_ka_timer_xpath, "/rp-keep-alive-timer",
		sizeof(rp_ka_timer_xpath));

	nb_cli_enqueue_change(vty, rp_ka_timer_xpath, NB_OP_MODIFY,
			      rp_ka_timer);

	return nb_cli_apply_changes(vty, NULL);
}

DEFUN (ip_pim_keep_alive,
       ip_pim_keep_alive_cmd,
       "ip pim keep-alive-timer (1-65535)",
       IP_STR
       "pim multicast routing\n"
       "Keep alive Timer\n"
       "Seconds\n")
{
	const char *vrfname;
	char ka_timer_xpath[XPATH_MAXLEN];

	vrfname = pim_cli_get_vrf_name(vty);
	if (vrfname == NULL)
		return CMD_WARNING_CONFIG_FAILED;

	snprintf(ka_timer_xpath, sizeof(ka_timer_xpath), FRR_PIM_VRF_XPATH,
		 "frr-pim:pimd", "pim", vrfname, "frr-routing:ipv4");
	strlcat(ka_timer_xpath, "/keep-alive-timer", sizeof(ka_timer_xpath));

	nb_cli_enqueue_change(vty, ka_timer_xpath, NB_OP_MODIFY,
			      argv[3]->arg);

	return nb_cli_apply_changes(vty, NULL);
}

DEFUN (no_ip_pim_keep_alive,
       no_ip_pim_keep_alive_cmd,
       "no ip pim keep-alive-timer [(1-65535)]",
       NO_STR
       IP_STR
       "pim multicast routing\n"
       "Keep alive Timer\n"
       IGNORED_IN_NO_STR)
{
	const char *vrfname;
	char ka_timer_xpath[XPATH_MAXLEN];

	vrfname = pim_cli_get_vrf_name(vty);
	if (vrfname == NULL)
		return CMD_WARNING_CONFIG_FAILED;

	snprintf(ka_timer_xpath, sizeof(ka_timer_xpath), FRR_PIM_VRF_XPATH,
		 "frr-pim:pimd", "pim", vrfname, "frr-routing:ipv4");
	strlcat(ka_timer_xpath, "/keep-alive-timer", sizeof(ka_timer_xpath));

	nb_cli_enqueue_change(vty, ka_timer_xpath, NB_OP_DESTROY, NULL);

	return nb_cli_apply_changes(vty, NULL);
}

DEFUN (ip_pim_packets,
       ip_pim_packets_cmd,
       "ip pim packets (1-255)",
       IP_STR
       "pim multicast routing\n"
       "packets to process at one time per fd\n"
       "Number of packets\n")
{
	char xpath[XPATH_MAXLEN];

	snprintf(xpath, sizeof(xpath), FRR_PIM_ROUTER_XPATH,
		 "frr-routing:ipv4");
	strlcat(xpath, "/packets", sizeof(xpath));

	nb_cli_enqueue_change(vty, xpath, NB_OP_MODIFY, argv[3]->arg);

	return nb_cli_apply_changes(vty, NULL);
}

DEFUN (no_ip_pim_packets,
       no_ip_pim_packets_cmd,
       "no ip pim packets [(1-255)]",
       NO_STR
       IP_STR
       "pim multicast routing\n"
       "packets to process at one time per fd\n"
       IGNORED_IN_NO_STR)
{
	char xpath[XPATH_MAXLEN];

	snprintf(xpath, sizeof(xpath), FRR_PIM_ROUTER_XPATH,
		 "frr-routing:ipv4");
	strlcat(xpath, "/packets", sizeof(xpath));

	nb_cli_enqueue_change(vty, xpath, NB_OP_DESTROY, NULL);

	return nb_cli_apply_changes(vty, NULL);
}

DEFPY (igmp_group_watermark,
       igmp_group_watermark_cmd,
       "ip igmp watermark-warn (1-65535)$limit",
       IP_STR
       IGMP_STR
       "Configure group limit for watermark warning\n"
       "Group count to generate watermark warning\n")
{
	PIM_DECLVAR_CONTEXT(vrf, pim);
	pim->igmp_watermark_limit = limit;

	return CMD_SUCCESS;
}

DEFPY (no_igmp_group_watermark,
       no_igmp_group_watermark_cmd,
       "no ip igmp watermark-warn [(1-65535)$limit]",
       NO_STR
       IP_STR
       IGMP_STR
       "Unconfigure group limit for watermark warning\n"
       IGNORED_IN_NO_STR)
{
	PIM_DECLVAR_CONTEXT(vrf, pim);
	pim->igmp_watermark_limit = 0;

	return CMD_SUCCESS;
}

DEFUN (ip_pim_v6_secondary,
       ip_pim_v6_secondary_cmd,
       "ip pim send-v6-secondary",
       IP_STR
       "pim multicast routing\n"
       "Send v6 secondary addresses\n")
{
	const char *vrfname;
	char send_v6_secondary_xpath[XPATH_MAXLEN];

	vrfname = pim_cli_get_vrf_name(vty);
	if (vrfname == NULL)
		return CMD_WARNING_CONFIG_FAILED;

	snprintf(send_v6_secondary_xpath, sizeof(send_v6_secondary_xpath),
		 FRR_PIM_VRF_XPATH,
		 "frr-pim:pimd", "pim", vrfname, "frr-routing:ipv4");
	strlcat(send_v6_secondary_xpath, "/send-v6-secondary",
		sizeof(send_v6_secondary_xpath));

	nb_cli_enqueue_change(vty, send_v6_secondary_xpath, NB_OP_MODIFY,
			      "true");

	return nb_cli_apply_changes(vty, NULL);
}

DEFUN (no_ip_pim_v6_secondary,
       no_ip_pim_v6_secondary_cmd,
       "no ip pim send-v6-secondary",
       NO_STR
       IP_STR
       "pim multicast routing\n"
       "Send v6 secondary addresses\n")
{
	const char *vrfname;
	char send_v6_secondary_xpath[XPATH_MAXLEN];

	vrfname = pim_cli_get_vrf_name(vty);
	if (vrfname == NULL)
		return CMD_WARNING_CONFIG_FAILED;

	snprintf(send_v6_secondary_xpath, sizeof(send_v6_secondary_xpath),
		 FRR_PIM_VRF_XPATH,
		 "frr-pim:pimd", "pim", vrfname, "frr-routing:ipv4");
	strlcat(send_v6_secondary_xpath, "/send-v6-secondary",
		sizeof(send_v6_secondary_xpath));

	nb_cli_enqueue_change(vty, send_v6_secondary_xpath, NB_OP_MODIFY,
			      "false");

	return nb_cli_apply_changes(vty, NULL);
}

DEFUN (ip_pim_rp,
       ip_pim_rp_cmd,
       "ip pim rp A.B.C.D [A.B.C.D/M]",
       IP_STR
       "pim multicast routing\n"
       "Rendevous Point\n"
       "ip address of RP\n"
       "Group Address range to cover\n")
{
	const char *vrfname;
	int idx_rp = 3, idx_group = 4;
	char rp_group_xpath[XPATH_MAXLEN];
	int result = 0;
	struct prefix group;
	struct in_addr rp_addr;
	const char *group_str =
		(argc == 5) ? argv[idx_group]->arg : "224.0.0.0/4";

	result = str2prefix(group_str, &group);
	if (result) {
		struct prefix temp;

		prefix_copy(&temp, &group);
		apply_mask(&temp);
		if (!prefix_same(&group, &temp)) {
			vty_out(vty, "%% Inconsistent address and mask: %s\n",
				group_str);
			return CMD_WARNING_CONFIG_FAILED;
		}
	}

	if (!result) {
		vty_out(vty, "%% Bad group address specified: %s\n",
			group_str);
		return CMD_WARNING_CONFIG_FAILED;
	}

	result = inet_pton(AF_INET, argv[idx_rp]->arg, &rp_addr);
	if (result <= 0) {
		vty_out(vty, "%% Bad RP address specified: %s\n",
			argv[idx_rp]->arg);
		return CMD_WARNING_CONFIG_FAILED;
	}

	vrfname = pim_cli_get_vrf_name(vty);
	if (vrfname == NULL)
		return CMD_WARNING_CONFIG_FAILED;

	snprintf(rp_group_xpath, sizeof(rp_group_xpath),
		 FRR_PIM_STATIC_RP_XPATH,
		 "frr-pim:pimd", "pim", vrfname, "frr-routing:ipv4",
		 argv[idx_rp]->arg);
	strlcat(rp_group_xpath, "/group-list", sizeof(rp_group_xpath));

	nb_cli_enqueue_change(vty, rp_group_xpath, NB_OP_CREATE, group_str);

	return nb_cli_apply_changes(vty, NULL);
}

DEFUN (ip_pim_rp_prefix_list,
       ip_pim_rp_prefix_list_cmd,
       "ip pim rp A.B.C.D prefix-list WORD",
       IP_STR
       "pim multicast routing\n"
       "Rendevous Point\n"
       "ip address of RP\n"
       "group prefix-list filter\n"
       "Name of a prefix-list\n")
{
	int idx_rp = 3, idx_plist = 5;
	const char *vrfname;
	char rp_plist_xpath[XPATH_MAXLEN];

	vrfname = pim_cli_get_vrf_name(vty);
	if (vrfname == NULL)
		return CMD_WARNING_CONFIG_FAILED;

	snprintf(rp_plist_xpath, sizeof(rp_plist_xpath),
		 FRR_PIM_STATIC_RP_XPATH,
		 "frr-pim:pimd", "pim", vrfname, "frr-routing:ipv4",
		 argv[idx_rp]->arg);
	strlcat(rp_plist_xpath, "/prefix-list", sizeof(rp_plist_xpath));

	nb_cli_enqueue_change(vty, rp_plist_xpath, NB_OP_MODIFY,
			      argv[idx_plist]->arg);

	return nb_cli_apply_changes(vty, NULL);
}

DEFUN (no_ip_pim_rp,
       no_ip_pim_rp_cmd,
       "no ip pim rp A.B.C.D [A.B.C.D/M]",
       NO_STR
       IP_STR
       "pim multicast routing\n"
       "Rendevous Point\n"
       "ip address of RP\n"
       "Group Address range to cover\n")
{
	int idx_rp = 4, idx_group = 5;
	const char *group_str =
		(argc == 6) ? argv[idx_group]->arg : "224.0.0.0/4";
	char group_list_xpath[XPATH_MAXLEN];
	char group_xpath[XPATH_MAXLEN];
	char rp_xpath[XPATH_MAXLEN];
	int printed;
	const char *vrfname;
	const struct lyd_node *group_dnode;

	vrfname = pim_cli_get_vrf_name(vty);
	if (vrfname == NULL)
		return CMD_WARNING_CONFIG_FAILED;

	snprintf(rp_xpath, sizeof(rp_xpath), FRR_PIM_STATIC_RP_XPATH,
		 "frr-pim:pimd", "pim", vrfname, "frr-routing:ipv4",
		 argv[idx_rp]->arg);


	printed = snprintf(group_list_xpath, sizeof(group_list_xpath),
			   "%s/group-list", rp_xpath);

	if (printed >= (int)(sizeof(group_list_xpath))) {
		vty_out(vty, "Xpath too long (%d > %u)", printed + 1,
			XPATH_MAXLEN);
		return CMD_WARNING_CONFIG_FAILED;
	}

	printed = snprintf(group_xpath, sizeof(group_xpath), "%s[.='%s']",
			   group_list_xpath, group_str);

	if (printed >= (int)(sizeof(group_xpath))) {
		vty_out(vty, "Xpath too long (%d > %u)", printed + 1,
			XPATH_MAXLEN);
		return CMD_WARNING_CONFIG_FAILED;
	}

	if (!yang_dnode_exists(vty->candidate_config->dnode, group_xpath)) {
		vty_out(vty, "%% Unable to find specified RP\n");
		return NB_OK;
	}

	group_dnode = yang_dnode_get(vty->candidate_config->dnode, group_xpath);

	if (yang_is_last_list_dnode(group_dnode))
		nb_cli_enqueue_change(vty, rp_xpath, NB_OP_DESTROY, NULL);
	else
		nb_cli_enqueue_change(vty, group_list_xpath, NB_OP_DESTROY,
				      group_str);

	return nb_cli_apply_changes(vty, NULL);
}

DEFUN (no_ip_pim_rp_prefix_list,
       no_ip_pim_rp_prefix_list_cmd,
       "no ip pim rp A.B.C.D prefix-list WORD",
       NO_STR
       IP_STR
       "pim multicast routing\n"
       "Rendevous Point\n"
       "ip address of RP\n"
       "group prefix-list filter\n"
       "Name of a prefix-list\n")
{
	int idx_rp = 4;
	int idx_plist = 6;
	char rp_xpath[XPATH_MAXLEN];
	char plist_xpath[XPATH_MAXLEN];
	const char *vrfname;
	const struct lyd_node *plist_dnode;
	const char *plist;

	vrfname = pim_cli_get_vrf_name(vty);
	if (vrfname == NULL)
		return CMD_WARNING_CONFIG_FAILED;

	snprintf(rp_xpath, sizeof(rp_xpath), FRR_PIM_STATIC_RP_XPATH,
		 "frr-pim:pimd", "pim", vrfname, "frr-routing:ipv4",
		 argv[idx_rp]->arg);

	snprintf(plist_xpath, sizeof(plist_xpath), FRR_PIM_STATIC_RP_XPATH,
		 "frr-pim:pimd", "pim", vrfname, "frr-routing:ipv4",
		 argv[idx_rp]->arg);
	strlcat(plist_xpath, "/prefix-list", sizeof(plist_xpath));

	plist_dnode = yang_dnode_get(vty->candidate_config->dnode, plist_xpath);
	if (!plist_dnode) {
		vty_out(vty, "%% Unable to find specified RP\n");
		return NB_OK;
	}

	plist = yang_dnode_get_string(plist_dnode, plist_xpath);
	if (strcmp(argv[idx_plist]->arg, plist)) {
		vty_out(vty, "%% Unable to find specified RP\n");
		return NB_OK;
	}

	nb_cli_enqueue_change(vty, rp_xpath, NB_OP_DESTROY, NULL);

	return nb_cli_apply_changes(vty, NULL);
}

DEFUN (ip_pim_ssm_prefix_list,
       ip_pim_ssm_prefix_list_cmd,
       "ip pim ssm prefix-list WORD",
       IP_STR
       "pim multicast routing\n"
       "Source Specific Multicast\n"
       "group range prefix-list filter\n"
       "Name of a prefix-list\n")
{
	const char *vrfname;
	char ssm_plist_xpath[XPATH_MAXLEN];

	vrfname = pim_cli_get_vrf_name(vty);
	if (vrfname == NULL)
		return CMD_WARNING_CONFIG_FAILED;

	snprintf(ssm_plist_xpath, sizeof(ssm_plist_xpath), FRR_PIM_VRF_XPATH,
		 "frr-pim:pimd", "pim", vrfname, "frr-routing:ipv4");
	strlcat(ssm_plist_xpath, "/ssm-prefix-list", sizeof(ssm_plist_xpath));

	nb_cli_enqueue_change(vty, ssm_plist_xpath, NB_OP_MODIFY, argv[4]->arg);

	return nb_cli_apply_changes(vty, NULL);
}

DEFUN (no_ip_pim_ssm_prefix_list,
       no_ip_pim_ssm_prefix_list_cmd,
       "no ip pim ssm prefix-list",
       NO_STR
       IP_STR
       "pim multicast routing\n"
       "Source Specific Multicast\n"
       "group range prefix-list filter\n")
{
	const char *vrfname;
	char ssm_plist_xpath[XPATH_MAXLEN];

	vrfname = pim_cli_get_vrf_name(vty);
	if (vrfname == NULL)
		return CMD_WARNING_CONFIG_FAILED;

	snprintf(ssm_plist_xpath, sizeof(ssm_plist_xpath),
		 FRR_PIM_VRF_XPATH,
		 "frr-pim:pimd", "pim", vrfname, "frr-routing:ipv4");
	strlcat(ssm_plist_xpath, "/ssm-prefix-list", sizeof(ssm_plist_xpath));

	nb_cli_enqueue_change(vty, ssm_plist_xpath, NB_OP_DESTROY, NULL);

	return nb_cli_apply_changes(vty, NULL);
}

DEFUN (no_ip_pim_ssm_prefix_list_name,
       no_ip_pim_ssm_prefix_list_name_cmd,
       "no ip pim ssm prefix-list WORD",
       NO_STR
       IP_STR
       "pim multicast routing\n"
       "Source Specific Multicast\n"
       "group range prefix-list filter\n"
       "Name of a prefix-list\n")
{
	const char *vrfname;
	const struct lyd_node *ssm_plist_dnode;
	char ssm_plist_xpath[XPATH_MAXLEN];
	const char *ssm_plist_name;

	vrfname = pim_cli_get_vrf_name(vty);
	if (vrfname == NULL)
		return CMD_WARNING_CONFIG_FAILED;

	snprintf(ssm_plist_xpath, sizeof(ssm_plist_xpath),
		 FRR_PIM_VRF_XPATH,
		 "frr-pim:pimd", "pim", vrfname, "frr-routing:ipv4");
	strlcat(ssm_plist_xpath, "/ssm-prefix-list", sizeof(ssm_plist_xpath));
	ssm_plist_dnode = yang_dnode_get(vty->candidate_config->dnode,
					 ssm_plist_xpath);

	if (!ssm_plist_dnode) {
		vty_out(vty,
			"%% pim ssm prefix-list %s doesn't exist\n",
			argv[5]->arg);
		return CMD_WARNING_CONFIG_FAILED;
	}

	ssm_plist_name = yang_dnode_get_string(ssm_plist_dnode, ".");

	if (ssm_plist_name && !strcmp(ssm_plist_name, argv[5]->arg)) {
		nb_cli_enqueue_change(vty, ssm_plist_xpath, NB_OP_DESTROY,
				      NULL);

		return nb_cli_apply_changes(vty, NULL);
	}

	vty_out(vty, "%% pim ssm prefix-list %s doesn't exist\n", argv[5]->arg);

	return CMD_WARNING_CONFIG_FAILED;
}

static void ip_pim_ssm_show_group_range(struct pim_instance *pim,
					struct vty *vty, bool uj)
{
	struct pim_ssm *ssm = pim->ssm_info;
	const char *range_str =
		ssm->plist_name ? ssm->plist_name : PIM_SSM_STANDARD_RANGE;

	if (uj) {
		json_object *json;
		json = json_object_new_object();
		json_object_string_add(json, "ssmGroups", range_str);
		vty_json(vty, json);
	} else
		vty_out(vty, "SSM group range : %s\n", range_str);
}

DEFUN (show_ip_pim_ssm_range,
       show_ip_pim_ssm_range_cmd,
       "show ip pim [vrf NAME] group-type [json]",
       SHOW_STR
       IP_STR
       PIM_STR
       VRF_CMD_HELP_STR
       "PIM group type\n"
       JSON_STR)
{
	int idx = 2;
	struct vrf *vrf = pim_cmd_lookup_vrf(vty, argv, argc, &idx);
	bool uj = use_json(argc, argv);

	if (!vrf)
		return CMD_WARNING;

	ip_pim_ssm_show_group_range(vrf->info, vty, uj);

	return CMD_SUCCESS;
}

static void ip_pim_ssm_show_group_type(struct pim_instance *pim,
				       struct vty *vty, bool uj,
				       const char *group)
{
	struct in_addr group_addr;
	const char *type_str;
	int result;

	result = inet_pton(AF_INET, group, &group_addr);
	if (result <= 0)
		type_str = "invalid";
	else {
		if (pim_is_group_224_4(group_addr))
			type_str =
				pim_is_grp_ssm(pim, group_addr) ? "SSM" : "ASM";
		else
			type_str = "not-multicast";
	}

	if (uj) {
		json_object *json;
		json = json_object_new_object();
		json_object_string_add(json, "groupType", type_str);
		vty_json(vty, json);
	} else
		vty_out(vty, "Group type : %s\n", type_str);
}

DEFUN (show_ip_pim_group_type,
       show_ip_pim_group_type_cmd,
       "show ip pim [vrf NAME] group-type A.B.C.D [json]",
       SHOW_STR
       IP_STR
       PIM_STR
       VRF_CMD_HELP_STR
       "multicast group type\n"
       "group address\n"
       JSON_STR)
{
	int idx = 2;
	struct vrf *vrf = pim_cmd_lookup_vrf(vty, argv, argc, &idx);
	bool uj = use_json(argc, argv);

	if (!vrf)
		return CMD_WARNING;

	argv_find(argv, argc, "A.B.C.D", &idx);
	ip_pim_ssm_show_group_type(vrf->info, vty, uj, argv[idx]->arg);

	return CMD_SUCCESS;
}

DEFUN (show_ip_pim_bsr,
       show_ip_pim_bsr_cmd,
       "show ip pim bsr [json]",
       SHOW_STR
       IP_STR
       PIM_STR
       "boot-strap router information\n"
       JSON_STR)
{
	int idx = 2;
	struct vrf *vrf = pim_cmd_lookup_vrf(vty, argv, argc, &idx);
	bool uj = use_json(argc, argv);

	if (!vrf)
		return CMD_WARNING;

	pim_show_bsr(vrf->info, vty, uj);

	return CMD_SUCCESS;
}

DEFUN (ip_ssmpingd,
       ip_ssmpingd_cmd,
       "ip ssmpingd [A.B.C.D]",
       IP_STR
       CONF_SSMPINGD_STR
       "Source address\n")
{
	int idx_ipv4 = 2;
	const char *source_str = (argc == 3) ? argv[idx_ipv4]->arg : "0.0.0.0";
	const char *vrfname;
	char ssmpingd_ip_xpath[XPATH_MAXLEN];

	vrfname = pim_cli_get_vrf_name(vty);
	if (vrfname == NULL)
		return CMD_WARNING_CONFIG_FAILED;

	snprintf(ssmpingd_ip_xpath, sizeof(ssmpingd_ip_xpath),
		 FRR_PIM_VRF_XPATH,
		 "frr-pim:pimd", "pim", vrfname, "frr-routing:ipv4");
	strlcat(ssmpingd_ip_xpath, "/ssm-pingd-source-ip",
		sizeof(ssmpingd_ip_xpath));

	nb_cli_enqueue_change(vty, ssmpingd_ip_xpath, NB_OP_CREATE,
			      source_str);

	return nb_cli_apply_changes(vty, NULL);
}

DEFUN (no_ip_ssmpingd,
       no_ip_ssmpingd_cmd,
       "no ip ssmpingd [A.B.C.D]",
       NO_STR
       IP_STR
       CONF_SSMPINGD_STR
       "Source address\n")
{
	const char *vrfname;
	int idx_ipv4 = 3;
	const char *source_str = (argc == 4) ? argv[idx_ipv4]->arg : "0.0.0.0";
	char ssmpingd_ip_xpath[XPATH_MAXLEN];

	vrfname = pim_cli_get_vrf_name(vty);
	if (vrfname == NULL)
		return CMD_WARNING_CONFIG_FAILED;

	snprintf(ssmpingd_ip_xpath, sizeof(ssmpingd_ip_xpath),
		 FRR_PIM_VRF_XPATH,
		 "frr-pim:pimd", "pim", vrfname, "frr-routing:ipv4");
	strlcat(ssmpingd_ip_xpath, "/ssm-pingd-source-ip",
		sizeof(ssmpingd_ip_xpath));

	nb_cli_enqueue_change(vty, ssmpingd_ip_xpath, NB_OP_DESTROY,
			      source_str);

	return nb_cli_apply_changes(vty, NULL);
}

DEFUN (ip_pim_ecmp,
       ip_pim_ecmp_cmd,
       "ip pim ecmp",
       IP_STR
       "pim multicast routing\n"
       "Enable PIM ECMP \n")
{
	const char *vrfname;
	char ecmp_xpath[XPATH_MAXLEN];

	vrfname = pim_cli_get_vrf_name(vty);
	if (vrfname == NULL)
		return CMD_WARNING_CONFIG_FAILED;

	snprintf(ecmp_xpath, sizeof(ecmp_xpath), FRR_PIM_VRF_XPATH,
		 "frr-pim:pimd", "pim", vrfname, "frr-routing:ipv4");
	strlcat(ecmp_xpath, "/ecmp", sizeof(ecmp_xpath));

	nb_cli_enqueue_change(vty, ecmp_xpath, NB_OP_MODIFY, "true");
	return nb_cli_apply_changes(vty, NULL);
}

DEFUN (no_ip_pim_ecmp,
       no_ip_pim_ecmp_cmd,
       "no ip pim ecmp",
       NO_STR
       IP_STR
       "pim multicast routing\n"
       "Disable PIM ECMP \n")
{
	const char *vrfname;
	char ecmp_xpath[XPATH_MAXLEN];

	vrfname = pim_cli_get_vrf_name(vty);
	if (vrfname == NULL)
		return CMD_WARNING_CONFIG_FAILED;

	snprintf(ecmp_xpath, sizeof(ecmp_xpath), FRR_PIM_VRF_XPATH,
		 "frr-pim:pimd", "pim", vrfname, "frr-routing:ipv4");
	strlcat(ecmp_xpath, "/ecmp", sizeof(ecmp_xpath));

	nb_cli_enqueue_change(vty, ecmp_xpath, NB_OP_MODIFY, "false");

	return nb_cli_apply_changes(vty, NULL);
}

DEFUN (ip_pim_ecmp_rebalance,
       ip_pim_ecmp_rebalance_cmd,
       "ip pim ecmp rebalance",
       IP_STR
       "pim multicast routing\n"
       "Enable PIM ECMP \n"
       "Enable PIM ECMP Rebalance\n")
{
	const char *vrfname;
	char ecmp_xpath[XPATH_MAXLEN];
	char ecmp_rebalance_xpath[XPATH_MAXLEN];

	vrfname = pim_cli_get_vrf_name(vty);
	if (vrfname == NULL)
		return CMD_WARNING_CONFIG_FAILED;

	snprintf(ecmp_xpath, sizeof(ecmp_xpath), FRR_PIM_VRF_XPATH,
		 "frr-pim:pimd", "pim", vrfname, "frr-routing:ipv4");
	strlcat(ecmp_xpath, "/ecmp", sizeof(ecmp_xpath));
	snprintf(ecmp_rebalance_xpath, sizeof(ecmp_rebalance_xpath),
		 FRR_PIM_VRF_XPATH,
		 "frr-pim:pimd", "pim", vrfname, "frr-routing:ipv4");
	strlcat(ecmp_rebalance_xpath, "/ecmp-rebalance",
		sizeof(ecmp_rebalance_xpath));

	nb_cli_enqueue_change(vty, ecmp_xpath, NB_OP_MODIFY, "true");
	nb_cli_enqueue_change(vty, ecmp_rebalance_xpath, NB_OP_MODIFY, "true");

	return nb_cli_apply_changes(vty, NULL);
}

DEFUN (no_ip_pim_ecmp_rebalance,
       no_ip_pim_ecmp_rebalance_cmd,
       "no ip pim ecmp rebalance",
       NO_STR
       IP_STR
       "pim multicast routing\n"
       "Disable PIM ECMP \n"
       "Disable PIM ECMP Rebalance\n")
{
	const char *vrfname;
	char ecmp_rebalance_xpath[XPATH_MAXLEN];

	vrfname = pim_cli_get_vrf_name(vty);
	if (vrfname == NULL)
		return CMD_WARNING_CONFIG_FAILED;

	snprintf(ecmp_rebalance_xpath, sizeof(ecmp_rebalance_xpath),
		 FRR_PIM_VRF_XPATH,
		 "frr-pim:pimd", "pim", vrfname, "frr-routing:ipv4");
	strlcat(ecmp_rebalance_xpath, "/ecmp-rebalance",
		sizeof(ecmp_rebalance_xpath));

	nb_cli_enqueue_change(vty, ecmp_rebalance_xpath, NB_OP_MODIFY, "false");

	return nb_cli_apply_changes(vty, NULL);
}

DEFUN (interface_ip_igmp,
       interface_ip_igmp_cmd,
       "ip igmp",
       IP_STR
       IFACE_IGMP_STR)
{
	nb_cli_enqueue_change(vty, "./enable", NB_OP_MODIFY, "true");

	return nb_cli_apply_changes(vty, FRR_GMP_INTERFACE_XPATH,
				    "frr-routing:ipv4");
}

DEFUN (interface_no_ip_igmp,
       interface_no_ip_igmp_cmd,
       "no ip igmp",
       NO_STR
       IP_STR
       IFACE_IGMP_STR)
{
	const struct lyd_node *pim_enable_dnode;
	char pim_if_xpath[XPATH_MAXLEN];

	int printed =
		snprintf(pim_if_xpath, sizeof(pim_if_xpath),
			 "%s/frr-pim:pim/address-family[address-family='%s']",
			 VTY_CURR_XPATH, "frr-routing:ipv4");

	if (printed >= (int)(sizeof(pim_if_xpath))) {
		vty_out(vty, "Xpath too long (%d > %u)", printed + 1,
			XPATH_MAXLEN);
		return CMD_WARNING_CONFIG_FAILED;
	}

	pim_enable_dnode = yang_dnode_getf(vty->candidate_config->dnode,
					   FRR_PIM_ENABLE_XPATH, VTY_CURR_XPATH,
					   "frr-routing:ipv4");
	if (!pim_enable_dnode) {
		nb_cli_enqueue_change(vty, pim_if_xpath, NB_OP_DESTROY, NULL);
		nb_cli_enqueue_change(vty, ".", NB_OP_DESTROY, NULL);
	} else {
		if (!yang_dnode_get_bool(pim_enable_dnode, ".")) {
			nb_cli_enqueue_change(vty, pim_if_xpath, NB_OP_DESTROY,
					      NULL);
			nb_cli_enqueue_change(vty, ".", NB_OP_DESTROY, NULL);
		} else
			nb_cli_enqueue_change(vty, "./enable",
					      NB_OP_MODIFY, "false");
	}

	return nb_cli_apply_changes(vty, FRR_GMP_INTERFACE_XPATH,
				    "frr-routing:ipv4");
}

DEFUN (interface_ip_igmp_join,
       interface_ip_igmp_join_cmd,
       "ip igmp join A.B.C.D [A.B.C.D]",
       IP_STR
       IFACE_IGMP_STR
       "IGMP join multicast group\n"
       "Multicast group address\n"
       "Source address\n")
{
	int idx_group = 3;
	int idx_source = 4;
	const char *source_str;
	char xpath[XPATH_MAXLEN];

	if (argc == 5) {
		source_str = argv[idx_source]->arg;

		if (strcmp(source_str, "0.0.0.0") == 0) {
			vty_out(vty, "Bad source address %s\n",
				argv[idx_source]->arg);
			return CMD_WARNING_CONFIG_FAILED;
		}
	} else
		source_str = "0.0.0.0";

	snprintf(xpath, sizeof(xpath), FRR_GMP_JOIN_XPATH,
		 "frr-routing:ipv4", argv[idx_group]->arg, source_str);

	nb_cli_enqueue_change(vty, xpath, NB_OP_CREATE, NULL);

	return nb_cli_apply_changes(vty, NULL);
}

DEFUN (interface_no_ip_igmp_join,
       interface_no_ip_igmp_join_cmd,
       "no ip igmp join A.B.C.D [A.B.C.D]",
       NO_STR
       IP_STR
       IFACE_IGMP_STR
       "IGMP join multicast group\n"
       "Multicast group address\n"
       "Source address\n")
{
	int idx_group = 4;
	int idx_source = 5;
	const char *source_str;
	char xpath[XPATH_MAXLEN];

	if (argc == 6) {
		source_str = argv[idx_source]->arg;

		if (strcmp(source_str, "0.0.0.0") == 0) {
			vty_out(vty, "Bad source address %s\n",
				argv[idx_source]->arg);
			return CMD_WARNING_CONFIG_FAILED;
		}
	} else
		source_str = "0.0.0.0";

	snprintf(xpath, sizeof(xpath), FRR_GMP_JOIN_XPATH,
		 "frr-routing:ipv4", argv[idx_group]->arg, source_str);

	nb_cli_enqueue_change(vty, xpath, NB_OP_DESTROY, NULL);

	return nb_cli_apply_changes(vty, NULL);
}

DEFUN (interface_ip_igmp_query_interval,
       interface_ip_igmp_query_interval_cmd,
       "ip igmp query-interval (1-65535)",
       IP_STR
       IFACE_IGMP_STR
       IFACE_IGMP_QUERY_INTERVAL_STR
       "Query interval in seconds\n")
{
	const struct lyd_node *pim_enable_dnode;

	pim_enable_dnode =
		yang_dnode_getf(vty->candidate_config->dnode,
				FRR_PIM_ENABLE_XPATH, VTY_CURR_XPATH,
				"frr-routing:ipv4");
	if (!pim_enable_dnode) {
		nb_cli_enqueue_change(vty, "./enable", NB_OP_MODIFY,
				      "true");
	} else {
		if (!yang_dnode_get_bool(pim_enable_dnode, "."))
			nb_cli_enqueue_change(vty, "./enable",
					      NB_OP_MODIFY, "true");
	}

	nb_cli_enqueue_change(vty, "./query-interval", NB_OP_MODIFY,
			      argv[3]->arg);

	return nb_cli_apply_changes(vty, FRR_GMP_INTERFACE_XPATH,
				    "frr-routing:ipv4");
}

DEFUN (interface_no_ip_igmp_query_interval,
       interface_no_ip_igmp_query_interval_cmd,
       "no ip igmp query-interval [(1-65535)]",
       NO_STR
       IP_STR
       IFACE_IGMP_STR
       IFACE_IGMP_QUERY_INTERVAL_STR
       IGNORED_IN_NO_STR)
{
	nb_cli_enqueue_change(vty, "./query-interval", NB_OP_DESTROY, NULL);

	return nb_cli_apply_changes(vty, FRR_GMP_INTERFACE_XPATH,
				    "frr-routing:ipv4");
}

DEFUN (interface_ip_igmp_version,
       interface_ip_igmp_version_cmd,
       "ip igmp version (2-3)",
       IP_STR
       IFACE_IGMP_STR
       "IGMP version\n"
       "IGMP version number\n")
{
	nb_cli_enqueue_change(vty, "./enable", NB_OP_MODIFY,
			      "true");
	nb_cli_enqueue_change(vty, "./igmp-version", NB_OP_MODIFY,
			      argv[3]->arg);

	return nb_cli_apply_changes(vty, FRR_GMP_INTERFACE_XPATH,
				    "frr-routing:ipv4");
}

DEFUN (interface_no_ip_igmp_version,
       interface_no_ip_igmp_version_cmd,
       "no ip igmp version (2-3)",
       NO_STR
       IP_STR
       IFACE_IGMP_STR
       "IGMP version\n"
       "IGMP version number\n")
{
	nb_cli_enqueue_change(vty, "./igmp-version", NB_OP_DESTROY, NULL);

	return nb_cli_apply_changes(vty, FRR_GMP_INTERFACE_XPATH,
				    "frr-routing:ipv4");
}

DEFUN (interface_ip_igmp_query_max_response_time,
       interface_ip_igmp_query_max_response_time_cmd,
       "ip igmp query-max-response-time (1-65535)",
       IP_STR
       IFACE_IGMP_STR
       IFACE_IGMP_QUERY_MAX_RESPONSE_TIME_STR
       "Query response value in deci-seconds\n")
{
	const struct lyd_node *pim_enable_dnode;

	pim_enable_dnode =
		yang_dnode_getf(vty->candidate_config->dnode,
				FRR_PIM_ENABLE_XPATH, VTY_CURR_XPATH,
				"frr-routing:ipv4");

	if (!pim_enable_dnode) {
		nb_cli_enqueue_change(vty, "./enable", NB_OP_MODIFY,
				      "true");
	} else {
		if (!yang_dnode_get_bool(pim_enable_dnode, "."))
			nb_cli_enqueue_change(vty, "./enable",
					      NB_OP_MODIFY, "true");
	}

	nb_cli_enqueue_change(vty, "./query-max-response-time", NB_OP_MODIFY,
			      argv[3]->arg);

	return nb_cli_apply_changes(vty, FRR_GMP_INTERFACE_XPATH,
				    "frr-routing:ipv4");
}

DEFUN (interface_no_ip_igmp_query_max_response_time,
       interface_no_ip_igmp_query_max_response_time_cmd,
       "no ip igmp query-max-response-time [(1-65535)]",
       NO_STR
       IP_STR
       IFACE_IGMP_STR
       IFACE_IGMP_QUERY_MAX_RESPONSE_TIME_STR
       IGNORED_IN_NO_STR)
{
	nb_cli_enqueue_change(vty, "./query-max-response-time", NB_OP_DESTROY,
			      NULL);
	return nb_cli_apply_changes(vty, FRR_GMP_INTERFACE_XPATH,
				    "frr-routing:ipv4");
}

DEFUN_HIDDEN (interface_ip_igmp_query_max_response_time_dsec,
	      interface_ip_igmp_query_max_response_time_dsec_cmd,
	      "ip igmp query-max-response-time-dsec (1-65535)",
	      IP_STR
	      IFACE_IGMP_STR
	      IFACE_IGMP_QUERY_MAX_RESPONSE_TIME_DSEC_STR
	      "Query response value in deciseconds\n")
{
	const struct lyd_node *pim_enable_dnode;

	pim_enable_dnode =
		yang_dnode_getf(vty->candidate_config->dnode,
				FRR_PIM_ENABLE_XPATH, VTY_CURR_XPATH,
				"frr-routing:ipv4");
	if (!pim_enable_dnode) {
		nb_cli_enqueue_change(vty, "./enable", NB_OP_MODIFY,
				      "true");
	} else {
		if (!yang_dnode_get_bool(pim_enable_dnode, "."))
			nb_cli_enqueue_change(vty, "./enable",
					      NB_OP_MODIFY, "true");
	}

	nb_cli_enqueue_change(vty, "./query-max-response-time", NB_OP_MODIFY,
			      argv[3]->arg);

	return nb_cli_apply_changes(vty, FRR_GMP_INTERFACE_XPATH,
				    "frr-routing:ipv4");
}

DEFUN_HIDDEN (interface_no_ip_igmp_query_max_response_time_dsec,
	      interface_no_ip_igmp_query_max_response_time_dsec_cmd,
	      "no ip igmp query-max-response-time-dsec [(1-65535)]",
	      NO_STR
	      IP_STR
	      IFACE_IGMP_STR
	      IFACE_IGMP_QUERY_MAX_RESPONSE_TIME_DSEC_STR
	      IGNORED_IN_NO_STR)
{
	nb_cli_enqueue_change(vty, "./query-max-response-time", NB_OP_DESTROY,
			      NULL);

	return nb_cli_apply_changes(vty, FRR_GMP_INTERFACE_XPATH,
				    "frr-routing:ipv4");
}

DEFUN (interface_ip_igmp_last_member_query_count,
       interface_ip_igmp_last_member_query_count_cmd,
       "ip igmp last-member-query-count (1-255)",
       IP_STR
       IFACE_IGMP_STR
       IFACE_IGMP_LAST_MEMBER_QUERY_COUNT_STR
       "Last member query count\n")
{
	const struct lyd_node *pim_enable_dnode;

	pim_enable_dnode =
		yang_dnode_getf(vty->candidate_config->dnode,
				FRR_PIM_ENABLE_XPATH, VTY_CURR_XPATH,
				"frr-routing:ipv4");
	if (!pim_enable_dnode) {
		nb_cli_enqueue_change(vty, "./enable", NB_OP_MODIFY,
				      "true");
	} else {
		if (!yang_dnode_get_bool(pim_enable_dnode, "."))
			nb_cli_enqueue_change(vty, "./enable",
					      NB_OP_MODIFY, "true");
	}

	nb_cli_enqueue_change(vty, "./robustness-variable", NB_OP_MODIFY,
			      argv[3]->arg);

	return nb_cli_apply_changes(vty, FRR_GMP_INTERFACE_XPATH,
				    "frr-routing:ipv4");
}

DEFUN (interface_no_ip_igmp_last_member_query_count,
       interface_no_ip_igmp_last_member_query_count_cmd,
       "no ip igmp last-member-query-count [(1-255)]",
       NO_STR
       IP_STR
       IFACE_IGMP_STR
       IFACE_IGMP_LAST_MEMBER_QUERY_COUNT_STR
       IGNORED_IN_NO_STR)
{
	nb_cli_enqueue_change(vty, "./robustness-variable", NB_OP_DESTROY,
			      NULL);

	return nb_cli_apply_changes(vty, FRR_GMP_INTERFACE_XPATH,
				    "frr-routing:ipv4");
}

DEFUN (interface_ip_igmp_last_member_query_interval,
       interface_ip_igmp_last_member_query_interval_cmd,
       "ip igmp last-member-query-interval (1-65535)",
       IP_STR
       IFACE_IGMP_STR
       IFACE_IGMP_LAST_MEMBER_QUERY_INTERVAL_STR
       "Last member query interval in deciseconds\n")
{
	const struct lyd_node *pim_enable_dnode;

	pim_enable_dnode =
		yang_dnode_getf(vty->candidate_config->dnode,
				FRR_PIM_ENABLE_XPATH, VTY_CURR_XPATH,
				"frr-routing:ipv4");
	if (!pim_enable_dnode) {
		nb_cli_enqueue_change(vty, "./enable", NB_OP_MODIFY,
				      "true");
	} else {
		if (!yang_dnode_get_bool(pim_enable_dnode, "."))
			nb_cli_enqueue_change(vty, "./enable",
					      NB_OP_MODIFY, "true");
	}

	nb_cli_enqueue_change(vty, "./last-member-query-interval", NB_OP_MODIFY,
			      argv[3]->arg);

	return nb_cli_apply_changes(vty, FRR_GMP_INTERFACE_XPATH,
				    "frr-routing:ipv4");
}

DEFUN (interface_no_ip_igmp_last_member_query_interval,
       interface_no_ip_igmp_last_member_query_interval_cmd,
       "no ip igmp last-member-query-interval [(1-65535)]",
       NO_STR
       IP_STR
       IFACE_IGMP_STR
       IFACE_IGMP_LAST_MEMBER_QUERY_INTERVAL_STR
       IGNORED_IN_NO_STR)
{
	nb_cli_enqueue_change(vty, "./last-member-query-interval",
			      NB_OP_DESTROY, NULL);

	return nb_cli_apply_changes(vty, FRR_GMP_INTERFACE_XPATH,
				    "frr-routing:ipv4");
}

DEFUN (interface_ip_pim_drprio,
       interface_ip_pim_drprio_cmd,
       "ip pim drpriority (1-4294967295)",
       IP_STR
       PIM_STR
       "Set the Designated Router Election Priority\n"
       "Value of the new DR Priority\n")
{
	int idx_number = 3;

	nb_cli_enqueue_change(vty, "./dr-priority", NB_OP_MODIFY,
			      argv[idx_number]->arg);

	return nb_cli_apply_changes(vty, FRR_PIM_INTERFACE_XPATH,
				    "frr-routing:ipv4");
}

DEFUN (interface_no_ip_pim_drprio,
       interface_no_ip_pim_drprio_cmd,
       "no ip pim drpriority [(1-4294967295)]",
       NO_STR
       IP_STR
       PIM_STR
       "Revert the Designated Router Priority to default\n"
       "Old Value of the Priority\n")
{
	nb_cli_enqueue_change(vty, "./dr-priority", NB_OP_DESTROY, NULL);

	return nb_cli_apply_changes(vty, FRR_PIM_INTERFACE_XPATH,
				   "frr-routing:ipv4");
}

DEFPY_HIDDEN (interface_ip_igmp_query_generate,
	      interface_ip_igmp_query_generate_cmd,
	      "ip igmp generate-query-once [version (2-3)]",
	      IP_STR
	      IFACE_IGMP_STR
	      "Generate igmp general query once\n"
	      "IGMP version\n"
	      "IGMP version number\n")
{
	VTY_DECLVAR_CONTEXT(interface, ifp);
	int igmp_version = 2;

	if (!ifp->info) {
		vty_out(vty, "IGMP/PIM is not enabled on the interface %s\n",
			ifp->name);
		return CMD_WARNING_CONFIG_FAILED;
	}

	if (argc > 3)
		igmp_version = atoi(argv[4]->arg);

	igmp_send_query_on_intf(ifp, igmp_version);

	return CMD_SUCCESS;
}

DEFPY_HIDDEN (pim_test_sg_keepalive,
	      pim_test_sg_keepalive_cmd,
	      "test pim [vrf NAME$name] keepalive-reset A.B.C.D$source A.B.C.D$group",
	      "Test code\n"
	      PIM_STR
	      VRF_CMD_HELP_STR
	      "Reset the Keepalive Timer\n"
	      "The Source we are resetting\n"
	      "The Group we are resetting\n")
{
	struct pim_upstream *up;
	struct pim_instance *pim;
	pim_sgaddr sg;

	sg.src = source;
	sg.grp = group;

	if (!name)
		pim = pim_get_pim_instance(VRF_DEFAULT);
	else {
		struct vrf *vrf = vrf_lookup_by_name(name);

		if (!vrf) {
			vty_out(vty, "%% Vrf specified: %s does not exist\n",
				name);
			return CMD_WARNING;
		}

		pim = pim_get_pim_instance(vrf->vrf_id);
	}

	if (!pim) {
		vty_out(vty, "%% Unable to find pim instance\n");
		return CMD_WARNING;
	}

	up = pim_upstream_find(pim, &sg);
	if (!up) {
		vty_out(vty, "%% Unable to find %pSG specified\n", &sg);
		return CMD_WARNING;
	}

	vty_out(vty, "Setting %pSG to current keep alive time: %d\n", &sg,
		pim->keep_alive_time);
	pim_upstream_keep_alive_timer_start(up, pim->keep_alive_time);

	return CMD_SUCCESS;
}

DEFPY (interface_ip_pim_activeactive,
       interface_ip_pim_activeactive_cmd,
       "[no$no] ip pim active-active",
       NO_STR
       IP_STR
       PIM_STR
       "Mark interface as Active-Active for MLAG operations, Hidden because not finished yet\n")
{
	if (no)
		nb_cli_enqueue_change(vty, "./active-active", NB_OP_MODIFY,
				      "false");
	else {
		nb_cli_enqueue_change(vty, "./pim-enable", NB_OP_MODIFY,
				      "true");

		nb_cli_enqueue_change(vty, "./active-active", NB_OP_MODIFY,
				      "true");
	}

	return nb_cli_apply_changes(vty,
			FRR_PIM_INTERFACE_XPATH,
			"frr-routing:ipv4");
}

DEFUN_HIDDEN (interface_ip_pim_ssm,
	      interface_ip_pim_ssm_cmd,
	      "ip pim ssm",
	      IP_STR
	      PIM_STR
	      IFACE_PIM_STR)
{
	int ret;

	nb_cli_enqueue_change(vty, "./pim-enable", NB_OP_MODIFY, "true");

	ret = nb_cli_apply_changes(vty,
			FRR_PIM_INTERFACE_XPATH,
			"frr-routing:ipv4");

	if (ret != NB_OK)
		return ret;

	vty_out(vty,
		"WARN: Enabled PIM SM on interface; configure PIM SSM range if needed\n");

	return NB_OK;
}

DEFUN_HIDDEN (interface_ip_pim_sm,
	      interface_ip_pim_sm_cmd,
	      "ip pim sm",
	      IP_STR
	      PIM_STR
	      IFACE_PIM_SM_STR)
{
	nb_cli_enqueue_change(vty, "./pim-enable", NB_OP_MODIFY, "true");

	return nb_cli_apply_changes(vty,
			FRR_PIM_INTERFACE_XPATH,
			"frr-routing:ipv4");
}

DEFUN (interface_ip_pim,
       interface_ip_pim_cmd,
       "ip pim",
       IP_STR
       PIM_STR)
{
	nb_cli_enqueue_change(vty, "./pim-enable", NB_OP_MODIFY, "true");

	return nb_cli_apply_changes(vty,
			FRR_PIM_INTERFACE_XPATH,
			"frr-routing:ipv4");

}

DEFUN_HIDDEN (interface_no_ip_pim_ssm,
	      interface_no_ip_pim_ssm_cmd,
	      "no ip pim ssm",
	      NO_STR
	      IP_STR
	      PIM_STR
	      IFACE_PIM_STR)
{
	const struct lyd_node *igmp_enable_dnode;
	char igmp_if_xpath[XPATH_MAXLEN];

	int printed =
		snprintf(igmp_if_xpath, sizeof(igmp_if_xpath),
			 "%s/frr-gmp:gmp/address-family[address-family='%s']",
			 VTY_CURR_XPATH, "frr-routing:ipv4");

	if (printed >= (int)(sizeof(igmp_if_xpath))) {
		vty_out(vty, "Xpath too long (%d > %u)", printed + 1,
			XPATH_MAXLEN);
		return CMD_WARNING_CONFIG_FAILED;
	}

	igmp_enable_dnode = yang_dnode_getf(vty->candidate_config->dnode,
					    FRR_GMP_ENABLE_XPATH,
					    VTY_CURR_XPATH,
					    "frr-routing:ipv4");
	if (!igmp_enable_dnode) {
		nb_cli_enqueue_change(vty, igmp_if_xpath, NB_OP_DESTROY, NULL);
		nb_cli_enqueue_change(vty, ".", NB_OP_DESTROY, NULL);
	} else {
		if (!yang_dnode_get_bool(igmp_enable_dnode, ".")) {
			nb_cli_enqueue_change(vty, igmp_if_xpath, NB_OP_DESTROY,
					      NULL);
			nb_cli_enqueue_change(vty, ".", NB_OP_DESTROY, NULL);
		} else
			nb_cli_enqueue_change(vty, "./pim-enable", NB_OP_MODIFY,
					      "false");
	}

	return nb_cli_apply_changes(vty, FRR_PIM_INTERFACE_XPATH,
				    "frr-routing:ipv4");
}

DEFUN_HIDDEN (interface_no_ip_pim_sm,
	      interface_no_ip_pim_sm_cmd,
	      "no ip pim sm",
	      NO_STR
	      IP_STR
	      PIM_STR
	      IFACE_PIM_SM_STR)
{
	const struct lyd_node *igmp_enable_dnode;
	char igmp_if_xpath[XPATH_MAXLEN];

	int printed =
		snprintf(igmp_if_xpath, sizeof(igmp_if_xpath),
			 "%s/frr-gmp:gmp/address-family[address-family='%s']",
			 VTY_CURR_XPATH, "frr-routing:ipv4");

	if (printed >= (int)(sizeof(igmp_if_xpath))) {
		vty_out(vty, "Xpath too long (%d > %u)", printed + 1,
			XPATH_MAXLEN);
		return CMD_WARNING_CONFIG_FAILED;
	}

	igmp_enable_dnode =
		yang_dnode_getf(vty->candidate_config->dnode,
				FRR_GMP_ENABLE_XPATH, VTY_CURR_XPATH,
				"frr-routing:ipv4");

	if (!igmp_enable_dnode) {
		nb_cli_enqueue_change(vty, igmp_if_xpath, NB_OP_DESTROY, NULL);
		nb_cli_enqueue_change(vty, ".", NB_OP_DESTROY, NULL);
	} else {
		if (!yang_dnode_get_bool(igmp_enable_dnode, ".")) {
			nb_cli_enqueue_change(vty, igmp_if_xpath, NB_OP_DESTROY,
					      NULL);
			nb_cli_enqueue_change(vty, ".", NB_OP_DESTROY, NULL);
		} else
			nb_cli_enqueue_change(vty, "./pim-enable", NB_OP_MODIFY,
					      "false");
	}

	return nb_cli_apply_changes(vty,
			FRR_PIM_INTERFACE_XPATH,
			"frr-routing:ipv4");
}

DEFUN (interface_no_ip_pim,
       interface_no_ip_pim_cmd,
       "no ip pim",
       NO_STR
       IP_STR
       PIM_STR)
{
	const struct lyd_node *igmp_enable_dnode;
	char igmp_if_xpath[XPATH_MAXLEN];

	int printed =
		snprintf(igmp_if_xpath, sizeof(igmp_if_xpath),
			 "%s/frr-gmp:gmp/address-family[address-family='%s']",
			 VTY_CURR_XPATH, "frr-routing:ipv4");

	if (printed >= (int)(sizeof(igmp_if_xpath))) {
		vty_out(vty, "Xpath too long (%d > %u)", printed + 1,
			XPATH_MAXLEN);
		return CMD_WARNING_CONFIG_FAILED;
	}

	igmp_enable_dnode =
		yang_dnode_getf(vty->candidate_config->dnode,
				FRR_GMP_ENABLE_XPATH, VTY_CURR_XPATH,
				"frr-routing:ipv4");

	if (!igmp_enable_dnode) {
		nb_cli_enqueue_change(vty, igmp_if_xpath, NB_OP_DESTROY, NULL);
		nb_cli_enqueue_change(vty, ".", NB_OP_DESTROY, NULL);
	} else {
		if (!yang_dnode_get_bool(igmp_enable_dnode, ".")) {
			nb_cli_enqueue_change(vty, igmp_if_xpath, NB_OP_DESTROY,
					      NULL);
			nb_cli_enqueue_change(vty, ".", NB_OP_DESTROY, NULL);
		} else
			nb_cli_enqueue_change(vty, "./pim-enable", NB_OP_MODIFY,
					      "false");
	}

	return nb_cli_apply_changes(vty,
				    FRR_PIM_INTERFACE_XPATH,
				    "frr-routing:ipv4");
}

/* boundaries */
DEFUN(interface_ip_pim_boundary_oil,
      interface_ip_pim_boundary_oil_cmd,
      "ip multicast boundary oil WORD",
      IP_STR
      "Generic multicast configuration options\n"
      "Define multicast boundary\n"
      "Filter OIL by group using prefix list\n"
      "Prefix list to filter OIL with\n")
{
	nb_cli_enqueue_change(vty, "./multicast-boundary-oil", NB_OP_MODIFY,
			      argv[4]->arg);

	return nb_cli_apply_changes(vty,
				    FRR_PIM_INTERFACE_XPATH,
				    "frr-routing:ipv4");

}

DEFUN(interface_no_ip_pim_boundary_oil,
      interface_no_ip_pim_boundary_oil_cmd,
      "no ip multicast boundary oil [WORD]",
      NO_STR
      IP_STR
      "Generic multicast configuration options\n"
      "Define multicast boundary\n"
      "Filter OIL by group using prefix list\n"
      "Prefix list to filter OIL with\n")
{
	nb_cli_enqueue_change(vty, "./multicast-boundary-oil", NB_OP_DESTROY,
			      NULL);

	return nb_cli_apply_changes(vty,
				    FRR_PIM_INTERFACE_XPATH,
				    "frr-routing:ipv4");
}

DEFUN (interface_ip_mroute,
       interface_ip_mroute_cmd,
       "ip mroute INTERFACE A.B.C.D [A.B.C.D]",
       IP_STR
       "Add multicast route\n"
       "Outgoing interface name\n"
       "Group address\n"
       "Source address\n")
{
	int idx_interface = 2;
	int idx_ipv4 = 3;
	const char *source_str;

	if (argc == (idx_ipv4 + 1))
		source_str = "0.0.0.0";
	else
		source_str = argv[idx_ipv4 + 1]->arg;

	nb_cli_enqueue_change(vty, "./oif", NB_OP_MODIFY,
			      argv[idx_interface]->arg);

	return nb_cli_apply_changes(vty,
				    FRR_PIM_MROUTE_XPATH,
				    "frr-routing:ipv4", source_str,
				    argv[idx_ipv4]->arg);
}

DEFUN (interface_no_ip_mroute,
       interface_no_ip_mroute_cmd,
       "no ip mroute INTERFACE A.B.C.D [A.B.C.D]",
       NO_STR
       IP_STR
       "Add multicast route\n"
       "Outgoing interface name\n"
       "Group Address\n"
       "Source Address\n")
{
	int idx_ipv4 = 4;
	const char *source_str;

	if (argc == (idx_ipv4 + 1))
		source_str = "0.0.0.0";
	else
		source_str = argv[idx_ipv4 + 1]->arg;

	nb_cli_enqueue_change(vty, ".", NB_OP_DESTROY, NULL);

	return nb_cli_apply_changes(vty,
				    FRR_PIM_MROUTE_XPATH,
				    "frr-routing:ipv4", source_str,
				    argv[idx_ipv4]->arg);
}

DEFUN (interface_ip_pim_hello,
       interface_ip_pim_hello_cmd,
       "ip pim hello (1-65535) [(1-65535)]",
       IP_STR
       PIM_STR
       IFACE_PIM_HELLO_STR
       IFACE_PIM_HELLO_TIME_STR
       IFACE_PIM_HELLO_HOLD_STR)
{
	int idx_time = 3;
	int idx_hold = 4;
	const struct lyd_node *igmp_enable_dnode;

	igmp_enable_dnode =
		yang_dnode_getf(vty->candidate_config->dnode,
				FRR_GMP_ENABLE_XPATH, VTY_CURR_XPATH,
				"frr-routing:ipv4");
	if (!igmp_enable_dnode) {
		nb_cli_enqueue_change(vty, "./pim-enable", NB_OP_MODIFY,
				      "true");
	} else {
		if (!yang_dnode_get_bool(igmp_enable_dnode, "."))
			nb_cli_enqueue_change(vty, "./pim-enable", NB_OP_MODIFY,
					      "true");
	}

	nb_cli_enqueue_change(vty, "./hello-interval", NB_OP_MODIFY,
			      argv[idx_time]->arg);

	if (argc == idx_hold + 1)
		nb_cli_enqueue_change(vty, "./hello-holdtime", NB_OP_MODIFY,
				      argv[idx_hold]->arg);

	return nb_cli_apply_changes(vty,
				    FRR_PIM_INTERFACE_XPATH,
				    "frr-routing:ipv4");
}

DEFUN (interface_no_ip_pim_hello,
       interface_no_ip_pim_hello_cmd,
       "no ip pim hello [(1-65535) [(1-65535)]]",
       NO_STR
       IP_STR
       PIM_STR
       IFACE_PIM_HELLO_STR
       IGNORED_IN_NO_STR
       IGNORED_IN_NO_STR)
{
	nb_cli_enqueue_change(vty, "./hello-interval", NB_OP_DESTROY, NULL);
	nb_cli_enqueue_change(vty, "./hello-holdtime", NB_OP_DESTROY, NULL);

	return nb_cli_apply_changes(vty,
				    FRR_PIM_INTERFACE_XPATH,
				    "frr-routing:ipv4");
}

DEFUN (debug_igmp,
       debug_igmp_cmd,
       "debug igmp",
       DEBUG_STR
       DEBUG_IGMP_STR)
{
	PIM_DO_DEBUG_IGMP_EVENTS;
	PIM_DO_DEBUG_IGMP_PACKETS;
	PIM_DO_DEBUG_IGMP_TRACE;
	return CMD_SUCCESS;
}

DEFUN (no_debug_igmp,
       no_debug_igmp_cmd,
       "no debug igmp",
       NO_STR
       DEBUG_STR
       DEBUG_IGMP_STR)
{
	PIM_DONT_DEBUG_IGMP_EVENTS;
	PIM_DONT_DEBUG_IGMP_PACKETS;
	PIM_DONT_DEBUG_IGMP_TRACE;
	return CMD_SUCCESS;
}


DEFUN (debug_igmp_events,
       debug_igmp_events_cmd,
       "debug igmp events",
       DEBUG_STR
       DEBUG_IGMP_STR
       DEBUG_IGMP_EVENTS_STR)
{
	PIM_DO_DEBUG_IGMP_EVENTS;
	return CMD_SUCCESS;
}

DEFUN (no_debug_igmp_events,
       no_debug_igmp_events_cmd,
       "no debug igmp events",
       NO_STR
       DEBUG_STR
       DEBUG_IGMP_STR
       DEBUG_IGMP_EVENTS_STR)
{
	PIM_DONT_DEBUG_IGMP_EVENTS;
	return CMD_SUCCESS;
}


DEFUN (debug_igmp_packets,
       debug_igmp_packets_cmd,
       "debug igmp packets",
       DEBUG_STR
       DEBUG_IGMP_STR
       DEBUG_IGMP_PACKETS_STR)
{
	PIM_DO_DEBUG_IGMP_PACKETS;
	return CMD_SUCCESS;
}

DEFUN (no_debug_igmp_packets,
       no_debug_igmp_packets_cmd,
       "no debug igmp packets",
       NO_STR
       DEBUG_STR
       DEBUG_IGMP_STR
       DEBUG_IGMP_PACKETS_STR)
{
	PIM_DONT_DEBUG_IGMP_PACKETS;
	return CMD_SUCCESS;
}


DEFUN (debug_igmp_trace,
       debug_igmp_trace_cmd,
       "debug igmp trace",
       DEBUG_STR
       DEBUG_IGMP_STR
       DEBUG_IGMP_TRACE_STR)
{
	PIM_DO_DEBUG_IGMP_TRACE;
	return CMD_SUCCESS;
}

DEFUN (no_debug_igmp_trace,
       no_debug_igmp_trace_cmd,
       "no debug igmp trace",
       NO_STR
       DEBUG_STR
       DEBUG_IGMP_STR
       DEBUG_IGMP_TRACE_STR)
{
	PIM_DONT_DEBUG_IGMP_TRACE;
	return CMD_SUCCESS;
}


DEFUN (debug_mroute,
       debug_mroute_cmd,
       "debug mroute",
       DEBUG_STR
       DEBUG_MROUTE_STR)
{
	PIM_DO_DEBUG_MROUTE;
	return CMD_SUCCESS;
}

DEFUN (debug_mroute_detail,
       debug_mroute_detail_cmd,
       "debug mroute detail",
       DEBUG_STR
       DEBUG_MROUTE_STR
       "detailed\n")
{
	PIM_DO_DEBUG_MROUTE_DETAIL;
	return CMD_SUCCESS;
}

DEFUN (no_debug_mroute,
       no_debug_mroute_cmd,
       "no debug mroute",
       NO_STR
       DEBUG_STR
       DEBUG_MROUTE_STR)
{
	PIM_DONT_DEBUG_MROUTE;
	return CMD_SUCCESS;
}

DEFUN (no_debug_mroute_detail,
       no_debug_mroute_detail_cmd,
       "no debug mroute detail",
       NO_STR
       DEBUG_STR
       DEBUG_MROUTE_STR
       "detailed\n")
{
	PIM_DONT_DEBUG_MROUTE_DETAIL;
	return CMD_SUCCESS;
}

DEFUN (debug_pim_static,
       debug_pim_static_cmd,
       "debug pim static",
       DEBUG_STR
       DEBUG_PIM_STR
       DEBUG_STATIC_STR)
{
	PIM_DO_DEBUG_STATIC;
	return CMD_SUCCESS;
}

DEFUN (no_debug_pim_static,
       no_debug_pim_static_cmd,
       "no debug pim static",
       NO_STR
       DEBUG_STR
       DEBUG_PIM_STR
       DEBUG_STATIC_STR)
{
	PIM_DONT_DEBUG_STATIC;
	return CMD_SUCCESS;
}


DEFUN (debug_pim,
       debug_pim_cmd,
       "debug pim",
       DEBUG_STR
       DEBUG_PIM_STR)
{
	PIM_DO_DEBUG_PIM_EVENTS;
	PIM_DO_DEBUG_PIM_PACKETS;
	PIM_DO_DEBUG_PIM_TRACE;
	PIM_DO_DEBUG_MSDP_EVENTS;
	PIM_DO_DEBUG_MSDP_PACKETS;
	PIM_DO_DEBUG_BSM;
	return CMD_SUCCESS;
}

DEFUN (no_debug_pim,
       no_debug_pim_cmd,
       "no debug pim",
       NO_STR
       DEBUG_STR
       DEBUG_PIM_STR)
{
	PIM_DONT_DEBUG_PIM_EVENTS;
	PIM_DONT_DEBUG_PIM_PACKETS;
	PIM_DONT_DEBUG_PIM_TRACE;
	PIM_DONT_DEBUG_MSDP_EVENTS;
	PIM_DONT_DEBUG_MSDP_PACKETS;

	PIM_DONT_DEBUG_PIM_PACKETDUMP_SEND;
	PIM_DONT_DEBUG_PIM_PACKETDUMP_RECV;
	PIM_DONT_DEBUG_BSM;

	return CMD_SUCCESS;
}

DEFUN (debug_pim_nht,
       debug_pim_nht_cmd,
       "debug pim nht",
       DEBUG_STR
       DEBUG_PIM_STR
       "Nexthop Tracking\n")
{
	PIM_DO_DEBUG_PIM_NHT;
	return CMD_SUCCESS;
}

DEFUN (no_debug_pim_nht,
       no_debug_pim_nht_cmd,
       "no debug pim nht",
       NO_STR
       DEBUG_STR
       DEBUG_PIM_STR
       "Nexthop Tracking\n")
{
	PIM_DONT_DEBUG_PIM_NHT;
	return CMD_SUCCESS;
}

DEFUN (debug_pim_nht_rp,
       debug_pim_nht_rp_cmd,
       "debug pim nht rp",
       DEBUG_STR
       DEBUG_PIM_STR
       "Nexthop Tracking\n"
       "RP Nexthop Tracking\n")
{
	PIM_DO_DEBUG_PIM_NHT_RP;
	return CMD_SUCCESS;
}

DEFUN (no_debug_pim_nht_rp,
       no_debug_pim_nht_rp_cmd,
       "no debug pim nht rp",
       NO_STR
       DEBUG_STR
       DEBUG_PIM_STR
       "Nexthop Tracking\n"
       "RP Nexthop Tracking\n")
{
	PIM_DONT_DEBUG_PIM_NHT_RP;
	return CMD_SUCCESS;
}

DEFUN (debug_pim_events,
       debug_pim_events_cmd,
       "debug pim events",
       DEBUG_STR
       DEBUG_PIM_STR
       DEBUG_PIM_EVENTS_STR)
{
	PIM_DO_DEBUG_PIM_EVENTS;
	return CMD_SUCCESS;
}

DEFUN (no_debug_pim_events,
       no_debug_pim_events_cmd,
       "no debug pim events",
       NO_STR
       DEBUG_STR
       DEBUG_PIM_STR
       DEBUG_PIM_EVENTS_STR)
{
	PIM_DONT_DEBUG_PIM_EVENTS;
	return CMD_SUCCESS;
}

DEFUN (debug_pim_packets,
       debug_pim_packets_cmd,
       "debug pim packets [<hello|joins|register>]",
       DEBUG_STR
       DEBUG_PIM_STR
       DEBUG_PIM_PACKETS_STR
       DEBUG_PIM_HELLO_PACKETS_STR
       DEBUG_PIM_J_P_PACKETS_STR
       DEBUG_PIM_PIM_REG_PACKETS_STR)
{
	int idx = 0;
	if (argv_find(argv, argc, "hello", &idx)) {
		PIM_DO_DEBUG_PIM_HELLO;
		vty_out(vty, "PIM Hello debugging is on\n");
	} else if (argv_find(argv, argc, "joins", &idx)) {
		PIM_DO_DEBUG_PIM_J_P;
		vty_out(vty, "PIM Join/Prune debugging is on\n");
	} else if (argv_find(argv, argc, "register", &idx)) {
		PIM_DO_DEBUG_PIM_REG;
		vty_out(vty, "PIM Register debugging is on\n");
	} else {
		PIM_DO_DEBUG_PIM_PACKETS;
		vty_out(vty, "PIM Packet debugging is on \n");
	}
	return CMD_SUCCESS;
}

DEFUN (no_debug_pim_packets,
       no_debug_pim_packets_cmd,
       "no debug pim packets [<hello|joins|register>]",
       NO_STR
       DEBUG_STR
       DEBUG_PIM_STR
       DEBUG_PIM_PACKETS_STR
       DEBUG_PIM_HELLO_PACKETS_STR
       DEBUG_PIM_J_P_PACKETS_STR
       DEBUG_PIM_PIM_REG_PACKETS_STR)
{
	int idx = 0;
	if (argv_find(argv, argc, "hello", &idx)) {
		PIM_DONT_DEBUG_PIM_HELLO;
		vty_out(vty, "PIM Hello debugging is off \n");
	} else if (argv_find(argv, argc, "joins", &idx)) {
		PIM_DONT_DEBUG_PIM_J_P;
		vty_out(vty, "PIM Join/Prune debugging is off \n");
	} else if (argv_find(argv, argc, "register", &idx)) {
		PIM_DONT_DEBUG_PIM_REG;
		vty_out(vty, "PIM Register debugging is off\n");
	} else
		PIM_DONT_DEBUG_PIM_PACKETS;

	return CMD_SUCCESS;
}


DEFUN (debug_pim_packetdump_send,
       debug_pim_packetdump_send_cmd,
       "debug pim packet-dump send",
       DEBUG_STR
       DEBUG_PIM_STR
       DEBUG_PIM_PACKETDUMP_STR
       DEBUG_PIM_PACKETDUMP_SEND_STR)
{
	PIM_DO_DEBUG_PIM_PACKETDUMP_SEND;
	return CMD_SUCCESS;
}

DEFUN (no_debug_pim_packetdump_send,
       no_debug_pim_packetdump_send_cmd,
       "no debug pim packet-dump send",
       NO_STR
       DEBUG_STR
       DEBUG_PIM_STR
       DEBUG_PIM_PACKETDUMP_STR
       DEBUG_PIM_PACKETDUMP_SEND_STR)
{
	PIM_DONT_DEBUG_PIM_PACKETDUMP_SEND;
	return CMD_SUCCESS;
}

DEFUN (debug_pim_packetdump_recv,
       debug_pim_packetdump_recv_cmd,
       "debug pim packet-dump receive",
       DEBUG_STR
       DEBUG_PIM_STR
       DEBUG_PIM_PACKETDUMP_STR
       DEBUG_PIM_PACKETDUMP_RECV_STR)
{
	PIM_DO_DEBUG_PIM_PACKETDUMP_RECV;
	return CMD_SUCCESS;
}

DEFUN (no_debug_pim_packetdump_recv,
       no_debug_pim_packetdump_recv_cmd,
       "no debug pim packet-dump receive",
       NO_STR
       DEBUG_STR
       DEBUG_PIM_STR
       DEBUG_PIM_PACKETDUMP_STR
       DEBUG_PIM_PACKETDUMP_RECV_STR)
{
	PIM_DONT_DEBUG_PIM_PACKETDUMP_RECV;
	return CMD_SUCCESS;
}

DEFUN (debug_pim_trace,
       debug_pim_trace_cmd,
       "debug pim trace",
       DEBUG_STR
       DEBUG_PIM_STR
       DEBUG_PIM_TRACE_STR)
{
	PIM_DO_DEBUG_PIM_TRACE;
	return CMD_SUCCESS;
}

DEFUN (debug_pim_trace_detail,
       debug_pim_trace_detail_cmd,
       "debug pim trace detail",
       DEBUG_STR
       DEBUG_PIM_STR
       DEBUG_PIM_TRACE_STR
       "Detailed Information\n")
{
	PIM_DO_DEBUG_PIM_TRACE_DETAIL;
	return CMD_SUCCESS;
}

DEFUN (no_debug_pim_trace,
       no_debug_pim_trace_cmd,
       "no debug pim trace",
       NO_STR
       DEBUG_STR
       DEBUG_PIM_STR
       DEBUG_PIM_TRACE_STR)
{
	PIM_DONT_DEBUG_PIM_TRACE;
	return CMD_SUCCESS;
}

DEFUN (no_debug_pim_trace_detail,
       no_debug_pim_trace_detail_cmd,
       "no debug pim trace detail",
       NO_STR
       DEBUG_STR
       DEBUG_PIM_STR
       DEBUG_PIM_TRACE_STR
       "Detailed Information\n")
{
	PIM_DONT_DEBUG_PIM_TRACE_DETAIL;
	return CMD_SUCCESS;
}

DEFUN (debug_ssmpingd,
       debug_ssmpingd_cmd,
       "debug ssmpingd",
       DEBUG_STR
       DEBUG_SSMPINGD_STR)
{
	PIM_DO_DEBUG_SSMPINGD;
	return CMD_SUCCESS;
}

DEFUN (no_debug_ssmpingd,
       no_debug_ssmpingd_cmd,
       "no debug ssmpingd",
       NO_STR
       DEBUG_STR
       DEBUG_SSMPINGD_STR)
{
	PIM_DONT_DEBUG_SSMPINGD;
	return CMD_SUCCESS;
}

DEFUN (debug_pim_zebra,
       debug_pim_zebra_cmd,
       "debug pim zebra",
       DEBUG_STR
       DEBUG_PIM_STR
       DEBUG_PIM_ZEBRA_STR)
{
	PIM_DO_DEBUG_ZEBRA;
	return CMD_SUCCESS;
}

DEFUN (no_debug_pim_zebra,
       no_debug_pim_zebra_cmd,
       "no debug pim zebra",
       NO_STR
       DEBUG_STR
       DEBUG_PIM_STR
       DEBUG_PIM_ZEBRA_STR)
{
	PIM_DONT_DEBUG_ZEBRA;
	return CMD_SUCCESS;
}

DEFUN(debug_pim_mlag, debug_pim_mlag_cmd, "debug pim mlag",
      DEBUG_STR DEBUG_PIM_STR DEBUG_PIM_MLAG_STR)
{
	PIM_DO_DEBUG_MLAG;
	return CMD_SUCCESS;
}

DEFUN(no_debug_pim_mlag, no_debug_pim_mlag_cmd, "no debug pim mlag",
      NO_STR DEBUG_STR DEBUG_PIM_STR DEBUG_PIM_MLAG_STR)
{
	PIM_DONT_DEBUG_MLAG;
	return CMD_SUCCESS;
}

DEFUN (debug_pim_vxlan,
       debug_pim_vxlan_cmd,
       "debug pim vxlan",
       DEBUG_STR
       DEBUG_PIM_STR
       DEBUG_PIM_VXLAN_STR)
{
	PIM_DO_DEBUG_VXLAN;
	return CMD_SUCCESS;
}

DEFUN (no_debug_pim_vxlan,
       no_debug_pim_vxlan_cmd,
       "no debug pim vxlan",
       NO_STR
       DEBUG_STR
       DEBUG_PIM_STR
       DEBUG_PIM_VXLAN_STR)
{
	PIM_DONT_DEBUG_VXLAN;
	return CMD_SUCCESS;
}

DEFUN (debug_msdp,
       debug_msdp_cmd,
       "debug msdp",
       DEBUG_STR
       DEBUG_MSDP_STR)
{
	PIM_DO_DEBUG_MSDP_EVENTS;
	PIM_DO_DEBUG_MSDP_PACKETS;
	return CMD_SUCCESS;
}

DEFUN (no_debug_msdp,
       no_debug_msdp_cmd,
       "no debug msdp",
       NO_STR
       DEBUG_STR
       DEBUG_MSDP_STR)
{
	PIM_DONT_DEBUG_MSDP_EVENTS;
	PIM_DONT_DEBUG_MSDP_PACKETS;
	return CMD_SUCCESS;
}

DEFUN (debug_msdp_events,
       debug_msdp_events_cmd,
       "debug msdp events",
       DEBUG_STR
       DEBUG_MSDP_STR
       DEBUG_MSDP_EVENTS_STR)
{
	PIM_DO_DEBUG_MSDP_EVENTS;
	return CMD_SUCCESS;
}

DEFUN (no_debug_msdp_events,
       no_debug_msdp_events_cmd,
       "no debug msdp events",
       NO_STR
       DEBUG_STR
       DEBUG_MSDP_STR
       DEBUG_MSDP_EVENTS_STR)
{
	PIM_DONT_DEBUG_MSDP_EVENTS;
	return CMD_SUCCESS;
}

DEFUN (debug_msdp_packets,
       debug_msdp_packets_cmd,
       "debug msdp packets",
       DEBUG_STR
       DEBUG_MSDP_STR
       DEBUG_MSDP_PACKETS_STR)
{
	PIM_DO_DEBUG_MSDP_PACKETS;
	return CMD_SUCCESS;
}

DEFUN (no_debug_msdp_packets,
       no_debug_msdp_packets_cmd,
       "no debug msdp packets",
       NO_STR
       DEBUG_STR
       DEBUG_MSDP_STR
       DEBUG_MSDP_PACKETS_STR)
{
	PIM_DONT_DEBUG_MSDP_PACKETS;
	return CMD_SUCCESS;
}

DEFUN (debug_mtrace,
       debug_mtrace_cmd,
       "debug mtrace",
       DEBUG_STR
       DEBUG_MTRACE_STR)
{
	PIM_DO_DEBUG_MTRACE;
	return CMD_SUCCESS;
}

DEFUN (no_debug_mtrace,
       no_debug_mtrace_cmd,
       "no debug mtrace",
       NO_STR
       DEBUG_STR
       DEBUG_MTRACE_STR)
{
	PIM_DONT_DEBUG_MTRACE;
	return CMD_SUCCESS;
}

DEFUN (debug_bsm,
       debug_bsm_cmd,
       "debug pim bsm",
       DEBUG_STR
       DEBUG_PIM_STR
       DEBUG_PIM_BSM_STR)
{
	PIM_DO_DEBUG_BSM;
	return CMD_SUCCESS;
}

DEFUN (no_debug_bsm,
       no_debug_bsm_cmd,
       "no debug pim bsm",
       NO_STR
       DEBUG_STR
       DEBUG_PIM_STR
       DEBUG_PIM_BSM_STR)
{
	PIM_DONT_DEBUG_BSM;
	return CMD_SUCCESS;
}


DEFUN_NOSH (show_debugging_pim,
	    show_debugging_pim_cmd,
	    "show debugging [pim]",
	    SHOW_STR
	    DEBUG_STR
	    PIM_STR)
{
	vty_out(vty, "PIM debugging status\n");

	pim_debug_config_write(vty);

	return CMD_SUCCESS;
}

DEFUN (interface_pim_use_source,
       interface_pim_use_source_cmd,
       "ip pim use-source A.B.C.D",
       IP_STR
       PIM_STR
       "Configure primary IP address\n"
       "source ip address\n")
{
	nb_cli_enqueue_change(vty, "./use-source", NB_OP_MODIFY, argv[3]->arg);

	return nb_cli_apply_changes(vty,
				    FRR_PIM_INTERFACE_XPATH,
				    "frr-routing:ipv4");
}

DEFUN (interface_no_pim_use_source,
       interface_no_pim_use_source_cmd,
       "no ip pim use-source [A.B.C.D]",
       NO_STR
       IP_STR
       PIM_STR
       "Delete source IP address\n"
       "source ip address\n")
{
	nb_cli_enqueue_change(vty, "./use-source", NB_OP_MODIFY, "0.0.0.0");

	return nb_cli_apply_changes(vty,
				    FRR_PIM_INTERFACE_XPATH,
				    "frr-routing:ipv4");
}

DEFPY (ip_pim_bfd,
       ip_pim_bfd_cmd,
       "ip pim bfd [profile BFDPROF$prof]",
       IP_STR
       PIM_STR
       "Enables BFD support\n"
       "Use BFD profile\n"
       "Use BFD profile name\n")
{
	const struct lyd_node *igmp_enable_dnode;

	igmp_enable_dnode =
		yang_dnode_getf(vty->candidate_config->dnode,
				FRR_GMP_ENABLE_XPATH, VTY_CURR_XPATH,
				"frr-routing:ipv4");
	if (!igmp_enable_dnode)
		nb_cli_enqueue_change(vty, "./pim-enable", NB_OP_MODIFY,
				      "true");
	else {
		if (!yang_dnode_get_bool(igmp_enable_dnode, "."))
			nb_cli_enqueue_change(vty, "./pim-enable", NB_OP_MODIFY,
					      "true");
	}

	nb_cli_enqueue_change(vty, "./bfd", NB_OP_CREATE, NULL);
	if (prof)
		nb_cli_enqueue_change(vty, "./bfd/profile", NB_OP_MODIFY, prof);

	return nb_cli_apply_changes(vty,
				    FRR_PIM_INTERFACE_XPATH,
				    "frr-routing:ipv4");
}

DEFPY(no_ip_pim_bfd_profile, no_ip_pim_bfd_profile_cmd,
      "no ip pim bfd profile [BFDPROF]",
      NO_STR
      IP_STR
      PIM_STR
      "Enables BFD support\n"
      "Disable BFD profile\n"
      "BFD Profile name\n")
{
	nb_cli_enqueue_change(vty, "./bfd/profile", NB_OP_DESTROY, NULL);

	return nb_cli_apply_changes(vty,
			FRR_PIM_INTERFACE_XPATH,
			"frr-routing:ipv4");
}

DEFUN (no_ip_pim_bfd,
       no_ip_pim_bfd_cmd,
       "no ip pim bfd",
       NO_STR
       IP_STR
       PIM_STR
       "Disables BFD support\n")
{
	nb_cli_enqueue_change(vty, "./bfd", NB_OP_DESTROY, NULL);

	return nb_cli_apply_changes(vty,
			FRR_PIM_INTERFACE_XPATH,
			"frr-routing:ipv4");
}

DEFUN (ip_pim_bsm,
       ip_pim_bsm_cmd,
       "ip pim bsm",
       IP_STR
       PIM_STR
       "Enables BSM support on the interface\n")
{
	const struct lyd_node *igmp_enable_dnode;

	igmp_enable_dnode =
		yang_dnode_getf(vty->candidate_config->dnode,
				FRR_GMP_ENABLE_XPATH, VTY_CURR_XPATH,
				"frr-routing:ipv4");
	if (!igmp_enable_dnode)
		nb_cli_enqueue_change(vty, "./pim-enable", NB_OP_MODIFY,
				      "true");
	else {
		if (!yang_dnode_get_bool(igmp_enable_dnode, "."))
			nb_cli_enqueue_change(vty, "./pim-enable", NB_OP_MODIFY,
					      "true");
	}

	nb_cli_enqueue_change(vty, "./bsm", NB_OP_MODIFY, "true");

	return nb_cli_apply_changes(vty,
			FRR_PIM_INTERFACE_XPATH, "frr-routing:ipv4");
}

DEFUN (no_ip_pim_bsm,
       no_ip_pim_bsm_cmd,
       "no ip pim bsm",
       NO_STR
       IP_STR
       PIM_STR
       "Disables BSM support\n")
{
	nb_cli_enqueue_change(vty, "./bsm", NB_OP_MODIFY, "false");

	return nb_cli_apply_changes(vty,
			FRR_PIM_INTERFACE_XPATH, "frr-routing:ipv4");
}

DEFUN (ip_pim_ucast_bsm,
       ip_pim_ucast_bsm_cmd,
       "ip pim unicast-bsm",
       IP_STR
       PIM_STR
       "Accept/Send unicast BSM on the interface\n")
{
	const struct lyd_node *igmp_enable_dnode;

	igmp_enable_dnode =
		yang_dnode_getf(vty->candidate_config->dnode,
				FRR_GMP_ENABLE_XPATH, VTY_CURR_XPATH,
				"frr-routing:ipv4");
	if (!igmp_enable_dnode)
		nb_cli_enqueue_change(vty, "./pim-enable", NB_OP_MODIFY,
				      "true");
	else {
		if (!yang_dnode_get_bool(igmp_enable_dnode, "."))
			nb_cli_enqueue_change(vty, "./pim-enable", NB_OP_MODIFY,
					      "true");
	}

	nb_cli_enqueue_change(vty, "./unicast-bsm", NB_OP_MODIFY, "true");

	return nb_cli_apply_changes(vty,
			FRR_PIM_INTERFACE_XPATH, "frr-routing:ipv4");

}

DEFUN (no_ip_pim_ucast_bsm,
       no_ip_pim_ucast_bsm_cmd,
       "no ip pim unicast-bsm",
       NO_STR
       IP_STR
       PIM_STR
       "Block send/receive unicast BSM on this interface\n")
{
	nb_cli_enqueue_change(vty, "./unicast-bsm", NB_OP_MODIFY, "false");

	return nb_cli_apply_changes(vty,
			FRR_PIM_INTERFACE_XPATH, "frr-routing:ipv4");
}

#if HAVE_BFDD > 0
DEFUN_HIDDEN (
	ip_pim_bfd_param,
	ip_pim_bfd_param_cmd,
	"ip pim bfd (2-255) (1-65535) (1-65535)",
	IP_STR
	PIM_STR
	"Enables BFD support\n"
	"Detect Multiplier\n"
	"Required min receive interval\n"
	"Desired min transmit interval\n")
#else
	DEFUN(
		ip_pim_bfd_param,
		ip_pim_bfd_param_cmd,
		"ip pim bfd (2-255) (1-65535) (1-65535)",
		IP_STR
		PIM_STR
		"Enables BFD support\n"
		"Detect Multiplier\n"
		"Required min receive interval\n"
		"Desired min transmit interval\n")
#endif /* HAVE_BFDD */
{
	int idx_number = 3;
	int idx_number_2 = 4;
	int idx_number_3 = 5;
	const struct lyd_node *igmp_enable_dnode;

	igmp_enable_dnode =
		yang_dnode_getf(vty->candidate_config->dnode,
				FRR_GMP_ENABLE_XPATH, VTY_CURR_XPATH,
				"frr-routing:ipv4");
	if (!igmp_enable_dnode)
		nb_cli_enqueue_change(vty, "./pim-enable", NB_OP_MODIFY,
				      "true");
	else {
		if (!yang_dnode_get_bool(igmp_enable_dnode, "."))
			nb_cli_enqueue_change(vty, "./pim-enable", NB_OP_MODIFY,
					      "true");
	}

	nb_cli_enqueue_change(vty, "./bfd", NB_OP_CREATE, NULL);
	nb_cli_enqueue_change(vty, "./bfd/min-rx-interval", NB_OP_MODIFY,
			      argv[idx_number_2]->arg);
	nb_cli_enqueue_change(vty, "./bfd/min-tx-interval", NB_OP_MODIFY,
			      argv[idx_number_3]->arg);
	nb_cli_enqueue_change(vty, "./bfd/detect_mult", NB_OP_MODIFY,
			      argv[idx_number]->arg);

	return nb_cli_apply_changes(vty,
			FRR_PIM_INTERFACE_XPATH, "frr-routing:ipv4");
}

#if HAVE_BFDD == 0
ALIAS(no_ip_pim_bfd, no_ip_pim_bfd_param_cmd,
      "no ip pim bfd (2-255) (1-65535) (1-65535)",
      NO_STR
      IP_STR
      PIM_STR
      "Enables BFD support\n"
      "Detect Multiplier\n"
      "Required min receive interval\n"
      "Desired min transmit interval\n")
#endif /* !HAVE_BFDD */

#if PIM_IPV != 6
DEFPY(ip_msdp_peer, ip_msdp_peer_cmd,
      "ip msdp peer A.B.C.D$peer source A.B.C.D$source",
      IP_STR
      CFG_MSDP_STR
      "Configure MSDP peer\n"
      "Peer IP address\n"
      "Source address for TCP connection\n"
      "Local IP address\n")
{
	const char *vrfname;
	char temp_xpath[XPATH_MAXLEN];
	char msdp_peer_source_xpath[XPATH_MAXLEN];

	vrfname = pim_cli_get_vrf_name(vty);
	if (vrfname == NULL)
		return CMD_WARNING_CONFIG_FAILED;

	snprintf(msdp_peer_source_xpath, sizeof(msdp_peer_source_xpath),
		 FRR_PIM_VRF_XPATH, "frr-pim:pimd", "pim", vrfname,
		 "frr-routing:ipv4");
	snprintf(temp_xpath, sizeof(temp_xpath),
		 "/msdp-peer[peer-ip='%s']/source-ip", peer_str);
	strlcat(msdp_peer_source_xpath, temp_xpath,
		sizeof(msdp_peer_source_xpath));

	nb_cli_enqueue_change(vty, msdp_peer_source_xpath, NB_OP_MODIFY,
			      source_str);

	return nb_cli_apply_changes(vty,
			FRR_PIM_INTERFACE_XPATH, "frr-routing:ipv4");
}

DEFPY(ip_msdp_timers, ip_msdp_timers_cmd,
      "ip msdp timers (1-65535)$keepalive (1-65535)$holdtime [(1-65535)$connretry]",
      IP_STR
      CFG_MSDP_STR
      "MSDP timers configuration\n"
      "Keep alive period (in seconds)\n"
      "Hold time period (in seconds)\n"
      "Connection retry period (in seconds)\n")
{
	const char *vrfname;
	char xpath[XPATH_MAXLEN];

	vrfname = pim_cli_get_vrf_name(vty);
	if (vrfname == NULL)
		return CMD_WARNING_CONFIG_FAILED;

	snprintf(xpath, sizeof(xpath), FRR_PIM_MSDP_XPATH, "frr-pim:pimd",
		 "pim", vrfname, "frr-routing:ipv4");
	nb_cli_enqueue_change(vty, "./hold-time", NB_OP_MODIFY, holdtime_str);
	nb_cli_enqueue_change(vty, "./keep-alive", NB_OP_MODIFY, keepalive_str);
	if (connretry_str)
		nb_cli_enqueue_change(vty, "./connection-retry", NB_OP_MODIFY,
				      connretry_str);
	else
		nb_cli_enqueue_change(vty, "./connection-retry", NB_OP_DESTROY,
				      NULL);

	nb_cli_apply_changes(vty, xpath);

	return CMD_SUCCESS;
}

DEFPY(no_ip_msdp_timers, no_ip_msdp_timers_cmd,
      "no ip msdp timers [(1-65535) (1-65535) [(1-65535)]]",
      NO_STR
      IP_STR
      CFG_MSDP_STR
      "MSDP timers configuration\n"
      IGNORED_IN_NO_STR
      IGNORED_IN_NO_STR
      IGNORED_IN_NO_STR)
{
	const char *vrfname;
	char xpath[XPATH_MAXLEN];

	vrfname = pim_cli_get_vrf_name(vty);
	if (vrfname == NULL)
		return CMD_WARNING_CONFIG_FAILED;

	snprintf(xpath, sizeof(xpath), FRR_PIM_MSDP_XPATH, "frr-pim:pimd",
		 "pim", vrfname, "frr-routing:ipv4");

	nb_cli_enqueue_change(vty, "./hold-time", NB_OP_DESTROY, NULL);
	nb_cli_enqueue_change(vty, "./keep-alive", NB_OP_DESTROY, NULL);
	nb_cli_enqueue_change(vty, "./connection-retry", NB_OP_DESTROY, NULL);

	nb_cli_apply_changes(vty, xpath);

	return CMD_SUCCESS;
}

DEFUN (no_ip_msdp_peer,
       no_ip_msdp_peer_cmd,
       "no ip msdp peer A.B.C.D",
       NO_STR
       IP_STR
       CFG_MSDP_STR
       "Delete MSDP peer\n"
       "peer ip address\n")
{
	const char *vrfname;
	char msdp_peer_xpath[XPATH_MAXLEN];
	char temp_xpath[XPATH_MAXLEN];

	vrfname = pim_cli_get_vrf_name(vty);
	if (vrfname == NULL)
		return CMD_WARNING_CONFIG_FAILED;

	snprintf(msdp_peer_xpath, sizeof(msdp_peer_xpath),
		 FRR_PIM_VRF_XPATH,
		 "frr-pim:pimd", "pim", vrfname, "frr-routing:ipv4");
	snprintf(temp_xpath, sizeof(temp_xpath),
		 "/msdp-peer[peer-ip='%s']",
		 argv[4]->arg);

	strlcat(msdp_peer_xpath, temp_xpath, sizeof(msdp_peer_xpath));

	nb_cli_enqueue_change(vty, msdp_peer_xpath, NB_OP_DESTROY, NULL);

	return nb_cli_apply_changes(vty, NULL);
}

DEFPY(ip_msdp_mesh_group_member,
      ip_msdp_mesh_group_member_cmd,
      "ip msdp mesh-group WORD$gname member A.B.C.D$maddr",
      IP_STR
      CFG_MSDP_STR
      "Configure MSDP mesh-group\n"
      "Mesh group name\n"
      "Mesh group member\n"
      "Peer IP address\n")
{
	const char *vrfname;
	char xpath_value[XPATH_MAXLEN];

	vrfname = pim_cli_get_vrf_name(vty);
	if (vrfname == NULL)
		return CMD_WARNING_CONFIG_FAILED;

	/* Create mesh group. */
	snprintf(xpath_value, sizeof(xpath_value),
		 FRR_PIM_VRF_XPATH "/msdp-mesh-groups[name='%s']",
		 "frr-pim:pimd", "pim", vrfname, "frr-routing:ipv4", gname);
	nb_cli_enqueue_change(vty, xpath_value, NB_OP_CREATE, NULL);

	/* Create mesh group member. */
	strlcat(xpath_value, "/members[address='", sizeof(xpath_value));
	strlcat(xpath_value, maddr_str, sizeof(xpath_value));
	strlcat(xpath_value, "']", sizeof(xpath_value));
	nb_cli_enqueue_change(vty, xpath_value, NB_OP_CREATE, NULL);

	return nb_cli_apply_changes(vty, NULL);
}

DEFPY(no_ip_msdp_mesh_group_member,
      no_ip_msdp_mesh_group_member_cmd,
      "no ip msdp mesh-group WORD$gname member A.B.C.D$maddr",
      NO_STR
      IP_STR
      CFG_MSDP_STR
      "Delete MSDP mesh-group member\n"
      "Mesh group name\n"
      "Mesh group member\n"
      "Peer IP address\n")
{
	const char *vrfname;
	char xpath_value[XPATH_MAXLEN];
	char xpath_member_value[XPATH_MAXLEN];

	vrfname = pim_cli_get_vrf_name(vty);
	if (vrfname == NULL)
		return CMD_WARNING_CONFIG_FAILED;

	/* Get mesh group base XPath. */
	snprintf(xpath_value, sizeof(xpath_value),
		 FRR_PIM_VRF_XPATH "/msdp-mesh-groups[name='%s']",
		 "frr-pim:pimd", "pim", vrfname, "frr-routing:ipv4", gname);

	if (!yang_dnode_exists(vty->candidate_config->dnode, xpath_value)) {
		vty_out(vty, "%% mesh-group does not exist\n");
		return CMD_WARNING_CONFIG_FAILED;
	}

	/* Remove mesh group member. */
	strlcpy(xpath_member_value, xpath_value, sizeof(xpath_member_value));
	strlcat(xpath_member_value, "/members[address='",
		sizeof(xpath_member_value));
	strlcat(xpath_member_value, maddr_str, sizeof(xpath_member_value));
	strlcat(xpath_member_value, "']", sizeof(xpath_member_value));
	if (!yang_dnode_exists(vty->candidate_config->dnode,
			       xpath_member_value)) {
		vty_out(vty, "%% mesh-group member does not exist\n");
		return CMD_WARNING_CONFIG_FAILED;
	}

	nb_cli_enqueue_change(vty, xpath_member_value, NB_OP_DESTROY, NULL);

	/*
	 * If this is the last member, then we must remove the group altogether
	 * to not break legacy CLI behaviour.
	 */
	pim_cli_legacy_mesh_group_behavior(vty, gname);

	return nb_cli_apply_changes(vty, NULL);
}

DEFPY(ip_msdp_mesh_group_source,
      ip_msdp_mesh_group_source_cmd,
      "ip msdp mesh-group WORD$gname source A.B.C.D$saddr",
      IP_STR
      CFG_MSDP_STR
      "Configure MSDP mesh-group\n"
      "Mesh group name\n"
      "Mesh group local address\n"
      "Source IP address for the TCP connection\n")
{
	const char *vrfname;
	char xpath_value[XPATH_MAXLEN];

	vrfname = pim_cli_get_vrf_name(vty);
	if (vrfname == NULL)
		return CMD_WARNING_CONFIG_FAILED;

	/* Create mesh group. */
	snprintf(xpath_value, sizeof(xpath_value),
		 FRR_PIM_VRF_XPATH "/msdp-mesh-groups[name='%s']",
		 "frr-pim:pimd", "pim", vrfname, "frr-routing:ipv4", gname);
	nb_cli_enqueue_change(vty, xpath_value, NB_OP_CREATE, NULL);

	/* Create mesh group source. */
	strlcat(xpath_value, "/source", sizeof(xpath_value));
	nb_cli_enqueue_change(vty, xpath_value, NB_OP_MODIFY, saddr_str);

	return nb_cli_apply_changes(vty, NULL);
}

DEFPY(no_ip_msdp_mesh_group_source,
      no_ip_msdp_mesh_group_source_cmd,
      "no ip msdp mesh-group WORD$gname source [A.B.C.D]",
      NO_STR
      IP_STR
      CFG_MSDP_STR
      "Delete MSDP mesh-group source\n"
      "Mesh group name\n"
      "Mesh group source\n"
      "Mesh group local address\n")
{
	const char *vrfname;
	char xpath_value[XPATH_MAXLEN];

	vrfname = pim_cli_get_vrf_name(vty);
	if (vrfname == NULL)
		return CMD_WARNING_CONFIG_FAILED;

	/* Get mesh group base XPath. */
	snprintf(xpath_value, sizeof(xpath_value),
		 FRR_PIM_VRF_XPATH "/msdp-mesh-groups[name='%s']",
		 "frr-pim:pimd", "pim", vrfname, "frr-routing:ipv4", gname);
	nb_cli_enqueue_change(vty, xpath_value, NB_OP_CREATE, NULL);

	/* Create mesh group source. */
	strlcat(xpath_value, "/source", sizeof(xpath_value));
	nb_cli_enqueue_change(vty, xpath_value, NB_OP_DESTROY, NULL);

	/*
	 * If this is the last member, then we must remove the group altogether
	 * to not break legacy CLI behaviour.
	 */
	pim_cli_legacy_mesh_group_behavior(vty, gname);

	return nb_cli_apply_changes(vty, NULL);
}

DEFPY(no_ip_msdp_mesh_group,
      no_ip_msdp_mesh_group_cmd,
      "no ip msdp mesh-group WORD$gname",
      NO_STR
      IP_STR
      CFG_MSDP_STR
      "Delete MSDP mesh-group\n"
      "Mesh group name")
{
	const char *vrfname;
	char xpath_value[XPATH_MAXLEN];

	vrfname = pim_cli_get_vrf_name(vty);
	if (vrfname == NULL)
		return CMD_WARNING_CONFIG_FAILED;

	/* Get mesh group base XPath. */
	snprintf(xpath_value, sizeof(xpath_value),
		 FRR_PIM_VRF_XPATH "/msdp-mesh-groups[name='%s']",
		 "frr-pim:pimd", "pim", vrfname, "frr-routing:ipv4", gname);
	if (!yang_dnode_exists(vty->candidate_config->dnode, xpath_value))
		return CMD_SUCCESS;

	nb_cli_enqueue_change(vty, xpath_value, NB_OP_DESTROY, NULL);
	return nb_cli_apply_changes(vty, NULL);
}

static void ip_msdp_show_mesh_group(struct vty *vty, struct pim_msdp_mg *mg,
				    struct json_object *json)
{
	struct listnode *mbrnode;
	struct pim_msdp_mg_mbr *mbr;
	char mbr_str[INET_ADDRSTRLEN];
	char src_str[INET_ADDRSTRLEN];
	char state_str[PIM_MSDP_STATE_STRLEN];
	enum pim_msdp_peer_state state;
	json_object *json_mg_row = NULL;
	json_object *json_members = NULL;
	json_object *json_row = NULL;

	pim_inet4_dump("<source?>", mg->src_ip, src_str, sizeof(src_str));
	if (json) {
		/* currently there is only one mesh group but we should still
		 * make
		 * it a dict with mg-name as key */
		json_mg_row = json_object_new_object();
		json_object_string_add(json_mg_row, "name",
				       mg->mesh_group_name);
		json_object_string_add(json_mg_row, "source", src_str);
	} else {
		vty_out(vty, "Mesh group : %s\n", mg->mesh_group_name);
		vty_out(vty, "  Source : %s\n", src_str);
		vty_out(vty, "  Member                 State\n");
	}

	for (ALL_LIST_ELEMENTS_RO(mg->mbr_list, mbrnode, mbr)) {
		pim_inet4_dump("<mbr?>", mbr->mbr_ip, mbr_str, sizeof(mbr_str));
		if (mbr->mp) {
			state = mbr->mp->state;
		} else {
			state = PIM_MSDP_DISABLED;
		}
		pim_msdp_state_dump(state, state_str, sizeof(state_str));
		if (json) {
			json_row = json_object_new_object();
			json_object_string_add(json_row, "member", mbr_str);
			json_object_string_add(json_row, "state", state_str);
			if (!json_members) {
				json_members = json_object_new_object();
				json_object_object_add(json_mg_row, "members",
						       json_members);
			}
			json_object_object_add(json_members, mbr_str, json_row);
		} else {
			vty_out(vty, "  %-15s  %11s\n", mbr_str, state_str);
		}
	}

	if (json)
		json_object_object_add(json, mg->mesh_group_name, json_mg_row);
}

DEFUN (show_ip_msdp_mesh_group,
       show_ip_msdp_mesh_group_cmd,
       "show ip msdp [vrf NAME] mesh-group [json]",
       SHOW_STR
       IP_STR
       MSDP_STR
       VRF_CMD_HELP_STR
       "MSDP mesh-group information\n"
       JSON_STR)
{
	bool uj = use_json(argc, argv);
	int idx = 2;
	struct pim_msdp_mg *mg;
	struct vrf *vrf = pim_cmd_lookup_vrf(vty, argv, argc, &idx);
	struct pim_instance *pim = vrf->info;
	struct json_object *json = NULL;

	if (!vrf)
		return CMD_WARNING;

	/* Quick case: list is empty. */
	if (SLIST_EMPTY(&pim->msdp.mglist)) {
		if (uj)
			vty_out(vty, "{}\n");

		return CMD_SUCCESS;
	}

	if (uj)
		json = json_object_new_object();

	SLIST_FOREACH (mg, &pim->msdp.mglist, mg_entry)
		ip_msdp_show_mesh_group(vty, mg, json);

	if (uj)
		vty_json(vty, json);

	return CMD_SUCCESS;
}

DEFUN (show_ip_msdp_mesh_group_vrf_all,
       show_ip_msdp_mesh_group_vrf_all_cmd,
       "show ip msdp vrf all mesh-group [json]",
       SHOW_STR
       IP_STR
       MSDP_STR
       VRF_CMD_HELP_STR
       "MSDP mesh-group information\n"
       JSON_STR)
{
	bool uj = use_json(argc, argv);
	struct json_object *json = NULL, *vrf_json = NULL;
	struct pim_instance *pim;
	struct pim_msdp_mg *mg;
	struct vrf *vrf;

	if (uj)
		json = json_object_new_object();

	RB_FOREACH (vrf, vrf_name_head, &vrfs_by_name) {
		if (uj) {
			vrf_json = json_object_new_object();
			json_object_object_add(json, vrf->name, vrf_json);
		} else
			vty_out(vty, "VRF: %s\n", vrf->name);

		pim = vrf->info;
		SLIST_FOREACH (mg, &pim->msdp.mglist, mg_entry)
			ip_msdp_show_mesh_group(vty, mg, vrf_json);
	}

	if (uj)
		vty_json(vty, json);


	return CMD_SUCCESS;
}

static void ip_msdp_show_peers(struct pim_instance *pim, struct vty *vty,
			       bool uj)
{
	struct listnode *mpnode;
	struct pim_msdp_peer *mp;
	char peer_str[INET_ADDRSTRLEN];
	char local_str[INET_ADDRSTRLEN];
	char state_str[PIM_MSDP_STATE_STRLEN];
	char timebuf[PIM_MSDP_UPTIME_STRLEN];
	int64_t now;
	json_object *json = NULL;
	json_object *json_row = NULL;


	if (uj) {
		json = json_object_new_object();
	} else {
		vty_out(vty,
			"Peer                       Local        State    Uptime   SaCnt\n");
	}

	for (ALL_LIST_ELEMENTS_RO(pim->msdp.peer_list, mpnode, mp)) {
		if (mp->state == PIM_MSDP_ESTABLISHED) {
			now = pim_time_monotonic_sec();
			pim_time_uptime(timebuf, sizeof(timebuf),
					now - mp->uptime);
		} else {
			strlcpy(timebuf, "-", sizeof(timebuf));
		}
		pim_inet4_dump("<peer?>", mp->peer, peer_str, sizeof(peer_str));
		pim_inet4_dump("<local?>", mp->local, local_str,
			       sizeof(local_str));
		pim_msdp_state_dump(mp->state, state_str, sizeof(state_str));
		if (uj) {
			json_row = json_object_new_object();
			json_object_string_add(json_row, "peer", peer_str);
			json_object_string_add(json_row, "local", local_str);
			json_object_string_add(json_row, "state", state_str);
			json_object_string_add(json_row, "upTime", timebuf);
			json_object_int_add(json_row, "saCount", mp->sa_cnt);
			json_object_object_add(json, peer_str, json_row);
		} else {
			vty_out(vty, "%-15s  %15s  %11s  %8s  %6d\n", peer_str,
				local_str, state_str, timebuf, mp->sa_cnt);
		}
	}

	if (uj)
		vty_json(vty, json);
}

static void ip_msdp_show_peers_detail(struct pim_instance *pim, struct vty *vty,
				      const char *peer, bool uj)
{
	struct listnode *mpnode;
	struct pim_msdp_peer *mp;
	char peer_str[INET_ADDRSTRLEN];
	char local_str[INET_ADDRSTRLEN];
	char state_str[PIM_MSDP_STATE_STRLEN];
	char timebuf[PIM_MSDP_UPTIME_STRLEN];
	char katimer[PIM_MSDP_TIMER_STRLEN];
	char crtimer[PIM_MSDP_TIMER_STRLEN];
	char holdtimer[PIM_MSDP_TIMER_STRLEN];
	int64_t now;
	json_object *json = NULL;
	json_object *json_row = NULL;

	if (uj) {
		json = json_object_new_object();
	}

	for (ALL_LIST_ELEMENTS_RO(pim->msdp.peer_list, mpnode, mp)) {
		pim_inet4_dump("<peer?>", mp->peer, peer_str, sizeof(peer_str));
		if (strcmp(peer, "detail") && strcmp(peer, peer_str))
			continue;

		if (mp->state == PIM_MSDP_ESTABLISHED) {
			now = pim_time_monotonic_sec();
			pim_time_uptime(timebuf, sizeof(timebuf),
					now - mp->uptime);
		} else {
			strlcpy(timebuf, "-", sizeof(timebuf));
		}
		pim_inet4_dump("<local?>", mp->local, local_str,
			       sizeof(local_str));
		pim_msdp_state_dump(mp->state, state_str, sizeof(state_str));
		pim_time_timer_to_hhmmss(katimer, sizeof(katimer),
					 mp->ka_timer);
		pim_time_timer_to_hhmmss(crtimer, sizeof(crtimer),
					 mp->cr_timer);
		pim_time_timer_to_hhmmss(holdtimer, sizeof(holdtimer),
					 mp->hold_timer);

		if (uj) {
			json_row = json_object_new_object();
			json_object_string_add(json_row, "peer", peer_str);
			json_object_string_add(json_row, "local", local_str);
			if (mp->flags & PIM_MSDP_PEERF_IN_GROUP)
				json_object_string_add(json_row,
						       "meshGroupName",
						       mp->mesh_group_name);
			json_object_string_add(json_row, "state", state_str);
			json_object_string_add(json_row, "upTime", timebuf);
			json_object_string_add(json_row, "keepAliveTimer",
					       katimer);
			json_object_string_add(json_row, "connRetryTimer",
					       crtimer);
			json_object_string_add(json_row, "holdTimer",
					       holdtimer);
			json_object_string_add(json_row, "lastReset",
					       mp->last_reset);
			json_object_int_add(json_row, "connAttempts",
					    mp->conn_attempts);
			json_object_int_add(json_row, "establishedChanges",
					    mp->est_flaps);
			json_object_int_add(json_row, "saCount", mp->sa_cnt);
			json_object_int_add(json_row, "kaSent", mp->ka_tx_cnt);
			json_object_int_add(json_row, "kaRcvd", mp->ka_rx_cnt);
			json_object_int_add(json_row, "saSent", mp->sa_tx_cnt);
			json_object_int_add(json_row, "saRcvd", mp->sa_rx_cnt);
			json_object_object_add(json, peer_str, json_row);
		} else {
			vty_out(vty, "Peer : %s\n", peer_str);
			vty_out(vty, "  Local               : %s\n", local_str);
			if (mp->flags & PIM_MSDP_PEERF_IN_GROUP)
				vty_out(vty, "  Mesh Group          : %s\n",
					mp->mesh_group_name);
			vty_out(vty, "  State               : %s\n", state_str);
			vty_out(vty, "  Uptime              : %s\n", timebuf);

			vty_out(vty, "  Keepalive Timer     : %s\n", katimer);
			vty_out(vty, "  Conn Retry Timer    : %s\n", crtimer);
			vty_out(vty, "  Hold Timer          : %s\n", holdtimer);
			vty_out(vty, "  Last Reset          : %s\n",
				mp->last_reset);
			vty_out(vty, "  Conn Attempts       : %d\n",
				mp->conn_attempts);
			vty_out(vty, "  Established Changes : %d\n",
				mp->est_flaps);
			vty_out(vty, "  SA Count            : %d\n",
				mp->sa_cnt);
			vty_out(vty, "  Statistics          :\n");
			vty_out(vty,
				"                       Sent       Rcvd\n");
			vty_out(vty, "    Keepalives : %10d %10d\n",
				mp->ka_tx_cnt, mp->ka_rx_cnt);
			vty_out(vty, "    SAs        : %10d %10d\n",
				mp->sa_tx_cnt, mp->sa_rx_cnt);
			vty_out(vty, "\n");
		}
	}

	if (uj)
		vty_json(vty, json);
}

DEFUN (show_ip_msdp_peer_detail,
       show_ip_msdp_peer_detail_cmd,
       "show ip msdp [vrf NAME] peer [detail|A.B.C.D] [json]",
       SHOW_STR
       IP_STR
       MSDP_STR
       VRF_CMD_HELP_STR
       "MSDP peer information\n"
       "Detailed output\n"
       "peer ip address\n"
       JSON_STR)
{
	bool uj = use_json(argc, argv);
	int idx = 2;
	struct vrf *vrf = pim_cmd_lookup_vrf(vty, argv, argc, &idx);

	if (!vrf)
		return CMD_WARNING;

	char *arg = NULL;

	if (argv_find(argv, argc, "detail", &idx))
		arg = argv[idx]->text;
	else if (argv_find(argv, argc, "A.B.C.D", &idx))
		arg = argv[idx]->arg;

	if (arg)
		ip_msdp_show_peers_detail(vrf->info, vty, argv[idx]->arg, uj);
	else
		ip_msdp_show_peers(vrf->info, vty, uj);

	return CMD_SUCCESS;
}

DEFUN (show_ip_msdp_peer_detail_vrf_all,
       show_ip_msdp_peer_detail_vrf_all_cmd,
       "show ip msdp vrf all peer [detail|A.B.C.D] [json]",
       SHOW_STR
       IP_STR
       MSDP_STR
       VRF_CMD_HELP_STR
       "MSDP peer information\n"
       "Detailed output\n"
       "peer ip address\n"
       JSON_STR)
{
	int idx = 2;
	bool uj = use_json(argc, argv);
	struct vrf *vrf;
	bool first = true;

	if (uj)
		vty_out(vty, "{ ");
	RB_FOREACH (vrf, vrf_name_head, &vrfs_by_name) {
		if (uj) {
			if (!first)
				vty_out(vty, ", ");
			vty_out(vty, " \"%s\": ", vrf->name);
			first = false;
		} else
			vty_out(vty, "VRF: %s\n", vrf->name);
		if (argv_find(argv, argc, "detail", &idx)
		    || argv_find(argv, argc, "A.B.C.D", &idx))
			ip_msdp_show_peers_detail(vrf->info, vty,
						  argv[idx]->arg, uj);
		else
			ip_msdp_show_peers(vrf->info, vty, uj);
	}
	if (uj)
		vty_out(vty, "}\n");

	return CMD_SUCCESS;
}

static void ip_msdp_show_sa(struct pim_instance *pim, struct vty *vty, bool uj)
{
	struct listnode *sanode;
	struct pim_msdp_sa *sa;
	char rp_str[INET_ADDRSTRLEN];
	char timebuf[PIM_MSDP_UPTIME_STRLEN];
	char spt_str[8];
	char local_str[8];
	int64_t now;
	json_object *json = NULL;
	json_object *json_group = NULL;
	json_object *json_row = NULL;

	if (uj) {
		json = json_object_new_object();
	} else {
		vty_out(vty,
			"Source                     Group               RP  Local  SPT    Uptime\n");
	}

	for (ALL_LIST_ELEMENTS_RO(pim->msdp.sa_list, sanode, sa)) {
		now = pim_time_monotonic_sec();
		pim_time_uptime(timebuf, sizeof(timebuf), now - sa->uptime);
		if (sa->flags & PIM_MSDP_SAF_PEER) {
			pim_inet4_dump("<rp?>", sa->rp, rp_str, sizeof(rp_str));
			if (sa->up) {
				strlcpy(spt_str, "yes", sizeof(spt_str));
			} else {
				strlcpy(spt_str, "no", sizeof(spt_str));
			}
		} else {
			strlcpy(rp_str, "-", sizeof(rp_str));
			strlcpy(spt_str, "-", sizeof(spt_str));
		}
		if (sa->flags & PIM_MSDP_SAF_LOCAL) {
			strlcpy(local_str, "yes", sizeof(local_str));
		} else {
			strlcpy(local_str, "no", sizeof(local_str));
		}
		if (uj) {
			char src_str[PIM_ADDRSTRLEN];
			char grp_str[PIM_ADDRSTRLEN];

			snprintfrr(grp_str, sizeof(grp_str), "%pPAs",
				   &sa->sg.grp);
			snprintfrr(src_str, sizeof(src_str), "%pPAs",
				   &sa->sg.src);

			json_object_object_get_ex(json, grp_str, &json_group);

			if (!json_group) {
				json_group = json_object_new_object();
				json_object_object_add(json, grp_str,
						       json_group);
			}

			json_row = json_object_new_object();
			json_object_string_add(json_row, "source", src_str);
			json_object_string_add(json_row, "group", grp_str);
			json_object_string_add(json_row, "rp", rp_str);
			json_object_string_add(json_row, "local", local_str);
			json_object_string_add(json_row, "sptSetup", spt_str);
			json_object_string_add(json_row, "upTime", timebuf);
			json_object_object_add(json_group, src_str, json_row);
		} else {
			vty_out(vty, "%-15pPAs  %15pPAs  %15s  %5c  %3c  %8s\n",
				&sa->sg.src, &sa->sg.grp, rp_str, local_str[0],
				spt_str[0], timebuf);
		}
	}

	if (uj)
		vty_json(vty, json);
}

static void ip_msdp_show_sa_entry_detail(struct pim_msdp_sa *sa,
					 const char *src_str,
					 const char *grp_str, struct vty *vty,
					 bool uj, json_object *json)
{
	char rp_str[INET_ADDRSTRLEN];
	char peer_str[INET_ADDRSTRLEN];
	char timebuf[PIM_MSDP_UPTIME_STRLEN];
	char spt_str[8];
	char local_str[8];
	char statetimer[PIM_MSDP_TIMER_STRLEN];
	int64_t now;
	json_object *json_group = NULL;
	json_object *json_row = NULL;

	now = pim_time_monotonic_sec();
	pim_time_uptime(timebuf, sizeof(timebuf), now - sa->uptime);
	if (sa->flags & PIM_MSDP_SAF_PEER) {
		pim_inet4_dump("<rp?>", sa->rp, rp_str, sizeof(rp_str));
		pim_inet4_dump("<peer?>", sa->peer, peer_str, sizeof(peer_str));
		if (sa->up) {
			strlcpy(spt_str, "yes", sizeof(spt_str));
		} else {
			strlcpy(spt_str, "no", sizeof(spt_str));
		}
	} else {
		strlcpy(rp_str, "-", sizeof(rp_str));
		strlcpy(peer_str, "-", sizeof(peer_str));
		strlcpy(spt_str, "-", sizeof(spt_str));
	}
	if (sa->flags & PIM_MSDP_SAF_LOCAL) {
		strlcpy(local_str, "yes", sizeof(local_str));
	} else {
		strlcpy(local_str, "no", sizeof(local_str));
	}
	pim_time_timer_to_hhmmss(statetimer, sizeof(statetimer),
				 sa->sa_state_timer);
	if (uj) {
		json_object_object_get_ex(json, grp_str, &json_group);

		if (!json_group) {
			json_group = json_object_new_object();
			json_object_object_add(json, grp_str, json_group);
		}

		json_row = json_object_new_object();
		json_object_string_add(json_row, "source", src_str);
		json_object_string_add(json_row, "group", grp_str);
		json_object_string_add(json_row, "rp", rp_str);
		json_object_string_add(json_row, "local", local_str);
		json_object_string_add(json_row, "sptSetup", spt_str);
		json_object_string_add(json_row, "upTime", timebuf);
		json_object_string_add(json_row, "stateTimer", statetimer);
		json_object_object_add(json_group, src_str, json_row);
	} else {
		vty_out(vty, "SA : %s\n", sa->sg_str);
		vty_out(vty, "  RP          : %s\n", rp_str);
		vty_out(vty, "  Peer        : %s\n", peer_str);
		vty_out(vty, "  Local       : %s\n", local_str);
		vty_out(vty, "  SPT Setup   : %s\n", spt_str);
		vty_out(vty, "  Uptime      : %s\n", timebuf);
		vty_out(vty, "  State Timer : %s\n", statetimer);
		vty_out(vty, "\n");
	}
}

static void ip_msdp_show_sa_detail(struct pim_instance *pim, struct vty *vty,
				   bool uj)
{
	struct listnode *sanode;
	struct pim_msdp_sa *sa;
	json_object *json = NULL;

	if (uj) {
		json = json_object_new_object();
	}

	for (ALL_LIST_ELEMENTS_RO(pim->msdp.sa_list, sanode, sa)) {
		char src_str[PIM_ADDRSTRLEN];
		char grp_str[PIM_ADDRSTRLEN];

		snprintfrr(grp_str, sizeof(grp_str), "%pPAs", &sa->sg.grp);
		snprintfrr(src_str, sizeof(src_str), "%pPAs", &sa->sg.src);

		ip_msdp_show_sa_entry_detail(sa, src_str, grp_str, vty, uj,
					     json);
	}

	if (uj)
		vty_json(vty, json);
}

DEFUN (show_ip_msdp_sa_detail,
       show_ip_msdp_sa_detail_cmd,
       "show ip msdp [vrf NAME] sa detail [json]",
       SHOW_STR
       IP_STR
       MSDP_STR
       VRF_CMD_HELP_STR
       "MSDP active-source information\n"
       "Detailed output\n"
       JSON_STR)
{
	bool uj = use_json(argc, argv);
	int idx = 2;
	struct vrf *vrf = pim_cmd_lookup_vrf(vty, argv, argc, &idx);

	if (!vrf)
		return CMD_WARNING;

	ip_msdp_show_sa_detail(vrf->info, vty, uj);

	return CMD_SUCCESS;
}

DEFUN (show_ip_msdp_sa_detail_vrf_all,
       show_ip_msdp_sa_detail_vrf_all_cmd,
       "show ip msdp vrf all sa detail [json]",
       SHOW_STR
       IP_STR
       MSDP_STR
       VRF_CMD_HELP_STR
       "MSDP active-source information\n"
       "Detailed output\n"
       JSON_STR)
{
	bool uj = use_json(argc, argv);
	struct vrf *vrf;
	bool first = true;

	if (uj)
		vty_out(vty, "{ ");
	RB_FOREACH (vrf, vrf_name_head, &vrfs_by_name) {
		if (uj) {
			if (!first)
				vty_out(vty, ", ");
			vty_out(vty, " \"%s\": ", vrf->name);
			first = false;
		} else
			vty_out(vty, "VRF: %s\n", vrf->name);
		ip_msdp_show_sa_detail(vrf->info, vty, uj);
	}
	if (uj)
		vty_out(vty, "}\n");

	return CMD_SUCCESS;
}

static void ip_msdp_show_sa_addr(struct pim_instance *pim, struct vty *vty,
				 const char *addr, bool uj)
{
	struct listnode *sanode;
	struct pim_msdp_sa *sa;
	json_object *json = NULL;

	if (uj) {
		json = json_object_new_object();
	}

	for (ALL_LIST_ELEMENTS_RO(pim->msdp.sa_list, sanode, sa)) {
		char src_str[PIM_ADDRSTRLEN];
		char grp_str[PIM_ADDRSTRLEN];

		snprintfrr(grp_str, sizeof(grp_str), "%pPAs", &sa->sg.grp);
		snprintfrr(src_str, sizeof(src_str), "%pPAs", &sa->sg.src);

		if (!strcmp(addr, src_str) || !strcmp(addr, grp_str)) {
			ip_msdp_show_sa_entry_detail(sa, src_str, grp_str, vty,
						     uj, json);
		}
	}

	if (uj)
		vty_json(vty, json);
}

static void ip_msdp_show_sa_sg(struct pim_instance *pim, struct vty *vty,
			       const char *src, const char *grp, bool uj)
{
	struct listnode *sanode;
	struct pim_msdp_sa *sa;
	json_object *json = NULL;

	if (uj) {
		json = json_object_new_object();
	}

	for (ALL_LIST_ELEMENTS_RO(pim->msdp.sa_list, sanode, sa)) {
		char src_str[PIM_ADDRSTRLEN];
		char grp_str[PIM_ADDRSTRLEN];

		snprintfrr(grp_str, sizeof(grp_str), "%pPAs", &sa->sg.grp);
		snprintfrr(src_str, sizeof(src_str), "%pPAs", &sa->sg.src);

		if (!strcmp(src, src_str) && !strcmp(grp, grp_str)) {
			ip_msdp_show_sa_entry_detail(sa, src_str, grp_str, vty,
						     uj, json);
		}
	}

	if (uj)
		vty_json(vty, json);
}

DEFUN (show_ip_msdp_sa_sg,
       show_ip_msdp_sa_sg_cmd,
       "show ip msdp [vrf NAME] sa [A.B.C.D [A.B.C.D]] [json]",
       SHOW_STR
       IP_STR
       MSDP_STR
       VRF_CMD_HELP_STR
       "MSDP active-source information\n"
       "source or group ip\n"
       "group ip\n"
       JSON_STR)
{
	bool uj = use_json(argc, argv);
	struct vrf *vrf;
	int idx = 2;

	vrf = pim_cmd_lookup_vrf(vty, argv, argc, &idx);

	if (!vrf)
		return CMD_WARNING;

	char *src_ip = argv_find(argv, argc, "A.B.C.D", &idx) ? argv[idx++]->arg
		: NULL;
	char *grp_ip = idx < argc && argv_find(argv, argc, "A.B.C.D", &idx)
		? argv[idx]->arg
		: NULL;

	if (src_ip && grp_ip)
		ip_msdp_show_sa_sg(vrf->info, vty, src_ip, grp_ip, uj);
	else if (src_ip)
		ip_msdp_show_sa_addr(vrf->info, vty, src_ip, uj);
	else
		ip_msdp_show_sa(vrf->info, vty, uj);

	return CMD_SUCCESS;
}

DEFUN (show_ip_msdp_sa_sg_vrf_all,
       show_ip_msdp_sa_sg_vrf_all_cmd,
       "show ip msdp vrf all sa [A.B.C.D [A.B.C.D]] [json]",
       SHOW_STR
       IP_STR
       MSDP_STR
       VRF_CMD_HELP_STR
       "MSDP active-source information\n"
       "source or group ip\n"
       "group ip\n"
       JSON_STR)
{
	bool uj = use_json(argc, argv);
	struct vrf *vrf;
	bool first = true;
	int idx = 2;

	char *src_ip = argv_find(argv, argc, "A.B.C.D", &idx) ? argv[idx++]->arg
		: NULL;
	char *grp_ip = idx < argc && argv_find(argv, argc, "A.B.C.D", &idx)
		? argv[idx]->arg
		: NULL;

	if (uj)
		vty_out(vty, "{ ");
	RB_FOREACH (vrf, vrf_name_head, &vrfs_by_name) {
		if (uj) {
			if (!first)
				vty_out(vty, ", ");
			vty_out(vty, " \"%s\": ", vrf->name);
			first = false;
		} else
			vty_out(vty, "VRF: %s\n", vrf->name);

		if (src_ip && grp_ip)
			ip_msdp_show_sa_sg(vrf->info, vty, src_ip, grp_ip, uj);
		else if (src_ip)
			ip_msdp_show_sa_addr(vrf->info, vty, src_ip, uj);
		else
			ip_msdp_show_sa(vrf->info, vty, uj);
	}
	if (uj)
		vty_out(vty, "}\n");

	return CMD_SUCCESS;
}
#endif /* PIM_IPV != 6 */

struct pim_sg_cache_walk_data {
	struct vty *vty;
	json_object *json;
	json_object *json_group;
	struct in_addr addr;
	bool addr_match;
};

static void pim_show_vxlan_sg_entry(struct pim_vxlan_sg *vxlan_sg,
				    struct pim_sg_cache_walk_data *cwd)
{
	struct vty *vty = cwd->vty;
	json_object *json = cwd->json;
	json_object *json_row;
	bool installed = (vxlan_sg->up) ? true : false;
	const char *iif_name = vxlan_sg->iif?vxlan_sg->iif->name:"-";
	const char *oif_name;

	if (pim_vxlan_is_orig_mroute(vxlan_sg))
		oif_name = vxlan_sg->orig_oif?vxlan_sg->orig_oif->name:"";
	else
		oif_name = vxlan_sg->term_oif?vxlan_sg->term_oif->name:"";

	if (cwd->addr_match && pim_addr_cmp(vxlan_sg->sg.src, cwd->addr) &&
	    pim_addr_cmp(vxlan_sg->sg.grp, cwd->addr)) {
		return;
	}
	if (json) {
		char src_str[PIM_ADDRSTRLEN];
		char grp_str[PIM_ADDRSTRLEN];

		snprintfrr(grp_str, sizeof(grp_str), "%pPAs",
			   &vxlan_sg->sg.grp);
		snprintfrr(src_str, sizeof(src_str), "%pPAs",
			   &vxlan_sg->sg.src);

		json_object_object_get_ex(json, grp_str, &cwd->json_group);

		if (!cwd->json_group) {
			cwd->json_group = json_object_new_object();
			json_object_object_add(json, grp_str,
					       cwd->json_group);
		}

		json_row = json_object_new_object();
		json_object_string_add(json_row, "source", src_str);
		json_object_string_add(json_row, "group", grp_str);
		json_object_string_add(json_row, "input", iif_name);
		json_object_string_add(json_row, "output", oif_name);
		if (installed)
			json_object_boolean_true_add(json_row, "installed");
		else
			json_object_boolean_false_add(json_row, "installed");
		json_object_object_add(cwd->json_group, src_str, json_row);
	} else {
		vty_out(vty, "%-15pPAs %-15pPAs %-15s %-15s %-5s\n",
			&vxlan_sg->sg.src, &vxlan_sg->sg.grp, iif_name,
			oif_name, installed ? "I" : "");
	}
}

static void pim_show_vxlan_sg_hash_entry(struct hash_bucket *bucket, void *arg)
{
	pim_show_vxlan_sg_entry((struct pim_vxlan_sg *)bucket->data,
				(struct pim_sg_cache_walk_data *)arg);
}

static void pim_show_vxlan_sg(struct pim_instance *pim,
			      struct vty *vty, bool uj)
{
	json_object *json = NULL;
	struct pim_sg_cache_walk_data cwd;

	if (uj) {
		json = json_object_new_object();
	} else {
		vty_out(vty, "Codes: I -> installed\n");
		vty_out(vty,
			"Source          Group           Input           Output          Flags\n");
	}

	memset(&cwd, 0, sizeof(cwd));
	cwd.vty = vty;
	cwd.json = json;
	hash_iterate(pim->vxlan.sg_hash, pim_show_vxlan_sg_hash_entry, &cwd);

	if (uj)
		vty_json(vty, json);
}

static void pim_show_vxlan_sg_match_addr(struct pim_instance *pim,
					 struct vty *vty, char *addr_str,
					 bool uj)
{
	json_object *json = NULL;
	struct pim_sg_cache_walk_data cwd;
	int result = 0;

	memset(&cwd, 0, sizeof(cwd));
	result = inet_pton(AF_INET, addr_str, &cwd.addr);
	if (result <= 0) {
		vty_out(vty, "Bad address %s: errno=%d: %s\n", addr_str,
			errno, safe_strerror(errno));
		return;
	}

	if (uj) {
		json = json_object_new_object();
	} else {
		vty_out(vty, "Codes: I -> installed\n");
		vty_out(vty,
			"Source          Group           Input           Output          Flags\n");
	}

	cwd.vty = vty;
	cwd.json = json;
	cwd.addr_match = true;
	hash_iterate(pim->vxlan.sg_hash, pim_show_vxlan_sg_hash_entry, &cwd);

	if (uj)
		vty_json(vty, json);
}

static void pim_show_vxlan_sg_one(struct pim_instance *pim,
				  struct vty *vty, char *src_str, char *grp_str,
				  bool uj)
{
	json_object *json = NULL;
	pim_sgaddr sg;
	int result = 0;
	struct pim_vxlan_sg *vxlan_sg;
	const char *iif_name;
	bool installed;
	const char *oif_name;

	result = inet_pton(AF_INET, src_str, &sg.src);
	if (result <= 0) {
		vty_out(vty, "Bad src address %s: errno=%d: %s\n", src_str,
			errno, safe_strerror(errno));
		return;
	}
	result = inet_pton(AF_INET, grp_str, &sg.grp);
	if (result <= 0) {
		vty_out(vty, "Bad grp address %s: errno=%d: %s\n", grp_str,
			errno, safe_strerror(errno));
		return;
	}

	if (uj)
		json = json_object_new_object();

	vxlan_sg = pim_vxlan_sg_find(pim, &sg);
	if (vxlan_sg) {
		installed = (vxlan_sg->up) ? true : false;
		iif_name = vxlan_sg->iif?vxlan_sg->iif->name:"-";

		if (pim_vxlan_is_orig_mroute(vxlan_sg))
			oif_name =
				vxlan_sg->orig_oif?vxlan_sg->orig_oif->name:"";
		else
			oif_name =
				vxlan_sg->term_oif?vxlan_sg->term_oif->name:"";

		if (uj) {
			json_object_string_add(json, "source", src_str);
			json_object_string_add(json, "group", grp_str);
			json_object_string_add(json, "input", iif_name);
			json_object_string_add(json, "output", oif_name);
			if (installed)
				json_object_boolean_true_add(json, "installed");
			else
				json_object_boolean_false_add(json,
							      "installed");
		} else {
			vty_out(vty, "SG : %s\n", vxlan_sg->sg_str);
			vty_out(vty, "  Input     : %s\n", iif_name);
			vty_out(vty, "  Output    : %s\n", oif_name);
			vty_out(vty, "  installed : %s\n",
				installed?"yes":"no");
		}
	}

	if (uj)
		vty_json(vty, json);
}

DEFUN (show_ip_pim_vxlan_sg,
       show_ip_pim_vxlan_sg_cmd,
       "show ip pim [vrf NAME] vxlan-groups [A.B.C.D [A.B.C.D]] [json]",
       SHOW_STR
       IP_STR
       PIM_STR
       VRF_CMD_HELP_STR
       "VxLAN BUM groups\n"
       "source or group ip\n"
       "group ip\n"
       JSON_STR)
{
	bool uj = use_json(argc, argv);
	struct vrf *vrf;
	int idx = 2;

	vrf = pim_cmd_lookup_vrf(vty, argv, argc, &idx);

	if (!vrf)
		return CMD_WARNING;

	char *src_ip = argv_find(argv, argc, "A.B.C.D", &idx) ?
		argv[idx++]->arg:NULL;
	char *grp_ip = idx < argc && argv_find(argv, argc, "A.B.C.D", &idx) ?
		argv[idx]->arg:NULL;

	if (src_ip && grp_ip)
		pim_show_vxlan_sg_one(vrf->info, vty, src_ip, grp_ip, uj);
	else if (src_ip)
		pim_show_vxlan_sg_match_addr(vrf->info, vty, src_ip, uj);
	else
		pim_show_vxlan_sg(vrf->info, vty, uj);

	return CMD_SUCCESS;
}

static void pim_show_vxlan_sg_work(struct pim_instance *pim,
				   struct vty *vty, bool uj)
{
	json_object *json = NULL;
	struct pim_sg_cache_walk_data cwd;
	struct listnode *node;
	struct pim_vxlan_sg *vxlan_sg;

	if (uj) {
		json = json_object_new_object();
	} else {
		vty_out(vty, "Codes: I -> installed\n");
		vty_out(vty,
			"Source          Group           Input           Flags\n");
	}

	memset(&cwd, 0, sizeof(cwd));
	cwd.vty = vty;
	cwd.json = json;
	for (ALL_LIST_ELEMENTS_RO(pim_vxlan_p->work_list, node, vxlan_sg))
		pim_show_vxlan_sg_entry(vxlan_sg, &cwd);

	if (uj)
		vty_json(vty, json);
}

DEFUN_HIDDEN (show_ip_pim_vxlan_sg_work,
              show_ip_pim_vxlan_sg_work_cmd,
              "show ip pim [vrf NAME] vxlan-work [json]",
              SHOW_STR
              IP_STR
              PIM_STR
              VRF_CMD_HELP_STR
              "VxLAN work list\n"
              JSON_STR)
{
	bool uj = use_json(argc, argv);
	struct vrf *vrf;
	int idx = 2;

	vrf = pim_cmd_lookup_vrf(vty, argv, argc, &idx);

	if (!vrf)
		return CMD_WARNING;

	pim_show_vxlan_sg_work(vrf->info, vty, uj);

	return CMD_SUCCESS;
}

DEFUN_HIDDEN (no_ip_pim_mlag,
	      no_ip_pim_mlag_cmd,
	      "no ip pim mlag",
	      NO_STR
	      IP_STR
	      PIM_STR
	      "MLAG\n")
{
	char mlag_xpath[XPATH_MAXLEN];

	snprintf(mlag_xpath, sizeof(mlag_xpath), FRR_PIM_VRF_XPATH,
		 "frr-pim:pimd", "pim", "default", "frr-routing:ipv4");
	strlcat(mlag_xpath, "/mlag", sizeof(mlag_xpath));

	nb_cli_enqueue_change(vty, mlag_xpath, NB_OP_DESTROY, NULL);


	return nb_cli_apply_changes(vty, NULL);
}

DEFUN_HIDDEN (ip_pim_mlag,
	      ip_pim_mlag_cmd,
	      "ip pim mlag INTERFACE role [primary|secondary] state [up|down] addr A.B.C.D",
	      IP_STR
	      PIM_STR
	      "MLAG\n"
	      "peerlink sub interface\n"
	      "MLAG role\n"
	      "MLAG role primary\n"
	      "MLAG role secondary\n"
	      "peer session state\n"
	      "peer session state up\n"
	      "peer session state down\n"
	      "configure PIP\n"
	      "unique ip address\n")
{
	int idx;
	char mlag_peerlink_rif_xpath[XPATH_MAXLEN];
	char mlag_my_role_xpath[XPATH_MAXLEN];
	char mlag_peer_state_xpath[XPATH_MAXLEN];
	char mlag_reg_address_xpath[XPATH_MAXLEN];

	snprintf(mlag_peerlink_rif_xpath, sizeof(mlag_peerlink_rif_xpath),
		 FRR_PIM_VRF_XPATH,
		 "frr-pim:pimd", "pim", "default", "frr-routing:ipv4");
	strlcat(mlag_peerlink_rif_xpath, "/mlag/peerlink-rif",
		sizeof(mlag_peerlink_rif_xpath));

	idx = 3;
	nb_cli_enqueue_change(vty, mlag_peerlink_rif_xpath, NB_OP_MODIFY,
			      argv[idx]->arg);

	snprintf(mlag_my_role_xpath, sizeof(mlag_my_role_xpath),
		 FRR_PIM_VRF_XPATH,
		 "frr-pim:pimd", "pim", "default", "frr-routing:ipv4");
	strlcat(mlag_my_role_xpath, "/mlag/my-role",
		sizeof(mlag_my_role_xpath));

	idx += 2;
	if (!strcmp(argv[idx]->arg, "primary")) {
		nb_cli_enqueue_change(vty, mlag_my_role_xpath, NB_OP_MODIFY,
				      "MLAG_ROLE_PRIMARY");

	} else if (!strcmp(argv[idx]->arg, "secondary")) {
		nb_cli_enqueue_change(vty, mlag_my_role_xpath, NB_OP_MODIFY,
				      "MLAG_ROLE_SECONDARY");

	} else {
		vty_out(vty, "unknown MLAG role %s\n", argv[idx]->arg);
		return CMD_WARNING;
	}

	snprintf(mlag_peer_state_xpath, sizeof(mlag_peer_state_xpath),
		 FRR_PIM_VRF_XPATH,
		 "frr-pim:pimd", "pim", "default", "frr-routing:ipv4");
	strlcat(mlag_peer_state_xpath, "/mlag/peer-state",
		sizeof(mlag_peer_state_xpath));

	idx += 2;
	if (!strcmp(argv[idx]->arg, "up")) {
		nb_cli_enqueue_change(vty, mlag_peer_state_xpath, NB_OP_MODIFY,
				      "true");

	} else if (strcmp(argv[idx]->arg, "down")) {
		nb_cli_enqueue_change(vty, mlag_peer_state_xpath, NB_OP_MODIFY,
				      "false");

	} else {
		vty_out(vty, "unknown MLAG state %s\n", argv[idx]->arg);
		return CMD_WARNING;
	}

	snprintf(mlag_reg_address_xpath, sizeof(mlag_reg_address_xpath),
		 FRR_PIM_VRF_XPATH,
		 "frr-pim:pimd", "pim", "default", "frr-routing:ipv4");
	strlcat(mlag_reg_address_xpath, "/mlag/reg-address",
		sizeof(mlag_reg_address_xpath));

	idx += 2;
	nb_cli_enqueue_change(vty, mlag_reg_address_xpath, NB_OP_MODIFY,
			      argv[idx]->arg);

	return nb_cli_apply_changes(vty, NULL);
}

void pim_cmd_init(void)
{
	if_cmd_init(pim_interface_config_write);

	install_node(&debug_node);

	install_element(ENABLE_NODE, &pim_test_sg_keepalive_cmd);

	install_element(CONFIG_NODE, &ip_pim_rp_cmd);
	install_element(VRF_NODE, &ip_pim_rp_cmd);
	install_element(CONFIG_NODE, &no_ip_pim_rp_cmd);
	install_element(VRF_NODE, &no_ip_pim_rp_cmd);
	install_element(CONFIG_NODE, &ip_pim_rp_prefix_list_cmd);
	install_element(VRF_NODE, &ip_pim_rp_prefix_list_cmd);
	install_element(CONFIG_NODE, &no_ip_pim_rp_prefix_list_cmd);
	install_element(VRF_NODE, &no_ip_pim_rp_prefix_list_cmd);
	install_element(CONFIG_NODE, &no_ip_pim_ssm_prefix_list_cmd);
	install_element(VRF_NODE, &no_ip_pim_ssm_prefix_list_cmd);
	install_element(CONFIG_NODE, &no_ip_pim_ssm_prefix_list_name_cmd);
	install_element(VRF_NODE, &no_ip_pim_ssm_prefix_list_name_cmd);
	install_element(CONFIG_NODE, &ip_pim_ssm_prefix_list_cmd);
	install_element(VRF_NODE, &ip_pim_ssm_prefix_list_cmd);
	install_element(CONFIG_NODE, &ip_pim_register_suppress_cmd);
	install_element(CONFIG_NODE, &no_ip_pim_register_suppress_cmd);
	install_element(CONFIG_NODE, &ip_pim_spt_switchover_infinity_cmd);
	install_element(VRF_NODE, &ip_pim_spt_switchover_infinity_cmd);
	install_element(CONFIG_NODE, &ip_pim_spt_switchover_infinity_plist_cmd);
	install_element(VRF_NODE, &ip_pim_spt_switchover_infinity_plist_cmd);
	install_element(CONFIG_NODE, &no_ip_pim_spt_switchover_infinity_cmd);
	install_element(VRF_NODE, &no_ip_pim_spt_switchover_infinity_cmd);
	install_element(CONFIG_NODE,
			&no_ip_pim_spt_switchover_infinity_plist_cmd);
	install_element(VRF_NODE, &no_ip_pim_spt_switchover_infinity_plist_cmd);
	install_element(CONFIG_NODE, &pim_register_accept_list_cmd);
	install_element(VRF_NODE, &pim_register_accept_list_cmd);
	install_element(CONFIG_NODE, &ip_pim_joinprune_time_cmd);
	install_element(CONFIG_NODE, &no_ip_pim_joinprune_time_cmd);
	install_element(CONFIG_NODE, &ip_pim_keep_alive_cmd);
	install_element(VRF_NODE, &ip_pim_keep_alive_cmd);
	install_element(CONFIG_NODE, &ip_pim_rp_keep_alive_cmd);
	install_element(VRF_NODE, &ip_pim_rp_keep_alive_cmd);
	install_element(CONFIG_NODE, &no_ip_pim_keep_alive_cmd);
	install_element(VRF_NODE, &no_ip_pim_keep_alive_cmd);
	install_element(CONFIG_NODE, &no_ip_pim_rp_keep_alive_cmd);
	install_element(VRF_NODE, &no_ip_pim_rp_keep_alive_cmd);
	install_element(CONFIG_NODE, &ip_pim_packets_cmd);
	install_element(CONFIG_NODE, &no_ip_pim_packets_cmd);
	install_element(CONFIG_NODE, &ip_pim_v6_secondary_cmd);
	install_element(VRF_NODE, &ip_pim_v6_secondary_cmd);
	install_element(CONFIG_NODE, &no_ip_pim_v6_secondary_cmd);
	install_element(VRF_NODE, &no_ip_pim_v6_secondary_cmd);
	install_element(CONFIG_NODE, &ip_ssmpingd_cmd);
	install_element(VRF_NODE, &ip_ssmpingd_cmd);
	install_element(CONFIG_NODE, &no_ip_ssmpingd_cmd);
	install_element(VRF_NODE, &no_ip_ssmpingd_cmd);
#if PIM_IPV != 6
	install_element(CONFIG_NODE, &ip_msdp_peer_cmd);
	install_element(VRF_NODE, &ip_msdp_peer_cmd);
	install_element(CONFIG_NODE, &no_ip_msdp_peer_cmd);
	install_element(VRF_NODE, &no_ip_msdp_peer_cmd);
#endif /* PIM_IPV != 6 */
	install_element(CONFIG_NODE, &ip_pim_ecmp_cmd);
	install_element(VRF_NODE, &ip_pim_ecmp_cmd);
	install_element(CONFIG_NODE, &no_ip_pim_ecmp_cmd);
	install_element(VRF_NODE, &no_ip_pim_ecmp_cmd);
	install_element(CONFIG_NODE, &ip_pim_ecmp_rebalance_cmd);
	install_element(VRF_NODE, &ip_pim_ecmp_rebalance_cmd);
	install_element(CONFIG_NODE, &no_ip_pim_ecmp_rebalance_cmd);
	install_element(VRF_NODE, &no_ip_pim_ecmp_rebalance_cmd);
	install_element(CONFIG_NODE, &ip_pim_mlag_cmd);
	install_element(CONFIG_NODE, &no_ip_pim_mlag_cmd);
	install_element(CONFIG_NODE, &igmp_group_watermark_cmd);
	install_element(VRF_NODE, &igmp_group_watermark_cmd);
	install_element(CONFIG_NODE, &no_igmp_group_watermark_cmd);
	install_element(VRF_NODE, &no_igmp_group_watermark_cmd);

	install_element(INTERFACE_NODE, &interface_ip_igmp_cmd);
	install_element(INTERFACE_NODE, &interface_no_ip_igmp_cmd);
	install_element(INTERFACE_NODE, &interface_ip_igmp_join_cmd);
	install_element(INTERFACE_NODE, &interface_no_ip_igmp_join_cmd);
	install_element(INTERFACE_NODE, &interface_ip_igmp_version_cmd);
	install_element(INTERFACE_NODE, &interface_no_ip_igmp_version_cmd);
	install_element(INTERFACE_NODE, &interface_ip_igmp_query_interval_cmd);
	install_element(INTERFACE_NODE,
			&interface_no_ip_igmp_query_interval_cmd);
	install_element(INTERFACE_NODE,
			&interface_ip_igmp_query_max_response_time_cmd);
	install_element(INTERFACE_NODE,
			&interface_no_ip_igmp_query_max_response_time_cmd);
	install_element(INTERFACE_NODE,
			&interface_ip_igmp_query_max_response_time_dsec_cmd);
	install_element(INTERFACE_NODE,
			&interface_no_ip_igmp_query_max_response_time_dsec_cmd);
	install_element(INTERFACE_NODE,
			&interface_ip_igmp_last_member_query_count_cmd);
	install_element(INTERFACE_NODE,
			&interface_no_ip_igmp_last_member_query_count_cmd);
	install_element(INTERFACE_NODE,
			&interface_ip_igmp_last_member_query_interval_cmd);
	install_element(INTERFACE_NODE,
			&interface_no_ip_igmp_last_member_query_interval_cmd);
	install_element(INTERFACE_NODE, &interface_ip_pim_activeactive_cmd);
	install_element(INTERFACE_NODE, &interface_ip_pim_ssm_cmd);
	install_element(INTERFACE_NODE, &interface_no_ip_pim_ssm_cmd);
	install_element(INTERFACE_NODE, &interface_ip_pim_sm_cmd);
	install_element(INTERFACE_NODE, &interface_no_ip_pim_sm_cmd);
	install_element(INTERFACE_NODE, &interface_ip_pim_cmd);
	install_element(INTERFACE_NODE, &interface_no_ip_pim_cmd);
	install_element(INTERFACE_NODE, &interface_ip_pim_drprio_cmd);
	install_element(INTERFACE_NODE, &interface_no_ip_pim_drprio_cmd);
	install_element(INTERFACE_NODE, &interface_ip_pim_hello_cmd);
	install_element(INTERFACE_NODE, &interface_no_ip_pim_hello_cmd);
	install_element(INTERFACE_NODE, &interface_ip_pim_boundary_oil_cmd);
	install_element(INTERFACE_NODE, &interface_no_ip_pim_boundary_oil_cmd);
	install_element(INTERFACE_NODE, &interface_ip_igmp_query_generate_cmd);

	// Static mroutes NEB
	install_element(INTERFACE_NODE, &interface_ip_mroute_cmd);
	install_element(INTERFACE_NODE, &interface_no_ip_mroute_cmd);

	install_element(VIEW_NODE, &show_ip_igmp_interface_cmd);
	install_element(VIEW_NODE, &show_ip_igmp_interface_vrf_all_cmd);
	install_element(VIEW_NODE, &show_ip_igmp_join_cmd);
	install_element(VIEW_NODE, &show_ip_igmp_join_vrf_all_cmd);
	install_element(VIEW_NODE, &show_ip_igmp_groups_cmd);
	install_element(VIEW_NODE, &show_ip_igmp_groups_vrf_all_cmd);
	install_element(VIEW_NODE, &show_ip_igmp_groups_retransmissions_cmd);
	install_element(VIEW_NODE, &show_ip_igmp_sources_cmd);
	install_element(VIEW_NODE, &show_ip_igmp_sources_retransmissions_cmd);
	install_element(VIEW_NODE, &show_ip_igmp_statistics_cmd);
	install_element(VIEW_NODE, &show_ip_pim_assert_cmd);
	install_element(VIEW_NODE, &show_ip_pim_assert_internal_cmd);
	install_element(VIEW_NODE, &show_ip_pim_assert_metric_cmd);
	install_element(VIEW_NODE, &show_ip_pim_assert_winner_metric_cmd);
	install_element(VIEW_NODE, &show_ip_pim_interface_traffic_cmd);
	install_element(VIEW_NODE, &show_ip_pim_interface_cmd);
	install_element(VIEW_NODE, &show_ip_pim_interface_vrf_all_cmd);
	install_element(VIEW_NODE, &show_ip_pim_join_cmd);
	install_element(VIEW_NODE, &show_ip_pim_join_vrf_all_cmd);
	install_element(VIEW_NODE, &show_ip_pim_jp_agg_cmd);
	install_element(VIEW_NODE, &show_ip_pim_local_membership_cmd);
	install_element(VIEW_NODE, &show_ip_pim_mlag_summary_cmd);
	install_element(VIEW_NODE, &show_ip_pim_mlag_up_cmd);
	install_element(VIEW_NODE, &show_ip_pim_mlag_up_vrf_all_cmd);
	install_element(VIEW_NODE, &show_ip_pim_neighbor_cmd);
	install_element(VIEW_NODE, &show_ip_pim_neighbor_vrf_all_cmd);
	install_element(VIEW_NODE, &show_ip_pim_rpf_cmd);
	install_element(VIEW_NODE, &show_ip_pim_rpf_vrf_all_cmd);
	install_element(VIEW_NODE, &show_ip_pim_secondary_cmd);
	install_element(VIEW_NODE, &show_ip_pim_state_cmd);
	install_element(VIEW_NODE, &show_ip_pim_state_vrf_all_cmd);
	install_element(VIEW_NODE, &show_ip_pim_upstream_cmd);
	install_element(VIEW_NODE, &show_ip_pim_upstream_vrf_all_cmd);
	install_element(VIEW_NODE, &show_ip_pim_channel_cmd);
	install_element(VIEW_NODE, &show_ip_pim_upstream_join_desired_cmd);
	install_element(VIEW_NODE, &show_ip_pim_upstream_rpf_cmd);
	install_element(VIEW_NODE, &show_ip_pim_rp_cmd);
	install_element(VIEW_NODE, &show_ip_pim_rp_vrf_all_cmd);
	install_element(VIEW_NODE, &show_ip_pim_bsr_cmd);
	install_element(VIEW_NODE, &show_ip_multicast_cmd);
	install_element(VIEW_NODE, &show_ip_multicast_vrf_all_cmd);
	install_element(VIEW_NODE, &show_ip_multicast_count_cmd);
	install_element(VIEW_NODE, &show_ip_multicast_count_vrf_all_cmd);
	install_element(VIEW_NODE, &show_ip_mroute_cmd);
	install_element(VIEW_NODE, &show_ip_mroute_vrf_all_cmd);
	install_element(VIEW_NODE, &show_ip_mroute_count_cmd);
	install_element(VIEW_NODE, &show_ip_mroute_count_vrf_all_cmd);
	install_element(VIEW_NODE, &show_ip_mroute_summary_cmd);
	install_element(VIEW_NODE, &show_ip_mroute_summary_vrf_all_cmd);
	install_element(VIEW_NODE, &show_ip_rib_cmd);
	install_element(VIEW_NODE, &show_ip_ssmpingd_cmd);
	install_element(VIEW_NODE, &show_ip_pim_nexthop_cmd);
	install_element(VIEW_NODE, &show_ip_pim_nexthop_lookup_cmd);
	install_element(VIEW_NODE, &show_ip_pim_bsrp_cmd);
	install_element(VIEW_NODE, &show_ip_pim_bsm_db_cmd);
	install_element(VIEW_NODE, &show_ip_pim_statistics_cmd);

	install_element(ENABLE_NODE, &clear_ip_mroute_count_cmd);
	install_element(ENABLE_NODE, &clear_ip_interfaces_cmd);
	install_element(ENABLE_NODE, &clear_ip_igmp_interfaces_cmd);
	install_element(ENABLE_NODE, &clear_ip_mroute_cmd);
	install_element(ENABLE_NODE, &clear_ip_pim_interfaces_cmd);
	install_element(ENABLE_NODE, &clear_ip_pim_interface_traffic_cmd);
	install_element(ENABLE_NODE, &clear_ip_pim_oil_cmd);
	install_element(ENABLE_NODE, &clear_ip_pim_statistics_cmd);
	install_element(ENABLE_NODE, &clear_ip_pim_bsr_db_cmd);

	install_element(ENABLE_NODE, &show_debugging_pim_cmd);

	install_element(ENABLE_NODE, &debug_igmp_cmd);
	install_element(ENABLE_NODE, &no_debug_igmp_cmd);
	install_element(ENABLE_NODE, &debug_igmp_events_cmd);
	install_element(ENABLE_NODE, &no_debug_igmp_events_cmd);
	install_element(ENABLE_NODE, &debug_igmp_packets_cmd);
	install_element(ENABLE_NODE, &no_debug_igmp_packets_cmd);
	install_element(ENABLE_NODE, &debug_igmp_trace_cmd);
	install_element(ENABLE_NODE, &no_debug_igmp_trace_cmd);
	install_element(ENABLE_NODE, &debug_mroute_cmd);
	install_element(ENABLE_NODE, &debug_mroute_detail_cmd);
	install_element(ENABLE_NODE, &no_debug_mroute_cmd);
	install_element(ENABLE_NODE, &no_debug_mroute_detail_cmd);
	install_element(ENABLE_NODE, &debug_pim_static_cmd);
	install_element(ENABLE_NODE, &no_debug_pim_static_cmd);
	install_element(ENABLE_NODE, &debug_pim_cmd);
	install_element(ENABLE_NODE, &no_debug_pim_cmd);
	install_element(ENABLE_NODE, &debug_pim_nht_cmd);
	install_element(ENABLE_NODE, &no_debug_pim_nht_cmd);
	install_element(ENABLE_NODE, &debug_pim_nht_rp_cmd);
	install_element(ENABLE_NODE, &no_debug_pim_nht_rp_cmd);
	install_element(ENABLE_NODE, &debug_pim_events_cmd);
	install_element(ENABLE_NODE, &no_debug_pim_events_cmd);
	install_element(ENABLE_NODE, &debug_pim_packets_cmd);
	install_element(ENABLE_NODE, &no_debug_pim_packets_cmd);
	install_element(ENABLE_NODE, &debug_pim_packetdump_send_cmd);
	install_element(ENABLE_NODE, &no_debug_pim_packetdump_send_cmd);
	install_element(ENABLE_NODE, &debug_pim_packetdump_recv_cmd);
	install_element(ENABLE_NODE, &no_debug_pim_packetdump_recv_cmd);
	install_element(ENABLE_NODE, &debug_pim_trace_cmd);
	install_element(ENABLE_NODE, &no_debug_pim_trace_cmd);
	install_element(ENABLE_NODE, &debug_pim_trace_detail_cmd);
	install_element(ENABLE_NODE, &no_debug_pim_trace_detail_cmd);
	install_element(ENABLE_NODE, &debug_ssmpingd_cmd);
	install_element(ENABLE_NODE, &no_debug_ssmpingd_cmd);
	install_element(ENABLE_NODE, &debug_pim_zebra_cmd);
	install_element(ENABLE_NODE, &no_debug_pim_zebra_cmd);
	install_element(ENABLE_NODE, &debug_pim_mlag_cmd);
	install_element(ENABLE_NODE, &no_debug_pim_mlag_cmd);
	install_element(ENABLE_NODE, &debug_pim_vxlan_cmd);
	install_element(ENABLE_NODE, &no_debug_pim_vxlan_cmd);
#if PIM_IPV != 6
	install_element(ENABLE_NODE, &debug_msdp_cmd);
	install_element(ENABLE_NODE, &no_debug_msdp_cmd);
	install_element(ENABLE_NODE, &debug_msdp_events_cmd);
	install_element(ENABLE_NODE, &no_debug_msdp_events_cmd);
	install_element(ENABLE_NODE, &debug_msdp_packets_cmd);
	install_element(ENABLE_NODE, &no_debug_msdp_packets_cmd);
#endif /* PIM_IPV != 6 */
	install_element(ENABLE_NODE, &debug_mtrace_cmd);
	install_element(ENABLE_NODE, &no_debug_mtrace_cmd);
	install_element(ENABLE_NODE, &debug_bsm_cmd);
	install_element(ENABLE_NODE, &no_debug_bsm_cmd);

	install_element(CONFIG_NODE, &debug_igmp_cmd);
	install_element(CONFIG_NODE, &no_debug_igmp_cmd);
	install_element(CONFIG_NODE, &debug_igmp_events_cmd);
	install_element(CONFIG_NODE, &no_debug_igmp_events_cmd);
	install_element(CONFIG_NODE, &debug_igmp_packets_cmd);
	install_element(CONFIG_NODE, &no_debug_igmp_packets_cmd);
	install_element(CONFIG_NODE, &debug_igmp_trace_cmd);
	install_element(CONFIG_NODE, &no_debug_igmp_trace_cmd);
	install_element(CONFIG_NODE, &debug_mroute_cmd);
	install_element(CONFIG_NODE, &debug_mroute_detail_cmd);
	install_element(CONFIG_NODE, &no_debug_mroute_cmd);
	install_element(CONFIG_NODE, &no_debug_mroute_detail_cmd);
	install_element(CONFIG_NODE, &debug_pim_static_cmd);
	install_element(CONFIG_NODE, &no_debug_pim_static_cmd);
	install_element(CONFIG_NODE, &debug_pim_cmd);
	install_element(CONFIG_NODE, &no_debug_pim_cmd);
	install_element(CONFIG_NODE, &debug_pim_nht_cmd);
	install_element(CONFIG_NODE, &no_debug_pim_nht_cmd);
	install_element(CONFIG_NODE, &debug_pim_nht_rp_cmd);
	install_element(CONFIG_NODE, &no_debug_pim_nht_rp_cmd);
	install_element(CONFIG_NODE, &debug_pim_events_cmd);
	install_element(CONFIG_NODE, &no_debug_pim_events_cmd);
	install_element(CONFIG_NODE, &debug_pim_packets_cmd);
	install_element(CONFIG_NODE, &no_debug_pim_packets_cmd);
	install_element(CONFIG_NODE, &debug_pim_packetdump_send_cmd);
	install_element(CONFIG_NODE, &no_debug_pim_packetdump_send_cmd);
	install_element(CONFIG_NODE, &debug_pim_packetdump_recv_cmd);
	install_element(CONFIG_NODE, &no_debug_pim_packetdump_recv_cmd);
	install_element(CONFIG_NODE, &debug_pim_trace_cmd);
	install_element(CONFIG_NODE, &no_debug_pim_trace_cmd);
	install_element(CONFIG_NODE, &debug_pim_trace_detail_cmd);
	install_element(CONFIG_NODE, &no_debug_pim_trace_detail_cmd);
	install_element(CONFIG_NODE, &debug_ssmpingd_cmd);
	install_element(CONFIG_NODE, &no_debug_ssmpingd_cmd);
	install_element(CONFIG_NODE, &debug_pim_zebra_cmd);
	install_element(CONFIG_NODE, &no_debug_pim_zebra_cmd);
	install_element(CONFIG_NODE, &debug_pim_mlag_cmd);
	install_element(CONFIG_NODE, &no_debug_pim_mlag_cmd);
	install_element(CONFIG_NODE, &debug_pim_vxlan_cmd);
	install_element(CONFIG_NODE, &no_debug_pim_vxlan_cmd);
#if PIM_IPV != 6
	install_element(CONFIG_NODE, &debug_msdp_cmd);
	install_element(CONFIG_NODE, &no_debug_msdp_cmd);
	install_element(CONFIG_NODE, &debug_msdp_events_cmd);
	install_element(CONFIG_NODE, &no_debug_msdp_events_cmd);
	install_element(CONFIG_NODE, &debug_msdp_packets_cmd);
	install_element(CONFIG_NODE, &no_debug_msdp_packets_cmd);
#endif /* PIM_IPV != 6 */
	install_element(CONFIG_NODE, &debug_mtrace_cmd);
	install_element(CONFIG_NODE, &no_debug_mtrace_cmd);
	install_element(CONFIG_NODE, &debug_bsm_cmd);
	install_element(CONFIG_NODE, &no_debug_bsm_cmd);

#if PIM_IPV != 6
	install_element(CONFIG_NODE, &ip_msdp_timers_cmd);
	install_element(VRF_NODE, &ip_msdp_timers_cmd);
	install_element(CONFIG_NODE, &no_ip_msdp_timers_cmd);
	install_element(VRF_NODE, &no_ip_msdp_timers_cmd);
	install_element(CONFIG_NODE, &ip_msdp_mesh_group_member_cmd);
	install_element(VRF_NODE, &ip_msdp_mesh_group_member_cmd);
	install_element(CONFIG_NODE, &no_ip_msdp_mesh_group_member_cmd);
	install_element(VRF_NODE, &no_ip_msdp_mesh_group_member_cmd);
	install_element(CONFIG_NODE, &ip_msdp_mesh_group_source_cmd);
	install_element(VRF_NODE, &ip_msdp_mesh_group_source_cmd);
	install_element(CONFIG_NODE, &no_ip_msdp_mesh_group_source_cmd);
	install_element(VRF_NODE, &no_ip_msdp_mesh_group_source_cmd);
	install_element(CONFIG_NODE, &no_ip_msdp_mesh_group_cmd);
	install_element(VRF_NODE, &no_ip_msdp_mesh_group_cmd);
	install_element(VIEW_NODE, &show_ip_msdp_peer_detail_cmd);
	install_element(VIEW_NODE, &show_ip_msdp_peer_detail_vrf_all_cmd);
	install_element(VIEW_NODE, &show_ip_msdp_sa_detail_cmd);
	install_element(VIEW_NODE, &show_ip_msdp_sa_detail_vrf_all_cmd);
	install_element(VIEW_NODE, &show_ip_msdp_sa_sg_cmd);
	install_element(VIEW_NODE, &show_ip_msdp_sa_sg_vrf_all_cmd);
	install_element(VIEW_NODE, &show_ip_msdp_mesh_group_cmd);
	install_element(VIEW_NODE, &show_ip_msdp_mesh_group_vrf_all_cmd);
#endif /* PIM_IPV != 6 */
	install_element(VIEW_NODE, &show_ip_pim_ssm_range_cmd);
	install_element(VIEW_NODE, &show_ip_pim_group_type_cmd);
	install_element(VIEW_NODE, &show_ip_pim_vxlan_sg_cmd);
	install_element(VIEW_NODE, &show_ip_pim_vxlan_sg_work_cmd);
	install_element(INTERFACE_NODE, &interface_pim_use_source_cmd);
	install_element(INTERFACE_NODE, &interface_no_pim_use_source_cmd);
	/* Install BSM command */
	install_element(INTERFACE_NODE, &ip_pim_bsm_cmd);
	install_element(INTERFACE_NODE, &no_ip_pim_bsm_cmd);
	install_element(INTERFACE_NODE, &ip_pim_ucast_bsm_cmd);
	install_element(INTERFACE_NODE, &no_ip_pim_ucast_bsm_cmd);
	/* Install BFD command */
	install_element(INTERFACE_NODE, &ip_pim_bfd_cmd);
	install_element(INTERFACE_NODE, &ip_pim_bfd_param_cmd);
	install_element(INTERFACE_NODE, &no_ip_pim_bfd_profile_cmd);
	install_element(INTERFACE_NODE, &no_ip_pim_bfd_cmd);
#if HAVE_BFDD == 0
	install_element(INTERFACE_NODE, &no_ip_pim_bfd_param_cmd);
#endif /* !HAVE_BFDD */
}
