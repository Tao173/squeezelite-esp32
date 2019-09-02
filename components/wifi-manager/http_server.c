/*
Copyright (c) 2017-2019 Tony Pottier

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

@file http_server.c
@author Tony Pottier
@brief Defines all functions necessary for the HTTP server to run.

Contains the freeRTOS task for the HTTP listener and all necessary support
function to process requests, decode URLs, serve files, etc. etc.

@note http_server task cannot run without the wifi_manager task!
@see https://idyl.io
@see https://github.com/tonyp7/esp32-wifi-manager
*/

#include "http_server.h"
#include "cmd_system.h"



/* @brief tag used for ESP serial console messages */
static const char TAG[] = "http_server";
static const char json_start[] = "{ \"autoexec\": %u, \"list\": [";
static const char json_end[] = "]}";
static const char template[] = "{ \"%s\": \"%s\" }";
static const char array_separator[]=",";

/* @brief task handle for the http server */
static TaskHandle_t task_http_server = NULL;


/**
 * @brief embedded binary data.
 * @see file "component.mk"
 * @see https://docs.espressif.com/projects/esp-idf/en/latest/api-guides/build-system.html#embedding-binary-data
 */
extern const uint8_t style_css_start[] asm("_binary_style_css_start");
extern const uint8_t style_css_end[]   asm("_binary_style_css_end");
extern const uint8_t jquery_gz_start[] asm("_binary_jquery_gz_start");
extern const uint8_t jquery_gz_end[] asm("_binary_jquery_gz_end");
extern const uint8_t code_js_start[] asm("_binary_code_js_start");
extern const uint8_t code_js_end[] asm("_binary_code_js_end");
extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[] asm("_binary_index_html_end");


/* const http headers stored in ROM */
const static char http_html_hdr[] = "HTTP/1.1 200 OK\nContent-type: text/html\n\n";
const static char http_css_hdr[] = "HTTP/1.1 200 OK\nContent-type: text/css\nCache-Control: public, max-age=31536000\n\n";
const static char http_js_hdr[] = "HTTP/1.1 200 OK\nContent-type: text/javascript\n\n";
const static char http_jquery_gz_hdr[] = "HTTP/1.1 200 OK\nContent-type: text/javascript\nAccept-Ranges: bytes\nContent-Length: 29995\nContent-Encoding: gzip\n\n";
const static char http_400_hdr[] = "HTTP/1.1 400 Bad Request\nContent-Length: 0\n\n";
const static char http_404_hdr[] = "HTTP/1.1 404 Not Found\nContent-Length: 0\n\n";
const static char http_503_hdr[] = "HTTP/1.1 503 Service Unavailable\nContent-Length: 0\n\n";
const static char http_ok_json_no_cache_hdr[] = "HTTP/1.1 200 OK\nContent-type: application/json\nCache-Control: no-store, no-cache, must-revalidate, max-age=0\nPragma: no-cache\n\n";
const static char http_redirect_hdr_start[] = "HTTP/1.1 302 Found\nLocation: http://";
const static char http_redirect_hdr_end[] = "/\n\n";



void http_server_start(){
	if(task_http_server == NULL){
		xTaskCreate(&http_server, "http_server", 1024*3, NULL, WIFI_MANAGER_TASK_PRIORITY-1, &task_http_server);
	}
}

void http_server(void *pvParameters) {

	struct netconn *conn, *newconn;
	err_t err;
	conn = netconn_new(NETCONN_TCP);
	netconn_bind(conn, IP_ADDR_ANY, 80);
	netconn_listen(conn);
	ESP_LOGI(TAG, "HTTP Server listening on 80/tcp");
	do {
		err = netconn_accept(conn, &newconn);
		if (err == ERR_OK) {
			http_server_netconn_serve(newconn);
			netconn_delete(newconn);
		}
		else
		{
			ESP_LOGE(TAG,"Error accepting new connection. Terminating HTTP server");
		}
		taskYIELD();  /* allows the freeRTOS scheduler to take over if needed. */
	} while(err == ERR_OK);

	netconn_close(conn);
	netconn_delete(conn);

	vTaskDelete( NULL );
}


char* http_server_get_header(char *request, char *header_name, int *len) {
	*len = 0;
	char *ret = NULL;
	char *ptr = NULL;

	ptr = strstr(request, header_name);
	if (ptr) {
		ret = ptr + strlen(header_name);
		ptr = ret;
		while (*ptr != '\0' && *ptr != '\n' && *ptr != '\r') {
			(*len)++;
			ptr++;
		}
		return ret;
	}
	return NULL;
}


void http_server_netconn_serve(struct netconn *conn) {

	struct netbuf *inbuf;
	char *buf = NULL;
	u16_t buflen;
	err_t err;
	const char new_line[2] = "\n";

	err = netconn_recv(conn, &inbuf);
	if (err == ERR_OK) {

		netbuf_data(inbuf, (void**)&buf, &buflen);

		/* extract the first line of the request */
		char *save_ptr = buf;
		char *line = strtok_r(save_ptr, new_line, &save_ptr);
		ESP_LOGD(TAG,"Processing line %s",line);

		if(line) {

			/* captive portal functionality: redirect to access point IP for HOST that are not the access point IP OR the STA IP */
			int lenH = 0;
			char *host = http_server_get_header(save_ptr, "Host: ", &lenH);
			/* determine if Host is from the STA IP address */
			wifi_manager_lock_sta_ip_string(portMAX_DELAY);
			bool access_from_sta_ip = lenH > 0?strstr(host, wifi_manager_get_sta_ip_string()):false;
			wifi_manager_unlock_sta_ip_string();

			if (lenH > 0 && !strstr(host, DEFAULT_AP_IP) && !access_from_sta_ip) {
				ESP_LOGI(TAG,"Redirecting to default AP IP Address : %s", DEFAULT_AP_IP);
				netconn_write(conn, http_redirect_hdr_start, sizeof(http_redirect_hdr_start) - 1, NETCONN_NOCOPY);
				netconn_write(conn, DEFAULT_AP_IP, sizeof(DEFAULT_AP_IP) - 1, NETCONN_NOCOPY);
				netconn_write(conn, http_redirect_hdr_end, sizeof(http_redirect_hdr_end) - 1, NETCONN_NOCOPY);

			}
			else{
				/* default page */
				if(strstr(line, "GET / ")) {
					netconn_write(conn, http_html_hdr, sizeof(http_html_hdr) - 1, NETCONN_NOCOPY);
					netconn_write(conn, index_html_start, index_html_end - index_html_start, NETCONN_NOCOPY);
				}
				else if(strstr(line, "GET /jquery.js ")) {
					netconn_write(conn, http_jquery_gz_hdr, sizeof(http_jquery_gz_hdr) - 1, NETCONN_NOCOPY);
					netconn_write(conn, jquery_gz_start, jquery_gz_end - jquery_gz_start, NETCONN_NOCOPY);
				}
				else if(strstr(line, "GET /code.js ")) {
					netconn_write(conn, http_js_hdr, sizeof(http_js_hdr) - 1, NETCONN_NOCOPY);
					netconn_write(conn, code_js_start, code_js_end - code_js_start, NETCONN_NOCOPY);
				}
				else if(strstr(line, "GET /ap.json ")) {
					/* if we can get the mutex, write the last version of the AP list */
					ESP_LOGI(TAG,"Processing ap.json request");
					if(wifi_manager_lock_json_buffer(( TickType_t ) 10)){
						netconn_write(conn, http_ok_json_no_cache_hdr, sizeof(http_ok_json_no_cache_hdr) - 1, NETCONN_NOCOPY);
						char *buff = wifi_manager_get_ap_list_json();
						netconn_write(conn, buff, strlen(buff), NETCONN_NOCOPY);
						wifi_manager_unlock_json_buffer();
					}
					else{
						netconn_write(conn, http_503_hdr, sizeof(http_503_hdr) - 1, NETCONN_NOCOPY);
						ESP_LOGE(TAG, "http_server_netconn_serve: GET /ap.json failed to obtain mutex");
					}
					/* request a wifi scan */
					ESP_LOGI(TAG,"Starting wifi scan");
					wifi_manager_scan_async();
				}
				else if(strstr(line, "GET /style.css ")) {
					netconn_write(conn, http_css_hdr, sizeof(http_css_hdr) - 1, NETCONN_NOCOPY);
					netconn_write(conn, style_css_start, style_css_end - style_css_start, NETCONN_NOCOPY);
				}
				else if(strstr(line, "GET /status.json ")){
					ESP_LOGI(TAG,"Serving status.json");
					if(wifi_manager_lock_json_buffer(( TickType_t ) 10)){
						char *buff = wifi_manager_get_ip_info_json();
						if(buff){
							netconn_write(conn, http_ok_json_no_cache_hdr, sizeof(http_ok_json_no_cache_hdr) - 1, NETCONN_NOCOPY);
							netconn_write(conn, buff, strlen(buff), NETCONN_NOCOPY);

							wifi_manager_unlock_json_buffer();
						}
						else{
							netconn_write(conn, http_503_hdr, sizeof(http_503_hdr) - 1, NETCONN_NOCOPY);
						}
					}
					else{
						netconn_write(conn, http_503_hdr, sizeof(http_503_hdr) - 1, NETCONN_NOCOPY);
						ESP_LOGE(TAG, "http_server_netconn_serve: GET /status failed to obtain mutex");
					}
				}
				else if(strstr(line, "GET /config.json ")){
					ESP_LOGI(TAG,"Serving config.json");
					char autoexec_name[21]={0};
					char * autoexec_value=NULL;
					uint8_t autoexec_flag=0;
					int buflen=MAX_COMMAND_LINE_SIZE+strlen(template)+1;
					char * buff = malloc(buflen);
          			char *s = "\"";
          			char *r = "\\\"";
					if(!buff)
					{
						ESP_LOGE(TAG,"Unable to allocate buffer for config.json!");
						netconn_write(conn, http_503_hdr, sizeof(http_503_hdr) - 1, NETCONN_NOCOPY);
					}
					else
					{
						int i=1;
						size_t l = 0;
						netconn_write(conn, http_ok_json_no_cache_hdr, sizeof(http_ok_json_no_cache_hdr) - 1, NETCONN_NOCOPY);

						autoexec_flag = wifi_manager_get_flag();
						snprintf(buff,buflen-1, json_start, autoexec_flag);
						netconn_write(conn, buff, strlen(buff), NETCONN_NOCOPY);
						do {
							snprintf(autoexec_name,sizeof(autoexec_name)-1,"autoexec%u",i);
							ESP_LOGD(TAG,"Getting command name %s", autoexec_name);
							autoexec_value = wifi_manager_alloc_get_config(autoexec_name, &l);
							if(autoexec_value!=NULL ){
								if(i>1)
								{
									netconn_write(conn, array_separator, strlen(array_separator), NETCONN_NOCOPY);
									ESP_LOGD(TAG,"%s", array_separator);
								}
								ESP_LOGI(TAG,"found command %s = %s", autoexec_name, autoexec_value);
                				strreplace(autoexec_value, s, r);
								snprintf(buff, buflen-1, template, autoexec_name, autoexec_value);
								netconn_write(conn, buff, strlen(buff), NETCONN_NOCOPY);
								ESP_LOGD(TAG,"%s", buff);
								ESP_LOGD(TAG,"Freeing memory for command %s name", autoexec_name);
								free(autoexec_value);
							}
							else {
								ESP_LOGD(TAG,"No matching command found for name %s", autoexec_name);
								break;
							}
							i++;
						} while(1);
						free(buff);
						netconn_write(conn, json_end, strlen(json_end), NETCONN_NOCOPY);
						ESP_LOGD(TAG,"%s", json_end);
					}
				}
				else if(strstr(line, "POST /factory.json ")){
					guided_factory();
				}
				else if(strstr(line, "POST /config.json ")){
					ESP_LOGI(TAG,"Serving POST config.json");

					if(wifi_manager_lock_json_buffer(( TickType_t ) 10)){
						int i=1;
						int lenS = 0, lenA=0;
						char autoexec_name[22]={0};
						char autoexec_key[12]={0};
						char * autoexec_value=NULL;
						char * autoexec_flag_s=NULL;
						uint8_t autoexec_flag=0;
						autoexec_flag_s = http_server_get_header(save_ptr, "X-Custom-autoexec: ", &lenA);
						if(autoexec_flag_s!=NULL && lenA > 0)
						{
							autoexec_flag = atoi(autoexec_flag_s);
							wifi_manager_save_autoexec_flag(autoexec_flag);
						}

						do {
							if(snprintf(autoexec_name,sizeof(autoexec_name)-1,"X-Custom-autoexec%u: ",i)<0)
							{
								ESP_LOGE(TAG,"Unable to process autoexec%u. Name length overflow.",i);
								break;
							}
							if(snprintf(autoexec_key,sizeof(autoexec_key)-1,"autoexec%u",i++)<0)
							{
								ESP_LOGE(TAG,"Unable to process autoexec%u. Name length overflow.",i);
								break;
							}
							ESP_LOGD(TAG,"Looking for command name %s.", autoexec_name);
							autoexec_value = http_server_get_header(save_ptr, autoexec_name, &lenS);


							if(autoexec_value ){
								// todo: replace line below, as it causes an error during compile.
								// snprintf(autoexec_value, lenS+1, autoexec_value);
								if(lenS < MAX_COMMAND_LINE_SIZE ){
									ESP_LOGD(TAG, "http_server_netconn_serve: config.json/ call, with %s: %s, length %i", autoexec_key, autoexec_value, lenS);
									wifi_manager_save_autoexec_config(autoexec_value,autoexec_key,lenS);
								}
								else
								{
									ESP_LOGE(TAG,"command line length is too long : %s = %s", autoexec_name, autoexec_value);
								}
							}
							else {
								ESP_LOGD(TAG,"No matching command found for name %s", autoexec_name);
								break;
							}
						} while(1);

						netconn_write(conn, http_ok_json_no_cache_hdr, sizeof(http_ok_json_no_cache_hdr) - 1, NETCONN_NOCOPY); //200ok

					}
					else{
						netconn_write(conn, http_503_hdr, sizeof(http_503_hdr) - 1, NETCONN_NOCOPY);
						ESP_LOGE(TAG, "http_server_netconn_serve: GET /status failed to obtain mutex");
					}
				}

				else if(strstr(line, "DELETE /connect.json ")) {
					ESP_LOGI(TAG, "http_server_netconn_serve: DELETE /connect.json");
					/* request a disconnection from wifi and forget about it */
					wifi_manager_disconnect_async();
					netconn_write(conn, http_ok_json_no_cache_hdr, sizeof(http_ok_json_no_cache_hdr) - 1, NETCONN_NOCOPY); /* 200 ok */
				}
				else if(strstr(line, "POST /connect.json ")) {
					ESP_LOGI(TAG, "http_server_netconn_serve: POST /connect.json");
					bool found = false;
					int lenS = 0, lenP = 0;
					char *ssid = NULL, *password = NULL;
					ssid = http_server_get_header(save_ptr, "X-Custom-ssid: ", &lenS);
					password = http_server_get_header(save_ptr, "X-Custom-pwd: ", &lenP);

					if(ssid && lenS <= MAX_SSID_SIZE && password && lenP <= MAX_PASSWORD_SIZE){
						wifi_config_t* config = wifi_manager_get_wifi_sta_config();
						memset(config, 0x00, sizeof(wifi_config_t));
						memcpy(config->sta.ssid, ssid, lenS);
						memcpy(config->sta.password, password, lenP);
						ESP_LOGD(TAG, "http_server_netconn_serve: wifi_manager_connect_async() call, with ssid: %s, password: %s", ssid, password);
						wifi_manager_connect_async();
						netconn_write(conn, http_ok_json_no_cache_hdr, sizeof(http_ok_json_no_cache_hdr) - 1, NETCONN_NOCOPY); //200ok
						found = true;
					}

					if(!found){
						/* bad request the authentification header is not complete/not the correct format */
						netconn_write(conn, http_400_hdr, sizeof(http_400_hdr) - 1, NETCONN_NOCOPY);
						ESP_LOGE(TAG, "bad request the authentification header is not complete/not the correct format");
					}

				}
				else{
					netconn_write(conn, http_400_hdr, sizeof(http_400_hdr) - 1, NETCONN_NOCOPY);
					ESP_LOGE(TAG, "bad request");
				}
			}
		}
		else{
			ESP_LOGE(TAG, "URL Not found. Sending 404.");
			netconn_write(conn, http_404_hdr, sizeof(http_404_hdr) - 1, NETCONN_NOCOPY);
		}
	}

	/* free the buffer */
	netbuf_delete(inbuf);
}

void strreplace(char *src, char *str, char *rep)
{
    char *p = strstr(src, str);
    if (p)
    {
        int len = strlen(src)+strlen(rep)-strlen(str);
        char r[len];
        memset(r, 0, len);
        if ( p >= src ){
            strncpy(r, src, p-src);
            r[p-src]='\0';
            strncat(r, rep, strlen(rep));
            strncat(r, p+strlen(str), p+strlen(str)-src+strlen(src));
            strcpy(src, r);
            strreplace(p+strlen(rep), str, rep);
        }
    }
}

