/* WiFi Example
 * Copyright (c) 2016 ARM Limited
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of n License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0find
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "mbed.h"
#include "TCPSocket.h"

#include "kv_config.h"
#include "mbed-cloud-client/MbedCloudClient.h" // Required for new MbedCloudClient()
#include "factory_configurator_client.h"       // Required for fcc_* functions and FCC_* defines
#include "m2mresource.h"                       // Required for M2MResource
#include "key_config_manager.h"                // Required for kcm_factory_reset
#include "mbed-trace/mbed_trace.h"             // Required for mbed_trace_*

// Pointers to the resources that will be created in main_application().
static MbedCloudClient* cloud_client;
static bool cloud_client_running = true;
static WiFiInterface* wifi;
/* static NetworkInterface* network = NULL; */

// Fake entropy needed for non-TRNG boards. Suitable only for demo devices.
const uint8_t MBED_CLOUD_DEV_ENTROPY[] = { 0xf6, 0xd6, 0xc0, 0x09, 0x9e, 0x6e, 0xf2, 0x37, 0xdc, 0x29, 0x88, 0xf1, 0x57, 0x32, 0x7d, 0xde, 0xac, 0xb3, 0x99, 0x8c, 0xb9, 0x11, 0x35, 0x18, 0xeb, 0x48, 0x29, 0x03, 0x6a, 0x94, 0x6d, 0xe8, 0x40, 0xc0, 0x28, 0xcc, 0xe4, 0x04, 0xc3, 0x1f, 0x4b, 0xc2, 0xe0, 0x68, 0xa0, 0x93, 0xe6, 0x3a };

static M2MResource* m2m_get_res;
static M2MResource* m2m_put_res;
static M2MResource* m2m_post_res;
static M2MResource* m2m_deregister_res;
static M2MResource* m2m_factory_reset_res;
static SocketAddress sa;

EventQueue queue(32 * EVENTS_EVENT_SIZE);
Thread t;
Mutex value_increment_mutex;
Mutex thread_mutex;

void print_client_ids(void)
{
    printf("Account ID: %s\n", cloud_client->endpoint_info()->account_id.c_str());
    printf("Endpoint name: %s\n", cloud_client->endpoint_info()->internal_endpoint_name.c_str());
    printf("Device ID: %s\n\n", cloud_client->endpoint_info()->endpoint_name.c_str());
}

void value_increment(void)
{
    value_increment_mutex.lock();
    m2m_get_res->set_value(m2m_get_res->get_value_int() + 1);
    printf("Counter %" PRIu64 "\n", m2m_get_res->get_value_int());
    value_increment_mutex.unlock();
}

void get_res_update(const char* /*object_name*/)
{
    printf("Counter resource set to %d\n", (int)m2m_get_res->get_value_int());
}

void put_res_update(const char* /*object_name*/)
{
    printf("PUT update %d\n", (int)m2m_put_res->get_value_int());
}

void execute_post(void* /*arguments*/)
{
    printf("POST executed\n");
}

void deregister_client(void)
{
    printf("Unregistering and disconnecting from the wifi.\n");
    cloud_client->close();
}

void deregister(void* /*arguments*/)
{
    printf("POST deregister executed\n");
    m2m_deregister_res->send_delayed_post_response();

    deregister_client();
}

void client_registered(void)
{
    printf("Client registered.\n");
    print_client_ids();
}

void client_unregistered(void)
{
    printf("Client unregistered.\n");
    (void) wifi->disconnect();
    cloud_client_running = false;
}

void factory_reset(void*)
{
    printf("POST factory reset executed\n");
    m2m_factory_reset_res->send_delayed_post_response();

    kcm_factory_reset();
}

void client_error(int err)
{
    printf("client_error(%d) -> %s\n", err, cloud_client->error_description());
}

void update_progress(uint32_t progress, uint32_t total)
{
    uint8_t percent = (uint8_t)((uint64_t)progress * 100 / total);
    printf("Update progress = %" PRIu8 "%%\n", percent);
}

void flush_stdin_buffer(void)
{
    FileHandle *debug_console = mbed::mbed_file_handle(STDIN_FILENO);
    while(debug_console->readable()) {
        char buffer[1];
        debug_console->read(buffer, 1);
    }
}

#define ECHO_SERVER_ADDR "echo.mbedcloudtesting.com"

static const int TCP_OS_STACK_SIZE = 4096;

static Semaphore AvailableThread(1);

Thread threadTCP(osPriorityNormal, TCP_OS_STACK_SIZE, 0, "TCP_THREAD");
Thread threadPELION(osPriorityNormal, TCP_OS_STACK_SIZE, 0, "PELION_THREAD");




void printfComplete(const char* st, char* val)
{
	AvailableThread.try_acquire();
	printf(st, val);
	AvailableThread.release();
}

void printfComplete(const char* st, int val)
{
	AvailableThread.try_acquire();
	printf(st, val);
	AvailableThread.release();
}

void printfComplete(const char* st)
{
	AvailableThread.try_acquire();
	printf(st);
	AvailableThread.release();
}


void fill_tx_buffer_ascii(char *buff, size_t len)
{
    for (size_t i = 0; i < len; ++i) {
        buff[i] = (rand() % 43) + '0';
    }
}

static void sock_connect()
{

  
    uint16_t ECHO_SERVER_PORT = 7;
    SocketAddress tcp_addr;
    TCPSocket sock;
    int ret = -1;
    
    ret = wifi->gethostbyname(ECHO_SERVER_ADDR, &tcp_addr);
    if (ret != 0) {
      printfComplete("\ngethostbyname error: %d\n", ret);
      return;
    }
	
    tcp_addr.set_port(ECHO_SERVER_PORT);

    ret = sock.open(wifi);
    if (ret != 0) {
        printfComplete("threadTCP:::Error from sock.open: %d", ret);
        return;
    }
	
    ret = sock.connect(tcp_addr);
    if (ret != 0) { 
        printfComplete("threadTCP:::Error from sock.connect: %d", ret);
        return;
    }

    {// sock.send()
      static const int BUFF_SIZE = 100;
      static char tx_buff[BUFF_SIZE] = {0};
      int bytes2process;
      int sent;
		
      fill_tx_buffer_ascii(tx_buff, BUFF_SIZE);
      bytes2process = BUFF_SIZE;
		
      for (int k = 0; k < 100; k++)
      {
        sent = 0;		
        sent = sock.send(&(tx_buff[BUFF_SIZE - bytes2process]), bytes2process);		
        wait(1);		
        printfComplete("\nthreadTCP::SENT:::(%d)\n", sent);
      }
    }
	
   ret = sock.close();
    if (ret != 0) { 
        printfComplete("threadTCP:::Error from sock.close: %d", ret);
        return;
    }	
	
}

static void sock_connect_pelion()
{

    int status = wifi->get_ip_address(&sa);
	
    if (status!=0) {
        printfComplete("PELION-ERROR::get_ip_address failed with %d\n", status);
        return;
    }

    // Run developer flow
    status = fcc_init();
	
    if (status != FCC_STATUS_SUCCESS) {
        printfComplete("PELION-ERROR::fcc_init() failed with %d\n", status);
        return;
    }

    //FOR DEVELOPPER ONLY
    (void) fcc_entropy_set(MBED_CLOUD_DEV_ENTROPY, sizeof(MBED_CLOUD_DEV_ENTROPY));
    status = fcc_developer_flow();
    if (status != FCC_STATUS_SUCCESS && status != FCC_STATUS_KCM_FILE_EXIST_ERROR && status != FCC_STATUS_CA_ERROR) {
        printfComplete("PELION-ERROR::fcc_developer_flow() failed with %d\n", status);
        return;
    }

    M2MObjectList m2m_obj_list;

    // GET resource 3200/0/5501
    // PUT also allowed for resetting the resource
    m2m_get_res = M2MInterfaceFactory::create_resource(m2m_obj_list, 3200, 0, 5501, M2MResourceInstance::INTEGER, M2MBase::GET_PUT_ALLOWED);
    if (m2m_get_res->set_value(0) != true) {
        printfComplete("PELION-ERROR::m2m_get_res->set_value() failed\n");
        return;
    }
    if (m2m_get_res->set_value_updated_function(get_res_update) != true) {
        printfComplete("PELION-ERROR::m2m_get_res->set_value_updated_function() failed\n");
        return;
    }

    // PUT resource 3201/0/5853
    m2m_put_res = M2MInterfaceFactory::create_resource(m2m_obj_list, 3201, 0, 5853, M2MResourceInstance::INTEGER, M2MBase::GET_PUT_ALLOWED);
    if (m2m_put_res->set_value(0) != true) {
        printfComplete("PELION-ERROR::m2m_put_res->set_value() failed\n");
        return;
    }
    if (m2m_put_res->set_value_updated_function(put_res_update) != true) {
        printfComplete("PELION-ERROR::m2m_put_res->set_value_updated_function() failed\n");
        return;
    }

    // POST resource 3201/0/5850
    m2m_post_res = M2MInterfaceFactory::create_resource(m2m_obj_list, 3201, 0, 5850, M2MResourceInstance::INTEGER, M2MBase::POST_ALLOWED);
    if (m2m_post_res->set_execute_function(execute_post) != true) {
        printfComplete("PELION-ERROR::m2m_post_res->set_execute_function() failed\n");
        return;
    }

    // POST resource 5000/0/1 to trigger deregister.
    m2m_deregister_res = M2MInterfaceFactory::create_resource(m2m_obj_list, 5000, 0, 1, M2MResourceInstance::INTEGER, M2MBase::POST_ALLOWED);

    // Use delayed response
    m2m_deregister_res->set_delayed_response(true);

    if (m2m_deregister_res->set_execute_function(deregister) != true) {
        printfComplete("PELION-ERROR::m2m_post_res->set_execute_function() failed\n");
        return;
    }

    // optional Device resource for running factory reset for the device. Path of this resource will be: 3/0/6.
    m2m_factory_reset_res = M2MInterfaceFactory::create_device()->create_resource(M2MDevice::FactoryReset);
    if (m2m_factory_reset_res) {
        m2m_factory_reset_res->set_execute_function(factory_reset);
    }


#ifdef MBED_CLOUD_CLIENT_SUPPORT_UPDATE
    cloud_client = new MbedCloudClient(client_registered, client_unregistered, client_error, NULL, update_progress);
#else
    cloud_client = new MbedCloudClient(client_registered, client_unregistered, client_error);
#endif // MBED_CLOUD_CLIENT_SUPPORT_UPDATE

    cloud_client->add_objects(m2m_obj_list); 
	 	
    status = 0;
        
    status = cloud_client->setup(wifi);
	
    t.start(callback(&queue, &EventQueue::dispatch_forever));
    queue.call_every(5000, value_increment);

    // Flush the stdin buffer before reading from it.
    flush_stdin_buffer();
    
    while(cloud_client_running) {
        int in_char = getchar();
        if (in_char == 'i') {
            print_client_ids(); // When 'i' is pressed, print endpoint info
            continue;
        } else if (in_char == 'r') {
            (void) fcc_storage_delete(); // When 'r' is pressed, erase storage and reboot the board.
            printf("Storage erased, rebooting the device.\n\n");
            ThisThread::sleep_for(1*1000);
            NVIC_SystemReset();
        } else if (in_char > 0 && in_char != 0x03) { // Ctrl+C is 0x03 in Mbed OS and Linux returns negative number
            value_increment(); // Simulate button press
            continue;
        }
        deregister_client();
        break;
    }		
}

static void connect_tcp()
{
    sock_connect();
    
    wifi->disconnect();
}

static void connect_tcp_pelion()
{
    sock_connect_pelion();
    
    wifi->disconnect();
}

int main()
{
    int status;
    
    status = mbed_trace_init();
    if (status != 0) {
        printfComplete("mbed_trace_init() failed with %d\n", status);
        return -1;
    }

    // Mount default kvstore
    printf("Application ready\n");
    status = kv_init_storage_config();
    if (status != MBED_SUCCESS) {
        printfComplete("kv_init_storage_config() - failed, status %d\n", status);
        return -1;
    }

    wifi = WiFiInterface::get_default_instance();
    if (!wifi) {
        printfComplete("ERROR: No WiFiInterface found.\n");
        return -1;
    }
	
    int ret = -1;
    ret = wifi->connect("Livebox-ff6b", "5FCA9A78AC5A87A3F40705CC3F", NSAPI_SECURITY_WPA_WPA2);;
	
    if (ret != 0) {
        printfComplete("\ntConnection error: %d\n", ret);
        return -1;
    }
	
    threadPELION.start(callback(connect_tcp_pelion));
	
    threadTCP.start(callback(connect_tcp));
}
