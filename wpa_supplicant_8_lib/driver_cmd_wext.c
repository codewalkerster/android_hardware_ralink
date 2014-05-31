/*
 * Driver interaction with extended Linux Wireless Extensions
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * Alternatively, this software may be distributed under the terms of BSD
 * license.
 *
 */

#include "includes.h"
#include <sys/ioctl.h>
#include <net/if_arp.h>
#include <net/if.h>

#include "wireless_copy.h"
#include "common.h"
#include "driver.h"
#include "eloop.h"
#include "priv_netlink.h"
#include "driver_wext.h"
#include "ieee802_11_defs.h"
#include "wpa_common.h"
#include "wpa_ctrl.h"
#include "wpa_supplicant_i.h"
#include "config.h"
#include "linux_ioctl.h"
#include "scan.h"

#include "driver_cmd_wext.h"
#include "driver_cmd_common.h"

/**
 * wpa_driver_wext_set_scan_timeout - Set scan timeout to report scan completion
 * @priv:  Pointer to private wext data from wpa_driver_wext_init()
 *
 * This function can be used to set registered timeout when starting a scan to
 * generate a scan completed event if the driver does not report this.
 */
static void wpa_driver_wext_set_scan_timeout(void *priv)
{
	struct wpa_driver_wext_data *drv = priv;
	int timeout = 10; /* In case scan A and B bands it can be long */

	/* Not all drivers generate "scan completed" wireless event, so try to
	 * read results after a timeout. */
	if (drv->scan_complete_events) {
	/*
	 * The driver seems to deliver SIOCGIWSCAN events to notify
	 * when scan is complete, so use longer timeout to avoid race
	 * conditions with scanning and following association request.
	 */
		timeout = 30;
	}
	wpa_printf(MSG_DEBUG, "Scan requested - scan timeout %d seconds",
		   timeout);
	eloop_cancel_timeout(wpa_driver_wext_scan_timeout, drv, drv->ctx);
	eloop_register_timeout(timeout, 0, wpa_driver_wext_scan_timeout, drv,
			       drv->ctx);
}

/**
 * wpa_driver_wext_combo_scan - Request the driver to initiate combo scan
 * @priv: Pointer to private wext data from wpa_driver_wext_init()
 * @params: Scan parameters
 * Returns: 0 on success, -1 on failure
 */
int wpa_driver_wext_combo_scan(void *priv, struct wpa_driver_scan_params *params)
{
	struct wpa_driver_wext_data *drv = priv;
	struct iwreq iwr;
	int ret = 0, timeout;
	struct iw_scan_req req;
	const u8 *ssid = params->ssids[0].ssid;
	size_t ssid_len = params->ssids[0].ssid_len;

	if (ssid_len > IW_ESSID_MAX_SIZE) {
		wpa_printf(MSG_DEBUG, "%s: too long SSID (%lu)",
			   __FUNCTION__, (unsigned long) ssid_len);
		return -1;
	}

	os_memset(&iwr, 0, sizeof(iwr));
	os_strlcpy(iwr.ifr_name, drv->ifname, IFNAMSIZ);

	if (ssid && ssid_len) {
		os_memset(&req, 0, sizeof(req));
		req.essid_len = ssid_len;
		req.bssid.sa_family = ARPHRD_ETHER;
		os_memset(req.bssid.sa_data, 0xff, ETH_ALEN);
		os_memcpy(req.essid, ssid, ssid_len);
		iwr.u.data.pointer = (caddr_t) &req;
		iwr.u.data.length = sizeof(req);
		iwr.u.data.flags = IW_SCAN_THIS_ESSID;
	}

	if (ioctl(drv->ioctl_sock, SIOCSIWSCAN, &iwr) < 0) {
		wpa_printf(MSG_ERROR, "ioctl[SIOCSIWSCAN]");
		ret = -1;
	}

	/* Not all drivers generate "scan completed" wireless event, so try to
	 * read results after a timeout. */
	timeout = 10;
	if (drv->scan_complete_events) {
		/*
		 * The driver seems to deliver SIOCGIWSCAN events to notify
		 * when scan is complete, so use longer timeout to avoid race
		 * conditions with scanning and following association request.
		 */
		timeout = 30;
	}
	wpa_printf(MSG_DEBUG, "Scan requested (ret=%d) - scan timeout %d "
		   "seconds", ret, timeout);
	eloop_cancel_timeout(wpa_driver_wext_scan_timeout, drv, drv->ctx);
	eloop_register_timeout(timeout, 0, wpa_driver_wext_scan_timeout, drv,
			       drv->ctx);

	return ret;
}

static int wpa_driver_wext_set_cscan_params(char *buf, size_t buf_len, char *cmd)
{
	char *pasv_ptr;
	int bp, i;
	u16 pasv_dwell = WEXT_CSCAN_PASV_DWELL_TIME_DEF;
	u8 channel;

	wpa_printf(MSG_DEBUG, "%s: %s", __func__, cmd);

	/* Get command parameters */
	pasv_ptr = os_strstr(cmd, ",TIME=");
	if (pasv_ptr) {
		*pasv_ptr = '\0';
		pasv_ptr += 6;
		pasv_dwell = (u16)atoi(pasv_ptr);
		if (pasv_dwell == 0)
			pasv_dwell = WEXT_CSCAN_PASV_DWELL_TIME_DEF;
	}
	channel = (u8)atoi(cmd + 5);

	bp = WEXT_CSCAN_HEADER_SIZE;
	os_memcpy(buf, WEXT_CSCAN_HEADER, bp);

	/* Set list of channels */
	buf[bp++] = WEXT_CSCAN_CHANNEL_SECTION;
	buf[bp++] = channel;
	if (channel != 0) {
		i = (pasv_dwell - 1) / WEXT_CSCAN_PASV_DWELL_TIME_DEF;
		for (; i > 0; i--) {
			if ((size_t)(bp + 12) >= buf_len)
				break;
			buf[bp++] = WEXT_CSCAN_CHANNEL_SECTION;
			buf[bp++] = channel;
		}
	} else {
		if (pasv_dwell > WEXT_CSCAN_PASV_DWELL_TIME_MAX)
			pasv_dwell = WEXT_CSCAN_PASV_DWELL_TIME_MAX;
	}

	/* Set passive dwell time (default is 250) */
	buf[bp++] = WEXT_CSCAN_PASV_DWELL_SECTION;
	if (channel != 0) {
		buf[bp++] = (u8)WEXT_CSCAN_PASV_DWELL_TIME_DEF;
		buf[bp++] = (u8)(WEXT_CSCAN_PASV_DWELL_TIME_DEF >> 8);
	} else {
		buf[bp++] = (u8)pasv_dwell;
		buf[bp++] = (u8)(pasv_dwell >> 8);
	}

	/* Set home dwell time (default is 40) */
	buf[bp++] = WEXT_CSCAN_HOME_DWELL_SECTION;
	buf[bp++] = (u8)WEXT_CSCAN_HOME_DWELL_TIME;
	buf[bp++] = (u8)(WEXT_CSCAN_HOME_DWELL_TIME >> 8);

	/* Set cscan type */
	buf[bp++] = WEXT_CSCAN_TYPE_SECTION;
	buf[bp++] = WEXT_CSCAN_TYPE_PASSIVE;
	return bp;
}

static char *wpa_driver_get_country_code(int channels)
{
	char *country = "US"; /* WEXT_NUMBER_SCAN_CHANNELS_FCC */

	if (channels == WEXT_NUMBER_SCAN_CHANNELS_ETSI)
		country = "EU";
	else if( channels == WEXT_NUMBER_SCAN_CHANNELS_MKK1)
		country = "JP";
	return country;
}

static int wpa_driver_set_backgroundscan_params(void *priv)
{
	struct wpa_driver_wext_data *drv = priv;
	struct wpa_supplicant *wpa_s;
	struct iwreq iwr;
	int ret = 0, i = 0, bp;
	char buf[WEXT_PNO_MAX_COMMAND_SIZE];
	struct wpa_ssid *ssid_conf;

	if (drv == NULL) {
		wpa_printf(MSG_ERROR, "%s: drv is NULL. Exiting", __func__);
		return -1;
	}
	if (drv->ctx == NULL) {
		wpa_printf(MSG_ERROR, "%s: drv->ctx is NULL. Exiting", __func__);
		return -1;
	}
	wpa_s = (struct wpa_supplicant *)(drv->ctx);
	if (wpa_s->conf == NULL) {
		wpa_printf(MSG_ERROR, "%s: wpa_s->conf is NULL. Exiting", __func__);
		return -1;
	}
	ssid_conf = wpa_s->conf->ssid;

	bp = WEXT_PNOSETUP_HEADER_SIZE;
	os_memcpy(buf, WEXT_PNOSETUP_HEADER, bp);
	buf[bp++] = WEXT_PNO_TLV_PREFIX;
	buf[bp++] = WEXT_PNO_TLV_VERSION;
	buf[bp++] = WEXT_PNO_TLV_SUBVERSION;
	buf[bp++] = WEXT_PNO_TLV_RESERVED;

	while ((i < WEXT_PNO_AMOUNT) && (ssid_conf != NULL)) {
		/* Check that there is enough space needed for 1 more SSID, the other sections and null termination */
		if ((bp + WEXT_PNO_SSID_HEADER_SIZE + IW_ESSID_MAX_SIZE + WEXT_PNO_NONSSID_SECTIONS_SIZE + 1) >= (int)sizeof(buf))
			break;
		if ((!ssid_conf->disabled) && (ssid_conf->ssid_len <= IW_ESSID_MAX_SIZE)){
			wpa_printf(MSG_DEBUG, "For PNO Scan: %s", ssid_conf->ssid);
			buf[bp++] = WEXT_PNO_SSID_SECTION;
			buf[bp++] = ssid_conf->ssid_len;
			os_memcpy(&buf[bp], ssid_conf->ssid, ssid_conf->ssid_len);
			bp += ssid_conf->ssid_len;
			i++;
		}
		ssid_conf = ssid_conf->next;
	}

	buf[bp++] = WEXT_PNO_SCAN_INTERVAL_SECTION;
	os_snprintf(&buf[bp], WEXT_PNO_SCAN_INTERVAL_LENGTH + 1, "%x", WEXT_PNO_SCAN_INTERVAL);
	bp += WEXT_PNO_SCAN_INTERVAL_LENGTH;

	buf[bp++] = WEXT_PNO_REPEAT_SECTION;
	os_snprintf(&buf[bp], WEXT_PNO_REPEAT_LENGTH + 1, "%x", WEXT_PNO_REPEAT);
	bp += WEXT_PNO_REPEAT_LENGTH;

	buf[bp++] = WEXT_PNO_MAX_REPEAT_SECTION;
	os_snprintf(&buf[bp], WEXT_PNO_MAX_REPEAT_LENGTH + 1, "%x", WEXT_PNO_MAX_REPEAT);
	bp += WEXT_PNO_MAX_REPEAT_LENGTH + 1;

	os_memset(&iwr, 0, sizeof(iwr));
	os_strncpy(iwr.ifr_name, drv->ifname, IFNAMSIZ);
	iwr.u.data.pointer = buf;
	iwr.u.data.length = bp;

	ret = ioctl(drv->ioctl_sock, SIOCSIWPRIV, &iwr);

	if (ret < 0) {
		wpa_printf(MSG_ERROR, "ioctl[SIOCSIWPRIV] (pnosetup): %d", ret);
		drv->errors++;
		if (drv->errors > DRV_NUMBER_SEQUENTIAL_ERRORS) {
			drv->errors = 0;
			wpa_msg(drv->ctx, MSG_INFO, WPA_EVENT_DRIVER_STATE "HANGED");
		}
	} else {
		drv->errors = 0;
	}
	return ret;

}

int wpa_driver_wext_driver_cmd( void *priv, char *cmd, char *buf, size_t buf_len )
{
	struct wpa_driver_wext_data *drv = priv;
	struct wpa_supplicant *wpa_s = (struct wpa_supplicant *)(drv->ctx);
	struct iwreq iwr;
	int ret = 0, flags;

	wpa_printf(MSG_DEBUG, "%s %s len = %d", __func__, cmd, buf_len);

	if (!drv->driver_is_started && (os_strcasecmp(cmd, "START") != 0)) {
		wpa_printf(MSG_ERROR,"WEXT: Driver not initialized yet");
		return -1;
	}

	if (os_strcasecmp(cmd, "RSSI-APPROX") == 0) {
		os_strncpy(cmd, RSSI_CMD, MAX_DRV_CMD_SIZE);
	} else if( os_strncasecmp(cmd, "SCAN-CHANNELS", 13) == 0 ) {
		int no_of_chan;

		no_of_chan = atoi(cmd + 13);
		os_snprintf(cmd, MAX_DRV_CMD_SIZE, "COUNTRY %s",
			wpa_driver_get_country_code(no_of_chan));
	} else if (os_strcasecmp(cmd, "STOP") == 0) {
		linux_set_iface_flags(drv->ioctl_sock, drv->ifname, 0);
	} else if( os_strcasecmp(cmd, "RELOAD") == 0 ) {
		wpa_printf(MSG_DEBUG,"Reload command");
		wpa_msg(drv->ctx, MSG_INFO, WPA_EVENT_DRIVER_STATE "HANGED");
		return ret;
	} else if( os_strcasecmp(cmd, "BGSCAN-START") == 0 ) {
		ret = wpa_driver_set_backgroundscan_params(priv);
		if (ret < 0) {
			return ret;
		}
		os_strncpy(cmd, "PNOFORCE 1", MAX_DRV_CMD_SIZE);
		drv->bgscan_enabled = 1;
	} else if( os_strcasecmp(cmd, "BGSCAN-STOP") == 0 ) {
		os_strncpy(cmd, "PNOFORCE 0", MAX_DRV_CMD_SIZE);
		drv->bgscan_enabled = 0;
	}

	os_memset(&iwr, 0, sizeof(iwr));
	os_strncpy(iwr.ifr_name, drv->ifname, IFNAMSIZ);
	os_memcpy(buf, cmd, strlen(cmd) + 1);
	iwr.u.data.pointer = buf;
	iwr.u.data.length = buf_len;

	if( os_strncasecmp(cmd, "CSCAN", 5) == 0 ) {
		if (!wpa_s->scanning && ((wpa_s->wpa_state <= WPA_SCANNING) ||
					(wpa_s->wpa_state >= WPA_COMPLETED))) {
			iwr.u.data.length = wpa_driver_wext_set_cscan_params(buf, buf_len, cmd);
		} else {
			wpa_printf(MSG_ERROR, "Ongoing Scan action...");
			return ret;
		}
	}
	ret = ioctl(drv->ioctl_sock, SIOCSIWPRIV, &iwr);

	if (ret < 0) {
		wpa_printf(MSG_DEBUG, "%s failed (%d): %s", __func__, ret, cmd);
	}

    //
    // All command fail. (Allways OK for USB Dongle)
    //
	ret = 0;
	
	if (ret < 0) {
		wpa_printf(MSG_ERROR, "%s failed (%d): %s", __func__, ret, cmd);
		drv->errors++;
		if (drv->errors > DRV_NUMBER_SEQUENTIAL_ERRORS) {
			drv->errors = 0;
			wpa_msg(drv->ctx, MSG_INFO, WPA_EVENT_DRIVER_STATE "HANGED");
		}
	} else {
		drv->errors = 0;
		ret = 0;
		if ((os_strcasecmp(cmd, RSSI_CMD) == 0) ||
		    (os_strcasecmp(cmd, LINKSPEED_CMD) == 0) ||
		    (os_strcasecmp(cmd, "MACADDR") == 0) ||
		    (os_strcasecmp(cmd, "GETPOWER") == 0) ||
		    (os_strcasecmp(cmd, "GETBAND") == 0)) {
			ret = strlen(buf);
		} else if (os_strcasecmp(cmd, "START") == 0) {
			drv->driver_is_started = TRUE;
			linux_set_iface_flags(drv->ioctl_sock, drv->ifname, 1);
			/* os_sleep(0, WPA_DRIVER_WEXT_WAIT_US);
			wpa_msg(drv->ctx, MSG_INFO, WPA_EVENT_DRIVER_STATE "STARTED"); */
		} else if (os_strcasecmp(cmd, "STOP") == 0) {
			drv->driver_is_started = FALSE;
			/* wpa_msg(drv->ctx, MSG_INFO, WPA_EVENT_DRIVER_STATE "STOPPED"); */
		} else if (os_strncasecmp(cmd, "CSCAN", 5) == 0) {
			wpa_driver_wext_set_scan_timeout(priv);
			wpa_supplicant_notify_scanning(wpa_s, 1);
		}
		wpa_printf(MSG_DEBUG, "%s %s len = %d, %d", __func__, buf, ret, strlen(buf));
	}
	return ret;
}

int wpa_driver_signal_poll(void *priv, struct wpa_signal_info *si)
{
    // fix
	si->current_signal = -60;
	si->current_txrate = 150 * 1000;
	return 0;
}
