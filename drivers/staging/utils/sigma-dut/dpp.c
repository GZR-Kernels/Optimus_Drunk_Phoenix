/*
 * Sigma Control API DUT (station/AP/sniffer)
 * Copyright (c) 2017, Qualcomm Atheros, Inc.
 * Copyright (c) 2018, The Linux Foundation
 * All Rights Reserved.
 * Licensed under the Clear BSD license. See README for more details.
 */

#include "sigma_dut.h"
#include <sys/wait.h>
#include "wpa_ctrl.h"
#include "wpa_helpers.h"

#ifdef ANDROID
char *dpp_qrcode_file = "/sdcard/wpadebug_qrdata.txt";
#endif /* ANDROID */


static int sigma_dut_is_ap(struct sigma_dut *dut)
{
	return dut->device_type == AP_unknown ||
		dut->device_type == AP_testbed ||
		dut->device_type == AP_dut;
}


static int dpp_hostapd_run(struct sigma_dut *dut)
{
	if (dut->hostapd_running)
		return 0;

	sigma_dut_print(dut, DUT_MSG_INFO,
			"Starting hostapd in unconfigured state for DPP");
	snprintf(dut->ap_ssid, sizeof(dut->ap_ssid), "unconfigured");
	if (!dut->ap_oper_chn)
		dut->ap_channel = 11;
	dut->ap_is_dual = 0;
	dut->ap_mode = dut->ap_channel <= 14 ? AP_11ng : AP_11na;
	dut->ap_key_mgmt = AP_OPEN;
	dut->ap_cipher = AP_PLAIN;
	return cmd_ap_config_commit(dut, NULL, NULL) == 1 ? 0 : -1;
}


static const char * dpp_get_curve(struct sigma_cmd *cmd, const char *arg)
{
	const char *val = get_param(cmd, arg);

	if (!val)
		val = "P-256";
	else if (strcasecmp(val, "BP-256R1") == 0)
		val = "BP-256";
	else if (strcasecmp(val, "BP-384R1") == 0)
		val = "BP-384";
	else if (strcasecmp(val, "BP-512R1") == 0)
		val = "BP-512";

	return val;
}


static int dpp_get_local_bootstrap(struct sigma_dut *dut,
				   struct sigma_conn *conn,
				   struct sigma_cmd *cmd, int send_result,
				   int *success)
{
	const char *curve = dpp_get_curve(cmd, "DPPCryptoIdentifier");
	const char *bs = get_param(cmd, "DPPBS");
	const char *chan_list = get_param(cmd, "DPPChannelList");
	char *pos, mac[50], buf[200], resp[1000], hex[2000];
	const char *ifname = get_station_ifname();
	int res;

	if (success)
		*success = 0;
	if (strcasecmp(bs, "QR") != 0) {
		send_resp(dut, conn, SIGMA_ERROR,
			  "errorCode,Unsupported DPPBS");
		return 0;
	}

	if (sigma_dut_is_ap(dut)) {
		u8 bssid[ETH_ALEN];

		if (!dut->hostapd_ifname) {
			sigma_dut_print(dut, DUT_MSG_ERROR,
					"hostapd ifname not specified (-j)");
			return -2;
		}
		ifname = dut->hostapd_ifname;
		if (get_hwaddr(dut->hostapd_ifname, bssid) < 0) {
			sigma_dut_print(dut, DUT_MSG_ERROR,
					"Could not get MAC address for %s",
					dut->hostapd_ifname);
			return -2;
		}
		snprintf(mac, sizeof(mac), "%02x%02x%02x%02x%02x%02x",
			 bssid[0], bssid[1], bssid[2],
			 bssid[3], bssid[4], bssid[5]);
	} else {
		if (get_wpa_status(ifname, "address", mac, sizeof(mac)) < 0)
			return -2;
	}

	pos = mac;
	while (*pos) {
		if (*pos == ':')
			memmove(pos, pos + 1, strlen(pos));
		else
			pos++;
	}

	if (sigma_dut_is_ap(dut) && dpp_hostapd_run(dut) < 0) {
		send_resp(dut, conn, SIGMA_ERROR,
			  "errorCode,Failed to start hostapd");
		return 0;
	}

	if (chan_list &&
	    (strcmp(chan_list, "0/0") == 0 || chan_list[0] == '\0')) {
		/* No channel list */
		res = snprintf(buf, sizeof(buf),
			       "DPP_BOOTSTRAP_GEN type=qrcode curve=%s mac=%s",
			       curve, mac);
	} else if (chan_list) {
		/* Channel list override (CTT case) - space separated tuple(s)
		 * of OperatingClass/Channel; convert to wpa_supplicant/hostapd
		 * format: comma separated tuples */
		strlcpy(resp, chan_list, sizeof(resp));
		for (pos = resp; *pos; pos++) {
			if (*pos == ' ')
				*pos = ',';
		}
		res = snprintf(buf, sizeof(buf),
			       "DPP_BOOTSTRAP_GEN type=qrcode curve=%s chan=%s mac=%s",
			       curve, resp, mac);
	} else {
		/* Default channel list (normal DUT case) */
		res = snprintf(buf, sizeof(buf),
			       "DPP_BOOTSTRAP_GEN type=qrcode curve=%s chan=81/11 mac=%s",
			       curve, mac);
	}

	if (res < 0 || res >= sizeof(buf) ||
	    wpa_command_resp(ifname, buf, resp, sizeof(resp)) < 0)
		return -2;
	if (strncmp(resp, "FAIL", 4) == 0)
		return -2;
	dut->dpp_local_bootstrap = atoi(resp);
	snprintf(buf, sizeof(buf), "DPP_BOOTSTRAP_GET_URI %d",
		 atoi(resp));
	if (wpa_command_resp(ifname, buf, resp, sizeof(resp)) < 0)
		return -2;
	if (strncmp(resp, "FAIL", 4) == 0)
		return -2;

	sigma_dut_print(dut, DUT_MSG_DEBUG, "URI: %s", resp);

	if (send_result) {
		ascii2hexstr(resp, hex);
		res = snprintf(resp, sizeof(resp), "BootstrappingData,%s", hex);
		send_resp(dut, conn, SIGMA_COMPLETE,
			  res >= 0 && res < sizeof(resp) ? resp : NULL);
	}

	if (success)
		*success = 1;
	return 0;
}


static int dpp_set_peer_bootstrap(struct sigma_dut *dut,
				  struct sigma_conn *conn,
				  struct sigma_cmd *cmd)
{
	const char *val = get_param(cmd, "DPPBootstrappingdata");
	char uri[1000];
	int res;

	if (!val) {
		send_resp(dut, conn, SIGMA_ERROR,
			  "errorCode,Missing DPPBootstrappingdata");
		return 0;
	}

	res = parse_hexstr(val, (unsigned char *) uri, sizeof(uri));
	if (res < 0 || (size_t) res >= sizeof(uri))
		return -2;
	uri[res] = '\0';
	sigma_dut_print(dut, DUT_MSG_DEBUG, "URI: %s", uri);
	free(dut->dpp_peer_uri);
	dut->dpp_peer_uri = strdup(uri);

	return 1;
}


static int dpp_hostapd_conf_update(struct sigma_dut *dut,
				   struct sigma_conn *conn, const char *ifname,
				   struct wpa_ctrl *ctrl)
{
	int res;
	char buf[2000], buf2[2500], *pos, *pos2;
	const char *conf_data_events[] = {
		"DPP-CONNECTOR",
		"DPP-CONFOBJ-PASS",
		"DPP-CONFOBJ-PSK",
		NULL
	};

	sigma_dut_print(dut, DUT_MSG_INFO,
			"Update hostapd configuration based on DPP Config Object");

	if (wpa_command(ifname, "SET wpa 2") < 0 ||
	    wpa_command(ifname, "SET wpa_key_mgmt DPP") < 0 ||
	    wpa_command(ifname, "SET ieee80211w 1") < 0 ||
	    wpa_command(ifname, "SET rsn_pairwise CCMP") < 0) {
		send_resp(dut, conn, SIGMA_ERROR,
			  "errorCode,Failed to update AP security parameters");
		goto out;
	}

	res = get_wpa_cli_event(dut, ctrl, "DPP-CONFOBJ-SSID",
				buf, sizeof(buf));
	if (res < 0) {
		send_resp(dut, conn, SIGMA_ERROR,
			  "errorCode,No DPP-CONFOBJ-SSID");
		goto out;
	}
	pos = strchr(buf, ' ');
	if (!pos)
		return -2;
	pos++;
	sigma_dut_print(dut, DUT_MSG_INFO,
			"DPP: Config Object SSID: %s", pos);
	snprintf(buf2, sizeof(buf2), "SET ssid %s", pos);
	if (wpa_command(ifname, buf2) < 0) {
		send_resp(dut, conn, SIGMA_ERROR,
			  "errorCode,Failed to update AP SSID");
		goto out;
	}

	res = get_wpa_cli_events(dut, ctrl, conf_data_events, buf, sizeof(buf));
	if (res < 0) {
		send_resp(dut, conn, SIGMA_ERROR,
			  "errorCode,No DPP-CONNECTOR/DPP-CONFOBJ-PASS/PSK");
		goto out;
	}

	if (!strstr(buf, "DPP-CONNECTOR")) {
		if (wpa_command(ifname, "SET wpa_key_mgmt WPA-PSK") < 0) {
			send_resp(dut, conn, SIGMA_ERROR,
				  "errorCode,Failed to update AP security parameters");
			goto out;
		}

		pos = strchr(buf, ' ');
		if (!pos)
			return -2;
		pos++;
		if (strstr(buf, "DPP-CONFOBJ-PASS")) {
			char pass[64];
			int pass_len;

			pass_len = parse_hexstr(pos, (u8 *) pass, sizeof(pass));
			if (pass_len < 0 || (size_t) pass_len >= sizeof(pass))
				return -2;
			pass[pass_len] = '\0';
			sigma_dut_print(dut, DUT_MSG_INFO,
					"DPP: Passphrase: %s", pass);
			snprintf(buf2, sizeof(buf2), "SET wpa_passphrase %s",
				 pass);
			if (wpa_command(ifname, buf2) < 0) {
				send_resp(dut, conn, SIGMA_ERROR,
					  "errorCode,Failed to set passphrase");
				goto out;
			}
		} else if (strstr(buf, "DPP-CONFOBJ-PSK")) {
			sigma_dut_print(dut, DUT_MSG_INFO,
					"DPP: PSK: %s", pos);
			snprintf(buf2, sizeof(buf2), "SET wpa_psk %s", pos);
			if (wpa_command(ifname, buf2) < 0) {
				send_resp(dut, conn, SIGMA_ERROR,
					  "errorCode,Failed to set PSK");
				goto out;
			}
		}

		goto skip_dpp_akm;
	}

	pos = strchr(buf, ' ');
	if (!pos)
		return -2;
	pos++;
	sigma_dut_print(dut, DUT_MSG_INFO, "DPP: Connector: %s", pos);
	snprintf(buf2, sizeof(buf2), "SET dpp_connector %s", pos);
	if (wpa_command(ifname, buf2) < 0) {
		send_resp(dut, conn, SIGMA_ERROR,
			  "errorCode,Failed to update AP Connector");
		goto out;
	}

	res = get_wpa_cli_event(dut, ctrl, "DPP-C-SIGN-KEY",
				buf, sizeof(buf));
	if (res < 0) {
		send_resp(dut, conn, SIGMA_ERROR,
			  "errorCode,No DPP-C-SIGN-KEY");
		goto out;
	}
	pos = strchr(buf, ' ');
	if (!pos)
		return -2;
	pos++;
	sigma_dut_print(dut, DUT_MSG_INFO, "DPP: C-sign-key: %s", pos);
	snprintf(buf2, sizeof(buf2), "SET dpp_csign %s", pos);
	if (wpa_command(ifname, buf2) < 0) {
		send_resp(dut, conn, SIGMA_ERROR,
			  "errorCode,Failed to update AP C-sign-key");
		goto out;
	}

	res = get_wpa_cli_event(dut, ctrl, "DPP-NET-ACCESS-KEY",
				buf, sizeof(buf));
	if (res < 0) {
		send_resp(dut, conn, SIGMA_ERROR,
			  "errorCode,No DPP-NET-ACCESS-KEY");
		goto out;
	}
	pos = strchr(buf, ' ');
	if (!pos)
		return -2;
	pos++;
	pos2 = strchr(pos, ' ');
	if (pos2)
		*pos2++ = '\0';
	sigma_dut_print(dut, DUT_MSG_INFO, "DPP: netAccessKey: %s", pos);
	snprintf(buf2, sizeof(buf2), "SET dpp_netaccesskey %s", pos);
	if (wpa_command(ifname, buf2) < 0) {
		send_resp(dut, conn, SIGMA_ERROR,
			  "errorCode,Failed to update AP netAccessKey");
		goto out;
	}
	if (pos2) {
		sigma_dut_print(dut, DUT_MSG_INFO,
				"DPP: netAccessKey expiry: %s", pos2);
		snprintf(buf2, sizeof(buf2), "SET dpp_netaccesskey_expiry %s",
			 pos2);
		if (wpa_command(ifname, buf2) < 0) {
			send_resp(dut, conn, SIGMA_ERROR,
				  "errorCode,Failed to update AP netAccessKey expiry");
			goto out;
		}
	}
skip_dpp_akm:

	if (wpa_command(ifname, "DISABLE") < 0 ||
	    wpa_command(ifname, "ENABLE") < 0) {
		send_resp(dut, conn, SIGMA_ERROR,
			  "errorCode,Failed to update AP configuration");
		goto out;
	}

	res = get_wpa_cli_event(dut, ctrl, "AP-ENABLED", buf, sizeof(buf));
	if (res < 0) {
		send_resp(dut, conn, SIGMA_ERROR, "errorCode,No AP-ENABLED");
		goto out;
	}

	return 1;
out:
	return 0;
}


struct dpp_test_info {
	const char *step;
	const char *frame;
	const char *attr;
	int value;
};

static const struct dpp_test_info dpp_tests[] = {
	{ "InvalidValue", "AuthenticationRequest", "WrappedData", 1 },
	{ "InvalidValue", "AuthenticationResponse", "WrappedData", 2 },
	{ "InvalidValue", "AuthenticationResponse", "PrimaryWrappedData", 2 },
	{ "InvalidValue", "AuthenticationConfirm", "WrappedData", 3 },
	{ "InvalidValue", "PKEXCRRequest", "WrappedData", 4 },
	{ "InvalidValue", "PKEXCRResponse", "WrappedData", 5 },
	{ "InvalidValue", "ConfigurationRequest", "WrappedData", 6 },
	{ "InvalidValue", "ConfigurationResponse", "WrappedData", 7 },
	{ "InvalidValue", "AuthenticationRequest", "InitCapabilities", 8 },
	{ "MissingAttribute", "AuthenticationRequest", "RespBSKeyHash", 10 },
	{ "MissingAttribute", "AuthenticationRequest", "InitBSKeyHash", 11 },
	{ "MissingAttribute", "AuthenticationRequest", "InitProtocolKey", 12 },
	{ "MissingAttribute", "AuthenticationRequest", "InitNonce", 13 },
	{ "MissingAttribute", "AuthenticationRequest", "InitCapabilities", 14 },
	{ "MissingAttribute", "AuthenticationRequest", "WrappedData", 15 },
	{ "MissingAttribute", "AuthenticationResponse", "DPPStatus", 16 },
	{ "MissingAttribute", "AuthenticationResponse", "RespBSKeyHash", 17 },
	{ "MissingAttribute", "AuthenticationResponse", "InitBSKeyHash", 18 },
	{ "MissingAttribute", "AuthenticationResponse", "RespProtocolKey", 19 },
	{ "MissingAttribute", "AuthenticationResponse", "RespNonce", 20 },
	{ "MissingAttribute", "AuthenticationResponse", "InitNonce", 21 },
	{ "MissingAttribute", "AuthenticationResponse", "RespCapabilities",
	  22 },
	{ "MissingAttribute", "AuthenticationResponse", "RespAuthTag", 23 },
	{ "MissingAttribute", "AuthenticationResponse", "WrappedData", 24 },
	{ "MissingAttribute", "AuthenticationResponse", "PrimaryWrappedData",
	  24 },
	{ "MissingAttribute", "AuthenticationConfirm", "DPPStatus", 25 },
	{ "MissingAttribute", "AuthenticationConfirm", "RespBSKeyHash", 26 },
	{ "MissingAttribute", "AuthenticationConfirm", "InitBSKeyHash", 27 },
	{ "MissingAttribute", "AuthenticationConfirm", "InitAuthTag", 28 },
	{ "MissingAttribute", "AuthenticationConfirm", "WrappedData", 29 },
	{ "InvalidValue", "AuthenticationResponse", "InitNonce", 30 },
	{ "InvalidValue", "AuthenticationResponse", "RespCapabilities", 31 },
	{ "InvalidValue", "AuthenticationResponse", "RespAuthTag", 32 },
	{ "InvalidValue", "AuthenticationConfirm", "InitAuthTag", 33 },
	{ "MissingAttribute", "PKEXExchangeRequest", "FiniteCyclicGroup", 34 },
	{ "MissingAttribute", "PKEXExchangeRequest", "EncryptedKey", 35 },
	{ "MissingAttribute", "PKEXExchangeResponse", "DPPStatus", 36 },
	{ "MissingAttribute", "PKEXExchangeResponse", "EncryptedKey", 37 },
	{ "MissingAttribute", "PKEXCRRequest", "BSKey", 38 },
	{ "MissingAttribute", "PKEXCRRequest", "InitAuthTag", 39 },
	{ "MissingAttribute", "PKEXCRRequest", "WrappedData", 40 },
	{ "MissingAttribute", "PKEXCRResponse", "BSKey", 41 },
	{ "MissingAttribute", "PKEXCRResponse", "RespAuthTag", 42 },
	{ "MissingAttribute", "PKEXCRResponse", "WrappedData", 43 },
	{ "InvalidValue", "PKEXExchangeRequest", "EncryptedKey", 44 },
	{ "InvalidValue", "PKEXExchangeResponse", "EncryptedKey", 45 },
	{ "InvalidValue", "PKEXExchangeResponse", "DPPStatus", 46 },
	{ "InvalidValue", "PKEXCRRequest", "BSKey", 47 },
	{ "InvalidValue", "PKEXCRResponse", "BSKey", 48 },
	{ "InvalidValue", "PKEXCRRequest", "InitAuthTag", 49 },
	{ "InvalidValue", "PKEXCRResponse", "RespAuthTag", 50 },
	{ "MissingAttribute", "ConfigurationRequest", "EnrolleeNonce", 51 },
	{ "MissingAttribute", "ConfigurationRequest", "ConfigAttr", 52 },
	{ "MissingAttribute", "ConfigurationRequest", "WrappedData", 53 },
	{ "MissingAttribute", "ConfigurationResponse", "EnrolleeNonce", 54 },
	{ "MissingAttribute", "ConfigurationResponse", "ConfigObj", 55 },
	{ "MissingAttribute", "ConfigurationResponse", "DPPStatus", 56 },
	{ "MissingAttribute", "ConfigurationResponse", "WrappedData", 57 },
	{ "InvalidValue", "ConfigurationResponse", "DPPStatus", 58 },
	{ "InvalidValue", "ConfigurationResponse", "EnrolleeNonce", 59 },
	{ "MissingAttribute", "PeerDiscoveryRequest", "TransactionID", 60 },
	{ "MissingAttribute", "PeerDiscoveryRequest", "Connector", 61 },
	{ "MissingAttribute", "PeerDiscoveryResponse", "TransactionID", 62 },
	{ "MissingAttribute", "PeerDiscoveryResponse", "DPPStatus", 63 },
	{ "MissingAttribute", "PeerDiscoveryResponse", "Connector", 64 },
	{ "InvalidValue", "AuthenticationRequest", "InitProtocolKey", 66 },
	{ "InvalidValue", "AuthenticationResponse", "RespProtocolKey", 67 },
	{ "InvalidValue", "AuthenticationRequest", "RespBSKeyHash", 68 },
	{ "InvalidValue", "AuthenticationRequest", "InitBSKeyHash", 69 },
	{ "InvalidValue", "AuthenticationResponse", "RespBSKeyHash", 70 },
	{ "InvalidValue", "AuthenticationResponse", "InitBSKeyHash", 71 },
	{ "InvalidValue", "AuthenticationConfirm", "RespBSKeyHash", 72 },
	{ "InvalidValue", "AuthenticationConfirm", "InitBSKeyHash", 73 },
	{ "InvalidValue", "AuthenticationResponse", "DPPStatus", 74 },
	{ "InvalidValue", "AuthenticationConfirm", "DPPStatus", 75 },
	{ "InvalidValue", "ConfigurationRequest", "ConfigAttr", 76 },
	{ "InvalidValue", "PeerDiscoveryResponse", "TransactionID", 77 },
	{ "InvalidValue", "PeerDiscoveryResponse", "DPPStatus", 78 },
	{ "InvalidValue", "PeerDiscoveryResponse", "Connector", 79 },
	{ "InvalidValue", "PeerDiscoveryRequest", "Connector", 80 },
	{ "InvalidValue", "AuthenticationRequest", "InitNonce", 81 },
	{ "InvalidValue", "PeerDiscoveryRequest", "TransactionID", 82 },
	{ "InvalidValue", "ConfigurationRequest", "EnrolleeNonce", 83 },
	{ "Timeout", "PKEXExchangeResponse", NULL, 84 },
	{ "Timeout", "PKEXCRRequest", NULL, 85 },
	{ "Timeout", "PKEXCRResponse", NULL, 86 },
	{ "Timeout", "AuthenticationRequest", NULL, 87 },
	{ "Timeout", "AuthenticationResponse", NULL, 88 },
	{ "Timeout", "AuthenticationConfirm", NULL, 89 },
	{ "Timeout", "ConfigurationRequest", NULL, 90 },
	{ NULL, NULL, NULL, 0 }
};


static int dpp_get_test(const char *step, const char *frame, const char *attr)
{
	int i;

	for (i = 0; dpp_tests[i].step; i++) {
		if (strcasecmp(step, dpp_tests[i].step) == 0 &&
		    strcasecmp(frame, dpp_tests[i].frame) == 0 &&
		    ((!attr && dpp_tests[i].attr == NULL) ||
		     (attr && strcasecmp(attr, dpp_tests[i].attr) == 0)))
			return dpp_tests[i].value;
	}

	return -1;
}


static int dpp_wait_tx(struct sigma_dut *dut, struct wpa_ctrl *ctrl,
		       int frame_type)
{
	char buf[200], tmp[20];
	int res;

	snprintf(tmp, sizeof(tmp), "type=%d", frame_type);
	for (;;) {
		res = get_wpa_cli_event(dut, ctrl, "DPP-TX", buf, sizeof(buf));
		if (res < 0)
			return -1;
		if (strstr(buf, tmp) != NULL)
			break;
	}

	return 0;
}


static int dpp_wait_tx_status(struct sigma_dut *dut, struct wpa_ctrl *ctrl,
			      int frame_type)
{
	char buf[200], tmp[20];
	int res;

	snprintf(tmp, sizeof(tmp), "type=%d", frame_type);
	for (;;) {
		res = get_wpa_cli_event(dut, ctrl, "DPP-TX", buf, sizeof(buf));
		if (res < 0)
			return -1;
		if (strstr(buf, tmp) != NULL)
			break;
	}

	res = get_wpa_cli_event(dut, ctrl, "DPP-TX-STATUS",
				buf, sizeof(buf));
	if (res < 0 || strstr(buf, "result=FAILED") != NULL)
		return -1;

	return 0;
}


static int dpp_wait_rx(struct sigma_dut *dut, struct wpa_ctrl *ctrl,
		       int frame_type, unsigned int max_wait)
{
	char buf[200], tmp[20];
	int res;
	unsigned int old_timeout;

	old_timeout = dut->default_timeout;
	if (max_wait > 0 && dut->default_timeout > max_wait)
		dut->default_timeout = max_wait;

	snprintf(tmp, sizeof(tmp), "type=%d", frame_type);
	for (;;) {
		res = get_wpa_cli_event(dut, ctrl, "DPP-RX", buf, sizeof(buf));
		if (res < 0) {
			dut->default_timeout = old_timeout;
			return -1;
		}
		if (strstr(buf, tmp) != NULL)
			break;
	}

	dut->default_timeout = old_timeout;
	return 0;
}


static int dpp_wait_rx_conf_req(struct sigma_dut *dut, struct wpa_ctrl *ctrl,
				unsigned int max_wait)
{
	char buf[200];
	int res;
	unsigned int old_timeout;

	old_timeout = dut->default_timeout;
	if (max_wait > 0 && dut->default_timeout > max_wait)
		dut->default_timeout = max_wait;

	for (;;) {
		res = get_wpa_cli_event(dut, ctrl, "DPP-CONF-REQ-RX",
					buf, sizeof(buf));
		if (res < 0) {
			dut->default_timeout = old_timeout;
			return -1;
		}

		break;
	}

	dut->default_timeout = old_timeout;
	return 0;
}


static int dpp_scan_peer_qrcode(struct sigma_dut *dut)
{
#ifdef ANDROID
	char buf[100];
	char *buf2 = NULL;
	FILE *fp = NULL;
	uint32_t length;
	unsigned int count;

	unlink(dpp_qrcode_file);

	snprintf(buf, sizeof(buf),
		 "am start -n w1.fi.wpadebug/w1.fi.wpadebug.QrCodeReadActivity");
	if (system(buf) != 0) {
		sigma_dut_print(dut, DUT_MSG_ERROR,
				"Failed to launch QR Code scanner");
		return -1;
	}

	count = 0;
	while (!(fp = fopen(dpp_qrcode_file, "r"))) {
		if (count > dut->default_timeout) {
			sigma_dut_print(dut, DUT_MSG_ERROR,
					"Failed to open dpp_qrcode_file - QR Code scanning timed out");
			return -1;
		}

		sleep(1);
		count++;
	}

	if (fseek(fp, 0, SEEK_END) < 0 || (length = ftell(fp)) <= 0 ||
	    fseek(fp, 0, SEEK_SET) < 0) {
		sigma_dut_print(dut, DUT_MSG_ERROR,
				"Failed to get QR Code result file length");
		fclose(fp);
		return -1;
	}

	buf2 = malloc(length + 1);
	if (!buf2) {
		fclose(fp);
		return -1;
	}

	if (fread(buf2, 1, length, fp) != length) {
		fclose(fp);
		free(buf2);
		return -1;
	}

	fclose(fp);
	buf2[length] = '\0';

	free(dut->dpp_peer_uri);
	dut->dpp_peer_uri = strdup(buf2);
	free(buf2);
	return 0;
#else /* ANDROID */
	pid_t pid;
	int pid_status;
	int pipe_out[2];
	char buf[4000], *pos;
	ssize_t len;
	int res = -1, ret;
	struct timeval tv;
	fd_set rfd;

	if (pipe(pipe_out) != 0) {
		perror("pipe");
		return -1;
	}

	pid = fork();
	if (pid < 0) {
		perror("fork");
		close(pipe_out[0]);
		close(pipe_out[1]);
		return -1;
	}

	if (pid == 0) {
		char *argv[4] = { "zbarcam", "--raw", "--prescale=320x240",
				  NULL };

		dup2(pipe_out[1], STDOUT_FILENO);
		close(pipe_out[0]);
		close(pipe_out[1]);
		execv("/usr/bin/zbarcam", argv);
		perror("execv");
		exit(0);
		return -1;
	}

	close(pipe_out[1]);

	FD_ZERO(&rfd);
	FD_SET(pipe_out[0], &rfd);
	tv.tv_sec = dut->default_timeout;
	tv.tv_usec = 0;

	ret = select(pipe_out[0] + 1, &rfd, NULL, NULL, &tv);
	if (ret < 0) {
		perror("select");
		goto out;
	}
	if (ret == 0) {
		sigma_dut_print(dut, DUT_MSG_DEBUG,
				"QR Code scanning timed out");
		goto out;
	}

	len = read(pipe_out[0], buf, sizeof(buf));
	if (len <= 0)
		goto out;
	if (len == sizeof(buf))
		len--;
	buf[len] = '\0';
	pos = strchr(buf, '\n');
	if (pos)
		*pos = '\0';
	sigma_dut_print(dut, DUT_MSG_DEBUG, "URI from QR scanner: %s", buf);

	free(dut->dpp_peer_uri);
	dut->dpp_peer_uri = strdup(buf);
	res = 0;
out:
	close(pipe_out[0]);
	kill(pid, SIGTERM);
	waitpid(pid, &pid_status, 0);

	return res;
#endif /* ANDROID */
}


static int dpp_display_own_qrcode(struct sigma_dut *dut)
{
	char buf[200], resp[2000];
	const char *ifname = get_station_ifname();
#ifdef ANDROID
	FILE *fp;
#else /* ANDROID */
	pid_t pid;
	int pid_status;
#endif /* ANDROID */

	snprintf(buf, sizeof(buf), "DPP_BOOTSTRAP_GET_URI %d",
		 dut->dpp_local_bootstrap);
	if (wpa_command_resp(ifname, buf, resp, sizeof(resp)) < 0 ||
	    strncmp(resp, "FAIL", 4) == 0)
		return -2;
	sigma_dut_print(dut, DUT_MSG_DEBUG, "Own bootstrap URI: %s", resp);

#ifdef ANDROID
	unlink(dpp_qrcode_file);

	fp = fopen(dpp_qrcode_file, "w");
	if (!fp) {
		sigma_dut_print(dut, DUT_MSG_ERROR, "Failed to open file %s",
				dpp_qrcode_file);
		return -2;
	}

	fwrite(resp, 1, strlen(resp), fp);
	fclose(fp);

	snprintf(buf, sizeof(buf),
		 "am start -n w1.fi.wpadebug/w1.fi.wpadebug.QrCodeDisplayActivity");
	if (system(buf) != 0) {
		sigma_dut_print(dut, DUT_MSG_ERROR, "Failed to display QR Code");
		return -1;
	}
#else /* ANDROID */
	pid = fork();
	if (pid < 0) {
		perror("fork");
		return -1;
	}

	if (pid == 0) {
		char *argv[3] = { "qr", resp, NULL };

		execv("/usr/bin/qr", argv);
		perror("execv");
		exit(0);
		return -1;
	}

	waitpid(pid, &pid_status, 0);
#endif /* ANDROID */

	return 0;
}


static int dpp_process_auth_response(struct sigma_dut *dut,
				     struct sigma_conn *conn,
				     struct wpa_ctrl *ctrl,
				     const char **auth_events,
				     const char *action_type,
				     int check_mutual, char *buf, size_t buflen)
{
	int res;

	res = get_wpa_cli_events(dut, ctrl, auth_events, buf, buflen);
	if (res < 0) {
		send_resp(dut, conn, SIGMA_COMPLETE,
			  "BootstrapResult,OK,AuthResult,Timeout");
		return res;
	}
	sigma_dut_print(dut, DUT_MSG_DEBUG, "DPP auth result: %s", buf);

	if (strstr(buf, "DPP-RESPONSE-PENDING")) {
		/* Display own QR code in manual mode */
		if (action_type && strcasecmp(action_type, "ManualDPP") == 0 &&
		    dpp_display_own_qrcode(dut) < 0) {
			send_resp(dut, conn, SIGMA_ERROR,
				  "errorCode,Failed to display own QR code");
			return -1;
		}

		/* Wait for the actual result after the peer has scanned the
		 * QR Code. */
		res = get_wpa_cli_events(dut, ctrl, auth_events,
					 buf, buflen);
		if (res < 0) {
			send_resp(dut, conn, SIGMA_COMPLETE,
				  "BootstrapResult,OK,AuthResult,Timeout");
			return res;
		}

		sigma_dut_print(dut, DUT_MSG_DEBUG, "DPP auth result: %s", buf);
	}

	if (check_mutual) {
		if (strstr(buf, "DPP-NOT-COMPATIBLE")) {
			send_resp(dut, conn, SIGMA_COMPLETE,
				  "BootstrapResult,OK,AuthResult,ROLES_NOT_COMPATIBLE");
			return -1;
		}

		if (!strstr(buf, "DPP-AUTH-DIRECTION")) {
			send_resp(dut, conn, SIGMA_ERROR,
				  "errorCode,No event for auth direction seen");
			return -1;
		}

		sigma_dut_print(dut, DUT_MSG_DEBUG, "DPP auth direction: %s",
				buf);
		if (strstr(buf, "mutual=1") == NULL) {
			send_resp(dut, conn, SIGMA_ERROR,
				  "errorCode,Peer did not use mutual authentication");
			return -1;
		}
	}

	return 0;
}


static int dpp_automatic_dpp(struct sigma_dut *dut,
			     struct sigma_conn *conn,
			     struct sigma_cmd *cmd)
{
	const char *bs = get_param(cmd, "DPPBS");
	const char *auth_role = get_param(cmd, "DPPAuthRole");
	const char *prov_role = get_param(cmd, "DPPProvisioningRole");
	const char *pkex_code = get_param(cmd, "DPPPKEXCode");
	const char *pkex_code_id = get_param(cmd, "DPPPKEXCodeIdentifier");
	const char *wait_conn = get_param(cmd, "DPPWaitForConnect");
	const char *self_conf = get_param(cmd, "DPPSelfConfigure");
	const char *step = get_param(cmd, "DPPStep");
	const char *frametype = get_param(cmd, "DPPFrameType");
	const char *attr = get_param(cmd, "DPPIEAttribute");
	const char *action_type = get_param(cmd, "DPPActionType");
	const char *role;
	const char *val;
	const char *conf_role;
	int conf_index = -1;
	char buf[2000];
	char conf_ssid[100];
	char conf_pass[100];
	char pkex_identifier[200];
	struct wpa_ctrl *ctrl;
	int res;
	unsigned int old_timeout;
	int own_pkex_id = -1;
	const char *ifname = get_station_ifname();
	const char *auth_events[] = {
		"DPP-AUTH-SUCCESS",
		"DPP-NOT-COMPATIBLE",
		"DPP-RESPONSE-PENDING",
		"DPP-SCAN-PEER-QR-CODE",
		"DPP-AUTH-DIRECTION",
		NULL
	};
	const char *conf_events[] = {
		"DPP-CONF-RECEIVED",
		"DPP-CONF-SENT",
		"DPP-CONF-FAILED",
		NULL
	};
	const char *conn_events[] = {
		"PMKSA-CACHE-ADDED",
		"CTRL-EVENT-CONNECTED",
		NULL
	};
	const char *group_id_str = NULL;
	char group_id[100];
	const char *result;
	int check_mutual = 0;
	int enrollee_ap;
	int force_gas_fragm = 0;
	int not_dpp_akm = 0;

	if (!wait_conn)
		wait_conn = "no";
	if (!self_conf)
		self_conf = "no";

	if (!auth_role) {
		send_resp(dut, conn, SIGMA_ERROR,
			  "errorCode,Missing DPPAuthRole");
		return 0;
	}

	if (!prov_role) {
		send_resp(dut, conn, SIGMA_ERROR,
			  "errorCode,Missing DPPProvisioningRole");
		return 0;
	}

	val = get_param(cmd, "DPPConfEnrolleeRole");
	if (val)
		enrollee_ap = strcasecmp(val, "AP") == 0;
	else
		enrollee_ap = sigma_dut_is_ap(dut);

	if ((step || frametype) && (!step || !frametype)) {
		send_resp(dut, conn, SIGMA_ERROR,
			  "errorCode,Invalid DPPStep,DPPFrameType,DPPIEAttribute combination");
		return 0;
	}

	if (sigma_dut_is_ap(dut)) {
		if (!dut->hostapd_ifname) {
			sigma_dut_print(dut, DUT_MSG_ERROR,
					"hostapd ifname not specified (-j)");
			return -2;
		}
		ifname = dut->hostapd_ifname;

		if (dpp_hostapd_run(dut) < 0) {
			send_resp(dut, conn, SIGMA_ERROR,
				  "errorCode,Failed to start hostapd");
			return 0;
		}
	}

	if (strcasecmp(prov_role, "Configurator") == 0 ||
	    strcasecmp(prov_role, "Both") == 0) {
		if (dut->dpp_conf_id < 0) {
			snprintf(buf, sizeof(buf),
				 "DPP_CONFIGURATOR_ADD curve=%s",
				 dpp_get_curve(cmd, "DPPSigningKeyECC"));
			if (wpa_command_resp(ifname, buf,
					     buf, sizeof(buf)) < 0) {
				send_resp(dut, conn, SIGMA_ERROR,
					  "errorCode,Failed to set up configurator");
				return 0;
			}
			dut->dpp_conf_id = atoi(buf);
		}
		if (strcasecmp(prov_role, "Configurator") == 0)
			role = "configurator";
		else
			role = "either";
	} else if (strcasecmp(prov_role, "Enrollee") == 0) {
		role = "enrollee";
	} else {
		send_resp(dut, conn, SIGMA_ERROR,
			  "errorCode,Unknown DPPProvisioningRole");
		return 0;
	}

	pkex_identifier[0] = '\0';
	if (strcasecmp(bs, "PKEX") == 0) {
		if (sigma_dut_is_ap(dut) && dut->ap_channel != 6) {
			/* For now, have to make operating channel match DPP
			 * listen channel. This should be removed once hostapd
			 * has support for DPP listen on non-operating channel.
			 */
			sigma_dut_print(dut, DUT_MSG_INFO,
					"Update hostapd operating channel to match listen needs");
			dut->ap_channel = 6;

			if (get_driver_type() == DRIVER_OPENWRT) {
				snprintf(buf, sizeof(buf),
					 "iwconfig %s channel %d",
					 dut->hostapd_ifname, dut->ap_channel);
				run_system(dut, buf);
			}

			if (wpa_command(ifname, "SET channel 6") < 0 ||
			    wpa_command(ifname, "DISABLE") < 0 ||
			    wpa_command(ifname, "ENABLE") < 0) {
				send_resp(dut, conn, SIGMA_ERROR,
					  "errorCode,Failed to update channel");
				return 0;
			}
		}

		if (!pkex_code) {
			send_resp(dut, conn, SIGMA_ERROR,
				  "errorCode,Missing DPPPKEXCode");
			return 0;
		}

		if (pkex_code_id)
			snprintf(pkex_identifier, sizeof(pkex_identifier),
				 "identifier=%s ", pkex_code_id);

		snprintf(buf, sizeof(buf),
			 "DPP_BOOTSTRAP_GEN type=pkex curve=%s",
			 dpp_get_curve(cmd, "DPPCryptoIdentifier"));
		if (wpa_command_resp(ifname, buf, buf, sizeof(buf)) < 0) {
			send_resp(dut, conn, SIGMA_ERROR,
				  "errorCode,Failed to set up PKEX");
			return 0;
		}
		own_pkex_id = atoi(buf);
	}

	ctrl = open_wpa_mon(ifname);
	if (!ctrl) {
		sigma_dut_print(dut, DUT_MSG_ERROR,
				"Failed to open wpa_supplicant monitor connection");
		return -2;
	}

	old_timeout = dut->default_timeout;
	val = get_param(cmd, "DPPTimeout");
	if (val && atoi(val) > 0) {
		dut->default_timeout = atoi(val);
		sigma_dut_print(dut, DUT_MSG_DEBUG, "DPP timeout: %u",
				dut->default_timeout);
	}

	conf_ssid[0] = '\0';
	conf_pass[0] = '\0';
	group_id[0] = '\0';
	val = get_param(cmd, "DPPConfIndex");
	if (val)
		conf_index = atoi(val);
	switch (conf_index) {
	case -1:
		conf_role = NULL;
		break;
	case 1:
		ascii2hexstr("DPPNET01", buf);
		res = snprintf(conf_ssid, sizeof(conf_ssid), "ssid=%s", buf);
		if (res < 0 || res >= sizeof(conf_ssid))
			goto err;
		if (enrollee_ap) {
			conf_role = "ap-dpp";
		} else {
			conf_role = "sta-dpp";
		}
		group_id_str = "DPPGROUP_DPP_INFRA";
		break;
	case 2:
		ascii2hexstr("DPPNET01", buf);
		res = snprintf(conf_ssid, sizeof(conf_ssid), "ssid=%s", buf);
		if (res < 0 || res >= sizeof(conf_ssid))
			goto err;
		snprintf(conf_pass, sizeof(conf_pass),
			 "psk=10506e102ad1e7f95112f6b127675bb8344dacacea60403f3fa4055aec85b0fc");
		if (enrollee_ap)
			conf_role = "ap-psk";
		else
			conf_role = "sta-psk";
		break;
	case 3:
		ascii2hexstr("DPPNET01", buf);
		res = snprintf(conf_ssid, sizeof(conf_ssid), "ssid=%s", buf);
		if (res < 0 || res >= sizeof(conf_ssid))
			goto err;
		ascii2hexstr("ThisIsDppPassphrase", buf);
		res = snprintf(conf_pass, sizeof(conf_pass), "pass=%s", buf);
		if (res < 0 || res >= sizeof(conf_pass))
			goto err;
		if (enrollee_ap)
			conf_role = "ap-psk";
		else
			conf_role = "sta-psk";
		break;
	case 4:
		ascii2hexstr("DPPNET01", buf);
		res = snprintf(conf_ssid, sizeof(conf_ssid), "ssid=%s", buf);
		if (res < 0 || res >= sizeof(conf_ssid))
			goto err;
		if (enrollee_ap) {
			conf_role = "ap-dpp";
		} else {
			conf_role = "sta-dpp";
		}
		group_id_str = "DPPGROUP_DPP_INFRA2";
		break;
	case 5:
		ascii2hexstr("DPPNET01", buf);
		res = snprintf(conf_ssid, sizeof(conf_ssid), "ssid=%s", buf);
		if (res < 0 || res >= sizeof(conf_ssid))
			goto err;
		ascii2hexstr("ThisIsDppPassphrase", buf);
		res = snprintf(conf_pass, sizeof(conf_pass), "pass=%s", buf);
		if (res < 0 || res >= sizeof(conf_pass))
			goto err;
		if (enrollee_ap)
			conf_role = "ap-sae";
		else
			conf_role = "sta-sae";
		break;
	case 6:
		ascii2hexstr("DPPNET01", buf);
		res = snprintf(conf_ssid, sizeof(conf_ssid), "ssid=%s", buf);
		if (res < 0 || res >= sizeof(conf_ssid))
			goto err;
		ascii2hexstr("ThisIsDppPassphrase", buf);
		res = snprintf(conf_pass, sizeof(conf_pass), "pass=%s", buf);
		if (res < 0 || res >= sizeof(conf_pass))
			goto err;
		if (enrollee_ap)
			conf_role = "ap-psk-sae";
		else
			conf_role = "sta-psk-sae";
		break;
	case 7:
		ascii2hexstr("DPPNET01", buf);
		res = snprintf(conf_ssid, sizeof(conf_ssid), "ssid=%s", buf);
		if (res < 0 || res >= sizeof(conf_ssid))
			goto err;
		if (enrollee_ap) {
			conf_role = "ap-dpp";
		} else {
			conf_role = "sta-dpp";
		}
		group_id_str = "DPPGROUP_DPP_INFRA";
		force_gas_fragm = 1;
		break;
	case 8:
		ascii2hexstr("DPPNET01", buf);
		res = snprintf(conf_ssid, sizeof(conf_ssid), "ssid=%s", buf);
		if (res < 0 || res >= sizeof(conf_ssid))
			goto err;
		ascii2hexstr("This_is_legacy_password", buf);
		res = snprintf(conf_pass, sizeof(conf_pass), "pass=%s", buf);
		if (res < 0 || res >= sizeof(conf_pass))
			goto err;
		if (enrollee_ap) {
			conf_role = "ap-dpp+psk+sae";
		} else {
			conf_role = "sta-dpp+psk+sae";
		}
		group_id_str = "DPPGROUP_DPP_INFRA1";
		break;
	default:
		send_resp(dut, conn, SIGMA_ERROR,
			  "errorCode,Unsupported DPPConfIndex");
		goto out;
	}

	if (group_id_str)
		snprintf(group_id, sizeof(group_id), " group_id=%s",
			 group_id_str);

	if (force_gas_fragm) {
		char spaces[1500];

		memset(spaces, ' ', sizeof(spaces));
		spaces[sizeof(spaces) - 1] = '\0';

		snprintf(buf, sizeof(buf),
			 "SET dpp_discovery_override {\"ssid\":\"DPPNET01\"}%s",
			 spaces);
		if (wpa_command(ifname, buf) < 0) {
			send_resp(dut, conn, SIGMA_ERROR,
				  "errorCode,Failed to set discovery override");
			goto out;
		}
	}

	if (step) {
		int test;

		test = dpp_get_test(step, frametype, attr);
		if (test <= 0) {
			send_resp(dut, conn, SIGMA_ERROR,
				  "errorCode,Unsupported DPPStep/DPPFrameType/DPPIEAttribute");
			goto out;
		}

		snprintf(buf, sizeof(buf), "SET dpp_test %d", test);
		if (wpa_command(ifname, buf) < 0) {
			send_resp(dut, conn, SIGMA_ERROR,
				  "errorCode,Failed to set dpp_test");
			goto out;
		}
	} else {
		wpa_command(ifname, "SET dpp_test 0");
	}

	if (strcasecmp(self_conf, "Yes") == 0) {
		if (strcasecmp(prov_role, "Configurator") != 0) {
			send_resp(dut, conn, SIGMA_ERROR,
				  "errorCode,Invalid DPPSelfConfigure use - only allowed for Configurator role");
			goto out;
		}
		if (!conf_role) {
			send_resp(dut, conn, SIGMA_ERROR,
				  "errorCode,Missing DPPConfIndex");
			goto out;
		}

		snprintf(buf, sizeof(buf),
			 "DPP_CONFIGURATOR_SIGN  conf=%s %s %s configurator=%d",
			 conf_role, conf_ssid, conf_pass, dut->dpp_conf_id);
		if (wpa_command(ifname, buf) < 0) {
			send_resp(dut, conn, SIGMA_ERROR,
				  "errorCode,Failed to initiate DPP self-configuration");
			goto out;
		}
		if (sigma_dut_is_ap(dut))
			goto update_ap;
		goto wait_connect;
	} else if (strcasecmp(auth_role, "Initiator") == 0) {
		char own_txt[20];
		int dpp_peer_bootstrap = -1;
		char neg_freq[30];

		val = get_param(cmd, "DPPAuthDirection");
		check_mutual = val && strcasecmp(val, "Mutual") == 0;

		neg_freq[0] = '\0';
		val = get_param(cmd, "DPPSubsequentChannel");
		if (val) {
			int opclass, channel, freq;

			opclass = atoi(val);
			val = strchr(val, '/');
			if (opclass == 0 || !val) {
				send_resp(dut, conn, SIGMA_ERROR,
					  "errorCode,Invalid DPPSubsequentChannel");
				goto out;
			}
			val++;
			channel = atoi(val);

			/* Ignoring opclass for now; could use it here for more
			 * robust frequency determination. */
			freq = channel_to_freq(dut, channel);
			if (!freq) {
				send_resp(dut, conn, SIGMA_ERROR,
					  "errorCode,Unsupported DPPSubsequentChannel channel");
				goto out;
			}
			snprintf(neg_freq, sizeof(neg_freq), " neg_freq=%d",
				 freq);
		}

		if (strcasecmp(bs, "QR") == 0) {
			if (!dut->dpp_peer_uri) {
				send_resp(dut, conn, SIGMA_ERROR,
					  "errorCode,Missing peer bootstrapping info");
				goto out;
			}

			snprintf(buf, sizeof(buf), "DPP_QR_CODE %s",
				 dut->dpp_peer_uri);
			if (wpa_command_resp(ifname, buf, buf,
					     sizeof(buf)) < 0 ||
			    strncmp(buf, "FAIL", 4) == 0) {
				send_resp(dut, conn, SIGMA_ERROR,
					  "errorCode,Failed to parse URI");
				goto out;
			}
			dpp_peer_bootstrap = atoi(buf);
		}

		if (dut->dpp_local_bootstrap >= 0)
			snprintf(own_txt, sizeof(own_txt), " own=%d",
				 dut->dpp_local_bootstrap);
		else
			own_txt[0] = '\0';
		if (strcasecmp(bs, "QR") == 0 &&
		    (strcasecmp(prov_role, "Configurator") == 0 ||
		     strcasecmp(prov_role, "Both") == 0)) {
			if (!conf_role) {
				send_resp(dut, conn, SIGMA_ERROR,
					  "errorCode,Missing DPPConfIndex");
				goto out;
			}
			snprintf(buf, sizeof(buf),
				 "DPP_AUTH_INIT peer=%d%s role=%s conf=%s %s %s configurator=%d%s%s",
				 dpp_peer_bootstrap, own_txt, role,
				 conf_role, conf_ssid, conf_pass,
				 dut->dpp_conf_id, neg_freq, group_id);
		} else if (strcasecmp(bs, "QR") == 0) {
			snprintf(buf, sizeof(buf),
				 "DPP_AUTH_INIT peer=%d%s role=%s%s%s",
				 dpp_peer_bootstrap, own_txt, role,
				 neg_freq, group_id);
		} else if (strcasecmp(bs, "PKEX") == 0 &&
			   (strcasecmp(prov_role, "Configurator") == 0 ||
			    strcasecmp(prov_role, "Both") == 0)) {
			if (!conf_role) {
				send_resp(dut, conn, SIGMA_ERROR,
					  "errorCode,Missing DPPConfIndex");
				goto out;
			}
			snprintf(buf, sizeof(buf),
				 "DPP_PKEX_ADD own=%d init=1 role=%s conf=%s %s %s configurator=%d %scode=%s",
				 own_pkex_id, role, conf_role,
				 conf_ssid, conf_pass, dut->dpp_conf_id,
				 pkex_identifier, pkex_code);
		} else if (strcasecmp(bs, "PKEX") == 0) {
			snprintf(buf, sizeof(buf),
				 "DPP_PKEX_ADD own=%d init=1 role=%s %scode=%s",
				 own_pkex_id, role, pkex_identifier, pkex_code);
		} else {
			send_resp(dut, conn, SIGMA_ERROR,
				  "errorCode,Unsupported DPPBS");
			goto out;
		}
		if (wpa_command(ifname, buf) < 0) {
			send_resp(dut, conn, SIGMA_ERROR,
				  "errorCode,Failed to initiate DPP authentication");
			goto out;
		}
	} else if (strcasecmp(auth_role, "Responder") == 0) {
		const char *delay_qr_resp;
		int mutual;
		int freq = 2462; /* default: channel 11 */

		if (strcasecmp(bs, "PKEX") == 0) {
			/* default: channel 6 for PKEX */
			freq = 2437;
		}

		delay_qr_resp = get_param(cmd, "DPPDelayQRResponse");

		val = get_param(cmd, "DPPAuthDirection");
		mutual = val && strcasecmp(val, "Mutual") == 0;

		val = get_param(cmd, "DPPListenChannel");
		if (val) {
			freq = channel_to_freq(dut, atoi(val));
			if (freq == 0) {
				send_resp(dut, conn, SIGMA_ERROR,
					  "errorCode,Unsupported DPPListenChannel value");
				goto out;
			}
		}

		if (!delay_qr_resp && dut->dpp_peer_uri) {
			snprintf(buf, sizeof(buf), "DPP_QR_CODE %s",
				 dut->dpp_peer_uri);
			if (wpa_command_resp(ifname, buf, buf,
					     sizeof(buf)) < 0) {
				send_resp(dut, conn, SIGMA_ERROR,
					  "errorCode,Failed to parse URI");
				goto out;
			}
		}

		if (strcasecmp(prov_role, "Configurator") == 0) {
			if (!conf_role) {
				send_resp(dut, conn, SIGMA_ERROR,
					  "errorCode,Missing DPPConfIndex");
				goto out;
			}
			snprintf(buf, sizeof(buf),
				 "SET dpp_configurator_params  conf=%s %s %s configurator=%d%s",
				 conf_role, conf_ssid, conf_pass,
				 dut->dpp_conf_id, group_id);
			if (wpa_command(ifname, buf) < 0) {
				send_resp(dut, conn, SIGMA_ERROR,
					  "errorCode,Failed to set configurator parameters");
				goto out;
			}
		}
		if (strcasecmp(bs, "PKEX") == 0) {
			snprintf(buf, sizeof(buf),
				 "DPP_PKEX_ADD own=%d role=%s %scode=%s",
				 own_pkex_id, role, pkex_identifier, pkex_code);
			if (wpa_command(ifname, buf) < 0) {
				send_resp(dut, conn, SIGMA_ERROR,
					  "errorCode,Failed to configure DPP PKEX");
				goto out;
			}
		}

		snprintf(buf, sizeof(buf), "DPP_LISTEN %d role=%s%s",
			 freq, role,
			 (strcasecmp(bs, "QR") == 0 && mutual) ?
			 " qr=mutual" : "");
		if (wpa_command(ifname, buf) < 0) {
			send_resp(dut, conn, SIGMA_ERROR,
				  "errorCode,Failed to start DPP listen");
			goto out;
		}

		if (get_driver_type() == DRIVER_OPENWRT) {
			snprintf(buf, sizeof(buf), "iwconfig %s channel %d",
				 dut->hostapd_ifname, freq_to_channel(freq));
			run_system(dut, buf);
		}

		if (delay_qr_resp && mutual && dut->dpp_peer_uri) {
			int wait_time = atoi(delay_qr_resp);

			res = get_wpa_cli_events(dut, ctrl, auth_events,
						 buf, sizeof(buf));
			if (res < 0) {
				send_resp(dut, conn, SIGMA_COMPLETE,
					  "BootstrapResult,OK,AuthResult,Timeout");
				goto out;
			}
			sigma_dut_print(dut, DUT_MSG_DEBUG,
					"DPP auth result: %s", buf);
			if (strstr(buf, "DPP-SCAN-PEER-QR-CODE") == NULL) {
				send_resp(dut, conn, SIGMA_ERROR,
					  "errorCode,No scan request for peer QR Code seen");
				goto out;
			}
			sigma_dut_print(dut, DUT_MSG_INFO,
					"Waiting %d second(s) before processing peer URI",
					wait_time);
			sleep(wait_time);

			snprintf(buf, sizeof(buf), "DPP_QR_CODE %s",
				 dut->dpp_peer_uri);
			if (wpa_command_resp(ifname, buf, buf,
					     sizeof(buf)) < 0) {
				send_resp(dut, conn, SIGMA_ERROR,
					  "errorCode,Failed to parse URI");
				goto out;
			}
		} else if (mutual && action_type &&
			   strcasecmp(action_type, "ManualDPP") == 0) {
			res = get_wpa_cli_events(dut, ctrl, auth_events,
						 buf, sizeof(buf));
			if (res < 0) {
				send_resp(dut, conn, SIGMA_COMPLETE,
					  "BootstrapResult,OK,AuthResult,Timeout");
				goto out;
			}
			sigma_dut_print(dut, DUT_MSG_DEBUG,
					"DPP auth result: %s", buf);
			if (strstr(buf, "DPP-NOT-COMPATIBLE")) {
			    send_resp(dut, conn, SIGMA_COMPLETE,
				      "BootstrapResult,OK,AuthResult,ROLES_NOT_COMPATIBLE");
			    goto out;
			}

			if (strstr(buf, "DPP-SCAN-PEER-QR-CODE") == NULL) {
				send_resp(dut, conn, SIGMA_ERROR,
					  "errorCode,No scan request for peer QR Code seen");
				goto out;
			}

			if (dpp_scan_peer_qrcode(dut) < 0) {
				send_resp(dut, conn, SIGMA_ERROR,
					  "errorCode,Failed to scan peer QR Code");
				goto out;
			}

			snprintf(buf, sizeof(buf), "DPP_QR_CODE %s",
				 dut->dpp_peer_uri);
			if (wpa_command_resp(ifname, buf, buf,
					     sizeof(buf)) < 0) {
				send_resp(dut, conn, SIGMA_ERROR,
					  "errorCode,Failed to parse URI");
				goto out;
			}
		}
	} else {
		send_resp(dut, conn, SIGMA_ERROR,
			  "errorCode,Unknown DPPAuthRole");
		goto out;
	}

	if (step && strcasecmp(step, "Timeout") == 0) {
		result = "errorCode,Unexpected state";

		if (strcasecmp(frametype, "PKEXExchangeResponse") == 0) {
			if (dpp_wait_rx(dut, ctrl, 8, -1) < 0)
				result = "BootstrapResult,Timeout";
			else
				result = "BootstrapResult,Errorsent";
		}

		if (strcasecmp(frametype, "PKEXCRRequest") == 0) {
			if (dpp_wait_rx(dut, ctrl, 9, -1) < 0)
				result = "BootstrapResult,Timeout";
			else
				result = "BootstrapResult,Errorsent";
		}

		if (strcasecmp(frametype, "PKEXCRResponse") == 0) {
			if (dpp_wait_rx(dut, ctrl, 10, -1) < 0)
				result = "BootstrapResult,Timeout";
			else
				result = "BootstrapResult,Errorsent";
		}

		if (strcasecmp(frametype, "AuthenticationRequest") == 0) {
			if (dpp_wait_rx(dut, ctrl, 0, -1) < 0)
				result = "BootstrapResult,OK,AuthResult,Timeout";
			else
				result = "BootstrapResult,OK,AuthResult,Errorsent";
		}

		if (strcasecmp(frametype, "AuthenticationResponse") == 0) {
			if (dpp_wait_rx(dut, ctrl, 1, -1) < 0)
				result = "BootstrapResult,OK,AuthResult,Timeout";
			else
				result = "BootstrapResult,OK,AuthResult,Errorsent";
		}

		if (strcasecmp(frametype, "AuthenticationConfirm") == 0) {
			if (strcasecmp(auth_role, "Initiator") == 0) {
				/* This special case of DPPStep,Timeout with
				 * DPPFrameType,AuthenticationConfirm on an
				 * Initiator is used to cover need for stopping
				 * the Initiator/Enrollee from sending out
				 * Configuration Request message. */
				if (strcasecmp(prov_role, "Enrollee") != 0) {
					send_resp(dut, conn, SIGMA_ERROR,
						  "errorCode,Unexpected use of timeout after AuthenticationConfirm TX in Configurator role");
					goto out;
				}
				if (check_mutual &&
				    dpp_process_auth_response(
					    dut, conn, ctrl, auth_events,
					    action_type, check_mutual,
					    buf, sizeof(buf)) < 0)
					goto out;
				if (dpp_wait_tx_status(dut, ctrl, 2) < 0)
					result = "BootstrapResult,OK,AuthResult,Timeout";
				else
					result = "BootstrapResult,OK,AuthResult,Errorsent,LastFrameReceived,AuthenticationResponse";
			} else {
				if (dpp_wait_rx(dut, ctrl, 2, -1) < 0)
					result = "BootstrapResult,OK,AuthResult,Timeout";
				else
					result = "BootstrapResult,OK,AuthResult,Errorsent,LastFrameReceived,AuthenticationConfirm";
			}
		}

		if (strcasecmp(frametype, "ConfigurationRequest") == 0) {
			if (get_wpa_cli_event(dut, ctrl, "DPP-CONF-FAILED",
					      buf, sizeof(buf)) < 0)
				result = "BootstrapResult,OK,AuthResult,OK,ConfResult,Timeout";
			else
				result = "BootstrapResult,OK,AuthResult,OK,ConfResult,Errorsent";
		}

		send_resp(dut, conn, SIGMA_COMPLETE, result);
		goto out;
	}

	if (frametype && strcasecmp(frametype, "PKEXExchangeRequest") == 0) {
		if (dpp_wait_tx_status(dut, ctrl, 7) < 0)
			result = "BootstrapResult,Timeout";
		else
			result = "BootstrapResult,Errorsent";
		send_resp(dut, conn, SIGMA_COMPLETE, result);
		goto out;
	}

	if (frametype && strcasecmp(frametype, "PKEXExchangeResponse") == 0) {
		if (dpp_wait_tx_status(dut, ctrl, 8) < 0)
			result = "BootstrapResult,Timeout";
		else
			result = "BootstrapResult,Errorsent";
		send_resp(dut, conn, SIGMA_COMPLETE, result);
		goto out;
	}

	if (frametype && strcasecmp(frametype, "PKEXCRRequest") == 0) {
		if (dpp_wait_tx_status(dut, ctrl, 9) < 0)
			result = "BootstrapResult,Timeout";
		else
			result = "BootstrapResult,Errorsent";
		send_resp(dut, conn, SIGMA_COMPLETE, result);
		goto out;
	}

	if (frametype && strcasecmp(frametype, "PKEXCRResponse") == 0) {
		if (dpp_wait_tx_status(dut, ctrl, 10) < 0)
			result = "BootstrapResult,Timeout";
		else
			result = "BootstrapResult,Errorsent";
		send_resp(dut, conn, SIGMA_COMPLETE, result);
		goto out;
	}

	if (!frametype && strcasecmp(bs, "PKEX") == 0 &&
	    strcasecmp(auth_role, "Responder") == 0) {
		if (dpp_wait_tx_status(dut, ctrl, 10) < 0) {
			send_resp(dut, conn, SIGMA_COMPLETE,
				  "BootstrapResult,Timeout");
			goto out;
		}
	}

	if (!frametype && strcasecmp(bs, "PKEX") == 0 &&
	    strcasecmp(auth_role, "Initiator") == 0) {
		if (dpp_wait_tx(dut, ctrl, 0) < 0) {
			send_resp(dut, conn, SIGMA_COMPLETE,
				  "BootstrapResult,Timeout");
			goto out;
		}
	}

	if (frametype && strcasecmp(frametype, "AuthenticationRequest") == 0) {
		if (dpp_wait_tx_status(dut, ctrl, 0) < 0) {
			send_resp(dut, conn, SIGMA_COMPLETE,
				  "BootstrapResult,OK,AuthResult,Timeout");
			goto out;
		}

		if (dpp_wait_rx(dut, ctrl, 1, 5) < 0)
			result = "BootstrapResult,OK,AuthResult,Errorsent,LastFrameReceived,None";
		else if	(get_wpa_cli_events(dut, ctrl, auth_events,
					    buf, sizeof(buf)) >= 0 &&
			 strstr(buf, "DPP-RESPONSE-PENDING") != NULL)
			result = "BootstrapResult,OK,AuthResult,Errorsent,LastFrameReceived,AuthenticationResponseWithStatusPending";
		else
			result = "BootstrapResult,OK,AuthResult,Errorsent,LastFrameReceived,AuthenticationResponse";
		send_resp(dut, conn, SIGMA_COMPLETE, result);
		goto out;
	}

	if (frametype && strcasecmp(frametype, "AuthenticationResponse") == 0) {
		if (dpp_wait_tx_status(dut, ctrl, 1) < 0) {
			send_resp(dut, conn, SIGMA_COMPLETE,
				  "BootstrapResult,OK,AuthResult,Timeout");
			goto out;
		}

		if (dpp_wait_rx(dut, ctrl, 2, 5) < 0)
			result = "BootstrapResult,OK,AuthResult,Errorsent,LastFrameReceived,AuthenticationRequest";
		else
			result = "BootstrapResult,OK,AuthResult,Errorsent,LastFrameReceived,AuthenticationConfirm";
		send_resp(dut, conn, SIGMA_COMPLETE, result);
		goto out;
	}

	if (dpp_process_auth_response(dut, conn, ctrl, auth_events, action_type,
				      check_mutual, buf, sizeof(buf)) < 0)
		goto out;

	if (frametype && strcasecmp(frametype, "AuthenticationConfirm") == 0) {
		if (dpp_wait_tx_status(dut, ctrl, 2) < 0) {
			send_resp(dut, conn, SIGMA_COMPLETE,
				  "BootstrapResult,OK,AuthResult,Timeout");
			goto out;
		}

		if (dpp_wait_rx_conf_req(dut, ctrl, 5) < 0)
			result = "BootstrapResult,OK,AuthResult,Errorsent,LastFrameReceived,AuthenticationResponse";
		else
			result = "BootstrapResult,OK,AuthResult,Errorsent,LastFrameReceived,ConfigurationRequest";
		send_resp(dut, conn, SIGMA_COMPLETE, result);
		goto out;
	}

	if (strstr(buf, "DPP-AUTH-DIRECTION")) {
		res = get_wpa_cli_events(dut, ctrl, auth_events,
					 buf, sizeof(buf));
		if (res < 0) {
			send_resp(dut, conn, SIGMA_COMPLETE,
				  "BootstrapResult,OK,AuthResult,Timeout");
			goto out;
		}

		sigma_dut_print(dut, DUT_MSG_DEBUG, "DPP auth result: %s", buf);
	}

	if (strstr(buf, "DPP-NOT-COMPATIBLE")) {
		send_resp(dut, conn, SIGMA_COMPLETE,
			  "BootstrapResult,OK,AuthResult,ROLES_NOT_COMPATIBLE");
		goto out;
	}

	if (!strstr(buf, "DPP-AUTH-SUCCESS")) {
		send_resp(dut, conn, SIGMA_COMPLETE,
			  "BootstrapResult,OK,AuthResult,FAILED");
		goto out;
	}

	if (frametype && strcasecmp(frametype, "ConfigurationRequest") == 0) {
		res = get_wpa_cli_event(dut, ctrl, "GAS-QUERY-DONE",
					buf, sizeof(buf));
		if (res < 0)
			result = "BootstrapResult,OK,AuthResult,OK,ConfResult,Timeout";
		else
			result = "BootstrapResult,OK,AuthResult,OK,ConfResult,Errorsent";
		send_resp(dut, conn, SIGMA_COMPLETE, result);
		goto out;
	}

	if (frametype && strcasecmp(frametype, "ConfigurationResponse") == 0) {
		res = get_wpa_cli_events(dut, ctrl, conf_events,
					 buf, sizeof(buf));
		if (res < 0)
			result = "BootstrapResult,OK,AuthResult,OK,ConfResult,Timeout";
		else
			result = "BootstrapResult,OK,AuthResult,OK,ConfResult,Errorsent,LastFrameReceived,ConfigurationRequest";
		send_resp(dut, conn, SIGMA_COMPLETE, result);
		goto out;
	}

	res = get_wpa_cli_events(dut, ctrl, conf_events, buf, sizeof(buf));
	if (res < 0) {
		send_resp(dut, conn, SIGMA_COMPLETE,
			  "BootstrapResult,OK,AuthResult,OK,ConfResult,Timeout");
		goto out;
	}
	sigma_dut_print(dut, DUT_MSG_DEBUG, "DPP conf result: %s", buf);

	if (!strstr(buf, "DPP-CONF-SENT") &&
	    !strstr(buf, "DPP-CONF-RECEIVED")) {
		send_resp(dut, conn, SIGMA_COMPLETE,
			  "BootstrapResult,OK,AuthResult,OK,ConfResult,FAILED");
		goto out;
	}

	if (sigma_dut_is_ap(dut) &&
	    strcasecmp(prov_role, "Enrollee") == 0) {
	update_ap:
		res = dpp_hostapd_conf_update(dut, conn, ifname, ctrl);
		if (res == 0)
			goto out;
		if (res < 0) {
			send_resp(dut, conn, SIGMA_ERROR, NULL);
			goto out;
		}
	}

	if (strcasecmp(wait_conn, "Yes") == 0 &&
	    !sigma_dut_is_ap(dut) &&
	    strcasecmp(prov_role, "Enrollee") == 0) {
		int netw_id;
		char *pos;

		res = get_wpa_cli_event(dut, ctrl, "DPP-NETWORK-ID",
					buf, sizeof(buf));
		if (res < 0) {
			send_resp(dut, conn, SIGMA_ERROR,
				  "errorCode,No DPP-NETWORK-ID");
			goto out;
		}
		pos = strchr(buf, ' ');
		if (!pos) {
			send_resp(dut, conn, SIGMA_ERROR,
				  "errorCode,Invalid DPP-NETWORK-ID");
			goto out;
		}
		pos++;
		netw_id = atoi(pos);
		snprintf(buf, sizeof(buf), "GET_NETWORK %d key_mgmt", netw_id);
		if (wpa_command_resp(ifname, buf, buf, sizeof(buf)) < 0) {
			send_resp(dut, conn, SIGMA_ERROR,
				  "errorCode,Could not fetch provisioned key_mgmt");
			goto out;
		}
		if (strncmp(buf, "SAE", 3) == 0) {
			/* SAE generates PMKSA-CACHE-ADDED event */
			not_dpp_akm = 1;
		}
	wait_connect:
		if (frametype && strcasecmp(frametype,
					    "PeerDiscoveryRequest") == 0) {
			if (dpp_wait_tx_status(dut, ctrl, 5) < 0)
				result = "BootstrapResult,OK,AuthResult,OK,ConfResult,OK,NetworkIntroResult,Timeout";
			else
				result = "BootstrapResult,OK,AuthResult,OK,ConfResult,OK,NetworkIntroResult,Errorsent";
			send_resp(dut, conn, SIGMA_COMPLETE, result);
			goto out;
		}

		res = get_wpa_cli_events(dut, ctrl, conn_events,
					 buf, sizeof(buf));
		if (res < 0) {
			send_resp(dut, conn, SIGMA_COMPLETE,
				  "BootstrapResult,OK,AuthResult,OK,ConfResult,OK,NetworkIntroResult,Timeout,NetworkConnectResult,Timeout");
			goto out;
		}
		sigma_dut_print(dut, DUT_MSG_DEBUG, "DPP connect result: %s",
				buf);

		if (strstr(buf, "PMKSA-CACHE-ADDED")) {
			res = get_wpa_cli_events(dut, ctrl, conn_events,
						 buf, sizeof(buf));
			if (res < 0) {
				send_resp(dut, conn, SIGMA_COMPLETE,
					  not_dpp_akm ?
					  "BootstrapResult,OK,AuthResult,OK,ConfResult,OK,NetworkConnectResult,Timeout" :
					  "BootstrapResult,OK,AuthResult,OK,ConfResult,OK,NetworkIntroResult,OK,NetworkConnectResult,Timeout");
				goto out;
			}
			sigma_dut_print(dut, DUT_MSG_DEBUG,
					"DPP connect result: %s", buf);
			if (strstr(buf, "CTRL-EVENT-CONNECTED"))
				send_resp(dut, conn, SIGMA_COMPLETE,
					  not_dpp_akm ?
					  "BootstrapResult,OK,AuthResult,OK,ConfResult,OK,NetworkConnectResult,OK" :
					  "BootstrapResult,OK,AuthResult,OK,ConfResult,OK,NetworkIntroResult,OK,NetworkConnectResult,OK");
			else
				send_resp(dut, conn, SIGMA_COMPLETE,
					  not_dpp_akm ?
					  "BootstrapResult,OK,AuthResult,OK,ConfResult,OK,NetworkConnectResult,Timeout" :
					  "BootstrapResult,OK,AuthResult,OK,ConfResult,OK,NetworkIntroResult,OK,NetworkConnectResult,Timeout");
			goto out;
		}

		send_resp(dut, conn, SIGMA_COMPLETE,
			  "BootstrapResult,OK,AuthResult,OK,ConfResult,OK,NetworkConnectResult,OK");
		goto out;
	}

	if (strcasecmp(wait_conn, "Yes") == 0 &&
	    frametype && strcasecmp(frametype, "PeerDiscoveryResponse") == 0) {
		if (dpp_wait_tx_status(dut, ctrl, 6) < 0)
			result = "BootstrapResult,OK,AuthResult,OK,ConfResult,OK,NetworkIntroResult,Timeout";
		else
			result = "BootstrapResult,OK,AuthResult,OK,ConfResult,OK,NetworkIntroResult,Errorsent";
		send_resp(dut, conn, SIGMA_COMPLETE, result);
		goto out;
	}

	send_resp(dut, conn, SIGMA_COMPLETE,
		  "BootstrapResult,OK,AuthResult,OK,ConfResult,OK");
out:
	wpa_ctrl_detach(ctrl);
	wpa_ctrl_close(ctrl);
	dut->default_timeout = old_timeout;
	return 0;
err:
	send_resp(dut, conn, SIGMA_ERROR, NULL);
	goto out;
}


static int dpp_manual_dpp(struct sigma_dut *dut,
			  struct sigma_conn *conn,
			  struct sigma_cmd *cmd)
{
	const char *auth_role = get_param(cmd, "DPPAuthRole");
	const char *self_conf = get_param(cmd, "DPPSelfConfigure");
	int res = -1, success;
	const char *val;
	unsigned int old_timeout;

	if (!auth_role) {
		send_resp(dut, conn, SIGMA_ERROR,
			  "errorCode,Missing DPPAuthRole");
		return 0;
	}

	if (!self_conf)
		self_conf = "no";

	old_timeout = dut->default_timeout;
	val = get_param(cmd, "DPPTimeout");
	if (val && atoi(val) > 0) {
		dut->default_timeout = atoi(val);
		sigma_dut_print(dut, DUT_MSG_DEBUG, "DPP timeout: %u",
				dut->default_timeout);
	}

	res = dpp_get_local_bootstrap(dut, conn, cmd, 0, &success);
	if (res || !success)
		goto out;

	if (strcasecmp(auth_role, "Responder") == 0) {
		res = dpp_display_own_qrcode(dut);
		if (res < 0)
			goto out;

		res = dpp_automatic_dpp(dut, conn, cmd);
		goto out;
	}

	if (strcasecmp(auth_role, "Initiator") == 0) {
		if (strcasecmp(self_conf, "Yes") != 0) {
			res = dpp_scan_peer_qrcode(dut);
			if (res < 0) {
				send_resp(dut, conn, SIGMA_ERROR,
					"errorCode,Failed to scan peer QR Code");
				res = 0;
				goto out;
			}
		}

		res = dpp_automatic_dpp(dut, conn, cmd);
		goto out;
	}

	send_resp(dut, conn, SIGMA_ERROR, "errorCode,Unknown DPPAuthRole");
	res = 0;
out:
	dut->default_timeout = old_timeout;
	return res;
}


int dpp_dev_exec_action(struct sigma_dut *dut, struct sigma_conn *conn,
			struct sigma_cmd *cmd)
{
	const char *type = get_param(cmd, "DPPActionType");
	const char *bs = get_param(cmd, "DPPBS");

	if (!bs) {
		send_resp(dut, conn, SIGMA_ERROR,
			  "errorCode,Missing DPPBS");
		return 0;
	}

	if (!type) {
		send_resp(dut, conn, SIGMA_ERROR,
			  "errorCode,Missing DPPActionType");
		return 0;
	}

	if (strcasecmp(type, "GetLocalBootstrap") == 0)
		return dpp_get_local_bootstrap(dut, conn, cmd, 1, NULL);
	if (strcasecmp(type, "SetPeerBootstrap") == 0)
		return dpp_set_peer_bootstrap(dut, conn, cmd);
	if (strcasecmp(type, "ManualDPP") == 0)
		return dpp_manual_dpp(dut, conn, cmd);
	if (strcasecmp(type, "AutomaticDPP") == 0)
		return dpp_automatic_dpp(dut, conn, cmd);

	send_resp(dut, conn, SIGMA_ERROR,
		  "errorCode,Unsupported DPPActionType");
	return 0;
}
