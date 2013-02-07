/*
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston,
 * MA 02111-1307 USA
 *
 * udhcpc scripts
 *
 * Copyright 2004, ASUSTeK Inc.
 * All Rights Reserved.
 * 
 * THIS SOFTWARE IS OFFERED "AS IS", AND BROADCOM GRANTS NO WARRANTIES OF ANY
 * KIND, EXPRESS OR IMPLIED, BY STATUTE, COMMUNICATION OR OTHERWISE. BROADCOM
 * SPECIFICALLY DISCLAIMS ANY IMPLIED WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A SPECIFIC PURPOSE OR NONINFRINGEMENT CONCERNING THIS SOFTWARE.
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <net/route.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>

#include <nvram/bcmnvram.h>
#include <netconf.h>
#include <shutils.h>
#include <rc.h>
#include <semaphore_mfp.h>

char udhcpstate[8];

static int
expires(char *wan_ifname, unsigned int in)
{
	time_t now;
	FILE *fp;
	char tmp[100];
	int unit;

	if ((unit = wan_ifunit(wan_ifname)) < 0)
		return -1;
	
	time(&now);
	snprintf(tmp, sizeof(tmp), "/tmp/udhcpc%d.expires", unit); 
	if (!(fp = fopen(tmp, "w"))) {
		perror(tmp);
		return errno;
	}
	fprintf(fp, "%d", (unsigned int) now + in);
	fclose(fp);
	return 0;
}	

/* 
 * deconfig: This argument is used when udhcpc starts, and when a
 * leases is lost. The script should put the interface in an up, but
 * deconfigured state.
*/
static int
deconfig(void)
{
	char *wan_ifname = safe_getenv("interface");

	if (nvram_match("wan0_proto", "l2tp") || nvram_match("wan0_proto", "pptp"))
	{
		/* fix hang-up issue */
		logmessage("dhcp client", "skipping resetting IP address to 0.0.0.0");
	} else
		ifconfig(wan_ifname, IFUP, "0.0.0.0", NULL);
	expires(wan_ifname, 0);

	wan_down(wan_ifname);

	logmessage("dhcp client", "%s: lease is lost", udhcpstate);
	wanmessage("lost IP from server");

	return 0;
}

/*
 * bound: This argument is used when udhcpc moves from an unbound, to
 * a bound state. All of the paramaters are set in enviromental
 * variables, The script should configure the interface, and set any
 * other relavent parameters (default gateway, dns server, etc).
*/

static int
bound(void)	// udhcpc bound here, also call wanup
{
	char *wan_ifname = safe_getenv("interface");
	char *value;
	char tmp[100], prefix[] = "wanXXXXXXXXXX_";
	int unit;

	if ((unit = wan_ifunit(wan_ifname)) < 0) 
		strcpy(prefix, "wanx_");
	else
		snprintf(prefix, sizeof(prefix), "wan%d_", unit);

	if ((value = getenv("ip")))
		nvram_set(strcat_r(prefix, "ipaddr", tmp), trim_r(value));
	if ((value = getenv("subnet")))
		nvram_set(strcat_r(prefix, "netmask", tmp), trim_r(value));
        if ((value = getenv("router")))
		nvram_set(strcat_r(prefix, "gateway", tmp), trim_r(value));
	if ((value = getenv("dns")))
	{
		nvram_set(strcat_r(prefix, "dns", tmp), trim_r(value));
		dbG("dhcp client::bound", "dns: %s", nvram_safe_get(strcat_r(prefix, "dns", tmp)));
		logmessage("dhcp client::bound", "dns: %s", nvram_safe_get(strcat_r(prefix, "dns", tmp)));
	}
	if ((value = getenv("wins")))
		nvram_set(strcat_r(prefix, "wins", tmp), trim_r(value));
	else
		nvram_set(strcat_r(prefix, "wins", tmp), "");

	nvram_set(strcat_r(prefix, "routes", tmp), getenv("routes"));
	nvram_set(strcat_r(prefix, "msroutes", tmp), getenv("msroutes"));
#if 0
	if ((value = getenv("hostname")))
		sethostname(trim_r(value), strlen(value) + 1);
#endif
	if ((value = getenv("domain")))
		nvram_set(strcat_r(prefix, "domain", tmp), trim_r(value));
	if ((value = getenv("lease"))) {
		nvram_set(strcat_r(prefix, "lease", tmp), trim_r(value));
		expires(wan_ifname, atoi(value));
	}

	ifconfig(wan_ifname, IFUP,
		 nvram_safe_get(strcat_r(prefix, "ipaddr", tmp)),
		 nvram_safe_get(strcat_r(prefix, "netmask", tmp)));

	spinlock_lock(SPINLOCK_DHCPRenew);
	nvram_set("dhcp_renew", "0");	// for detectWAN
	spinlock_unlock(SPINLOCK_DHCPRenew);

	wan_up(wan_ifname);

	logmessage("dhcp client", "%s IP: %s from %s (prefix: %s)", 
		udhcpstate, 
		nvram_safe_get(strcat_r(prefix, "ipaddr", tmp)), 
		nvram_safe_get(strcat_r(prefix, "gateway", tmp)), prefix);

	wanmessage("");
	dprintf("done\n");
	return 0;
}

/*
 * renew: This argument is used when a DHCP lease is renewed. All of
 * the paramaters are set in enviromental variables. This argument is
 * used when the interface is already configured, so the IP address,
 * will not change, however, the other DHCP paramaters, such as the
 * default gateway, subnet mask, and dns server may change.
 */
static int
renew(void)
{
	char *wan_ifname = safe_getenv("interface");
	char *value;
	char tmp[100], prefix[] = "wanXXXXXXXXXX_";
	int unit;

	if ((unit = wan_ifunit(wan_ifname)) < 0)
		strcpy(prefix, "wanx_");
	else
		snprintf(prefix, sizeof(prefix), "wan%d_", unit);

	if (!(value = getenv("subnet")) || !nvram_match(strcat_r(prefix, "netmask", tmp), trim_r(value)))
		return bound();
	if (!(value = getenv("router")) || !nvram_match(strcat_r(prefix, "gateway", tmp), trim_r(value)))
		return bound();

	if ((value = getenv("dns")) && !nvram_match(strcat_r(prefix, "dns", tmp), trim_r(value))) {
		nvram_set(strcat_r(prefix, "dns", tmp), trim_r(value));
		logmessage("dhcp client::renew", "dns: %s", nvram_safe_get(strcat_r(prefix, "dns", tmp)));
#if 0
		update_resolvconf();
#else
		dbG("dhcp client::renew", "call add_dns() with dns: %s", nvram_safe_get(strcat_r(prefix, "dns", tmp)));
		logmessage("dhcp client::renew", "call add_dns() with dns: %s", nvram_safe_get(strcat_r(prefix, "dns", tmp)));
		add_dns(wan_ifname);
#endif
	}

	if ((value = getenv("wins")))
		nvram_set(strcat_r(prefix, "wins", tmp), trim_r(value));
	else
		nvram_set(strcat_r(prefix, "wins", tmp), "");
#if 0
	if ((value = getenv("hostname")))
		sethostname(trim_r(value), strlen(value) + 1);
#endif
	if ((value = getenv("domain")))
		nvram_set(strcat_r(prefix, "domain", tmp), trim_r(value));
	if ((value = getenv("lease"))) {
		nvram_set(strcat_r(prefix, "lease", tmp), trim_r(value));
		expires(wan_ifname, atoi(value));
	}

	logmessage("dhcp client", "%s IP: %s from %s (prefix: %s)", 
		udhcpstate, 
		nvram_safe_get(strcat_r(prefix, "ipaddr", tmp)), 
		nvram_safe_get(strcat_r(prefix, "gateway", tmp)), prefix);

	if (unit == 0)
		update_wan_status(1);

	wanmessage("");

	dprintf("done\n");
	return 0;
}

int
udhcpc_main(int argc, char **argv)
{
	if (argv[1]) strcpy(udhcpstate, argv[1]);

	if (!argv[1])
		return EINVAL;
	else if (strstr(argv[1], "deconfig"))
		return deconfig();
	else if (strstr(argv[1], "bound"))
		return bound();
	else if (strstr(argv[1], "renew"))
		return renew();
	else if (strstr(argv[1], "leasefail"))
		return 0;
	else return deconfig();
}

