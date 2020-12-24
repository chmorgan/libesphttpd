/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
ESP8266 web server - platform-dependent routines, nonos version
*/

#include <libesphttpd/esp.h>
#include "libesphttpd/httpd.h"
#include "libesphttpd/platform.h"
#include "httpd-platform.h"

#ifndef FREERTOS

//Listening connection data
static struct espconn httpdConn;
static esp_tcp httpdTcp;

//Set/clear global httpd lock.
//Not needed on nonoos.
void ICACHE_FLASH_ATTR httpdPlatLock() {
}
void ICACHE_FLASH_ATTR httpdPlatUnlock() {
}


static void ICACHE_FLASH_ATTR platReconCb(void *arg, sint8 err) {
	//From ESP8266 SDK
	//If still no response, considers it as TCP connection broke, goes into espconn_reconnect_callback.

	ConnTypePtr conn=arg;
	//Just call disconnect to clean up pool and close connection.
	httpdDisconCb(conn, (char*)conn->proto.tcp->remote_ip, conn->proto.tcp->remote_port);
}

static void ICACHE_FLASH_ATTR platDisconCb(void *arg) {
	ConnTypePtr conn=arg;
	httpdDisconCb(conn, (char*)conn->proto.tcp->remote_ip, conn->proto.tcp->remote_port);
}

static void ICACHE_FLASH_ATTR platRecvCb(void *arg, char *data, unsigned short len) {
	ConnTypePtr conn=arg;
	httpdRecvCb(conn, (char*)conn->proto.tcp->remote_ip, conn->proto.tcp->remote_port, data, len);
}

static void ICACHE_FLASH_ATTR platSentCb(void *arg) {
	ConnTypePtr conn=arg;
	httpdSentCb(conn, (char*)conn->proto.tcp->remote_ip, conn->proto.tcp->remote_port);
}

static void ICACHE_FLASH_ATTR platConnCb(void *arg) {
	ConnTypePtr conn=arg;
	if (httpdConnectCb(conn, (char*)conn->proto.tcp->remote_ip, conn->proto.tcp->remote_port)) {
		espconn_regist_recvcb(conn, platRecvCb);
		espconn_regist_reconcb(conn, platReconCb);
		espconn_regist_disconcb(conn, platDisconCb);
		espconn_regist_sentcb(conn, platSentCb);
	} else {
		espconn_disconnect(conn);
	}
}


int ICACHE_FLASH_ATTR httpdPlatSendData(ConnTypePtr conn, char *buff, int len) {
	int r;
	r=espconn_sent(conn, (uint8_t*)buff, len);
	return (r>=0);
}

void ICACHE_FLASH_ATTR httpdPlatDisconnect(ConnTypePtr conn) {
	espconn_disconnect(conn);
}

void ICACHE_FLASH_ATTR httpdPlatDisableTimeout(ConnTypePtr conn) {
	//Can't disable timeout; set to 2 hours instead.
	espconn_regist_time(conn, 7199, 1);
}

//Initialize listening socket, do general initialization
HttpdInitStatus ICACHE_FLASH_ATTR httpdPlatInit(int port, int maxConnCt, uint32_t listenAddress, HttpdFlags flags) {
	// TODO: check flags
	// TODO: handle listenAddress

	httpdConn.type=ESPCONN_TCP;
	httpdConn.state=ESPCONN_NONE;
	httpdTcp.local_port=port;
	httpdConn.proto.tcp=&httpdTcp;
	espconn_regist_connectcb(&httpdConn, platConnCb);
	espconn_accept(&httpdConn);
	espconn_tcp_set_max_con_allow(&httpdConn, maxConnCt);

	return InitializationSuccess;
}

HttpdPlatTimerHandle httpdPlatTimerCreate(const char *name, int periodMs, int autoreload, void (*callback)(void *arg), void *ctx)
{
	HttpdPlatTimerHandle newTimer = malloc(sizeof(HttpdPlatTimerHandle));
	os_timer_setfn(&newTimer->timer, callback, ctx);

	// store the timer settings into the structure as we want to capture them here but
	// can't apply them until the timer is armed
	newTimer->autoReload = autoreload;
	newTimer->timerPeriodMS = periodMs;

	return newTimer;
}

void httpdPlatTimerStart(HttpdPlatTimerHandle timer) {
	os_timer_arm(&timer->timer, timer->timerPeriodMS, timer->autoReload);
}

void httpdPlatTimerStop(HttpdPlatTimerHandle timer) {
	os_timer_disarm(&timer->timer);
}

void httpdPlatTimerDelete(HttpdPlatTimerHandle timer) {
	os_timer_disarm(&timer->timer);
	free(timer);
}

#endif
