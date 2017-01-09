//
// Spreadtrum Auto Tester
//
// anli   2012-11-10
//

//note:
//        add code to match Marlin & 2341A chip and support FM,BT coexist.
//        2015/01/22 By zhangyj

#include <pthread.h>
#include <stdlib.h>

#include <hardware/bluetooth.h>
#include <hardware/bt_av.h>
#include <hardware/bt_gatt.h>
#include <hardware/bt_gatt_client.h>
#include <hardware/bt_gatt_server.h>
#include <hardware/bt_gatt_types.h>
#include <hardware/bt_hf.h>
#include <hardware/bt_hh.h>
#include <hardware/bt_hl.h>
#include <hardware/bt_pan.h>
#include <hardware/bt_rc.h>
#include <hardware/bt_sock.h>

#include <media/AudioRecord.h>
#include <media/AudioSystem.h>
#include <media/AudioTrack.h>
#include <media/mediarecorder.h>
#include <system/audio.h>
#include <system/audio_policy.h>
#include <time.h>


#include <cutils/properties.h>

#include "type.h"
#include "bt.h"
#include "util.h"
#include "perm.h"
extern int SendAudioTestCmd(const uchar * cmd,int bytes);
//++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
//--namespace sci_fm {
//++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
using namespace android;
using namespace at_perm;

//++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
//--namespace sci_bt {
//++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++

//------------------------------------------------------------------------------
// enable or disable local debug
#define DBG_ENABLE_DBGMSG
#define DBG_ENABLE_WRNMSG
#define DBG_ENABLE_ERRMSG
#define DBG_ENABLE_INFMSG
#define DBG_ENABLE_FUNINF
#include "debug.h"
//------------------------------------------------------------------------------


//------------------------------------------------------------------------------
static const bt_bdaddr_t  BADDR_ANY    = {{0, 0, 0, 0x00, 0x00, 0x00}};
static const bt_bdaddr_t  BADDR_LOCAL  = {{0, 0, 0, 0xff, 0xff, 0xff}};

static pthread_mutex_t sMutxExit    = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  sCondExit    = PTHREAD_COND_INITIALIZER;
static int             sInqStatus   = BT_STATUS_INQUIRE_UNK;
static bdremote_t      sRemoteDev[MAX_SUPPORT_RMTDEV_NUM];
static int             sRemoteDevNum = 0;

static int cb_counter;
static int lock_count;
static timer_t timer;
static alarm_cb saved_callback;
static void *saved_data;
//------------------------------------------------------------------------------
static void * btInquireThread( void *param );
//------------------------------------------------------------------------------
//------------------------------------------------------------------------------
static hw_module_t * s_hwModule = NULL;
static hw_device_t * s_hwDev    = NULL;

static bluetooth_device_t* sBtDevice = NULL;
static const bt_interface_t* sBtInterface = NULL;
static bt_state_t sBtState = BT_STATE_OFF;
static bool set_wake_alarm(uint64_t delay_millis, bool, alarm_cb cb, void *data);
static int acquire_wake_lock(const char *);
static int release_wake_lock(const char *);
//------------------------------------------------------------------------------
//Callbacks;
//------------------------------------------------------------------------------

void btDeviceFoundCallback(int num_properties, bt_property_t *properties);
void btDiscoveryStateChangedCallback(bt_discovery_state_t state);
void btAdapterStateChangedCallback(bt_state_t state);

int scan_enable = BT_SCAN_MODE_CONNECTABLE_DISCOVERABLE;
bt_property_t scan_enable_property = {
  BT_PROPERTY_ADAPTER_SCAN_MODE,
  1,
  &scan_enable,
};

static bt_os_callouts_t stub = {
  sizeof(bt_os_callouts_t),
  set_wake_alarm,
  acquire_wake_lock,
  release_wake_lock,
};

static bt_callbacks_t btCallbacks = {
    sizeof(bt_callbacks_t),
    btAdapterStateChangedCallback, /*adapter_state_changed */
    NULL, /*adapter_properties_cb */
    NULL, /* remote_device_properties_cb */
    btDeviceFoundCallback, /* device_found_cb */
    btDiscoveryStateChangedCallback, /* discovery_state_changed_cb */
    NULL, /* pin_request_cb  */
    NULL, /* ssp_request_cb  */
    NULL, /*bond_state_changed_cb */
    NULL, /* acl_state_changed_cb */
    NULL, /* thread_evt_cb */
    NULL, /*dut_mode_recv_cb */
    NULL, /* le_test_mode_cb */
    NULL /* energy_info_cb */
};

static bool set_wake_alarm(uint64_t delay_millis, bool, alarm_cb cb, void *data) {
/*  saved_callback = cb;
  saved_data = data;

  struct itimerspec wakeup_time;
  memset(&wakeup_time, 0, sizeof(wakeup_time));
  wakeup_time.it_value.tv_sec = (delay_millis / 1000);
  wakeup_time.it_value.tv_nsec = (delay_millis % 1000) * 1000000LL;
  timer_settime(timer, 0, &wakeup_time, NULL);
  return true;
*/
    static timer_t timer;
  static bool timer_created;

  if (!timer_created) {
    struct sigevent sigevent;
    memset(&sigevent, 0, sizeof(sigevent));
    sigevent.sigev_notify = SIGEV_THREAD;
    sigevent.sigev_notify_function = (void (*)(union sigval))cb;
    sigevent.sigev_value.sival_ptr = data;
    timer_create(CLOCK_MONOTONIC, &sigevent, &timer);
    timer_created = true;
  }

  struct itimerspec new_value;
  new_value.it_value.tv_sec = delay_millis / 1000;
  new_value.it_value.tv_nsec = (delay_millis % 1000) * 1000 * 1000;
  new_value.it_interval.tv_sec = 0;
  new_value.it_interval.tv_nsec = 0;
  timer_settime(timer, 0, &new_value, NULL);

  return true;
}
void btAdapterStateChangedCallback(bt_state_t state)
{
    if(state == BT_STATE_OFF || state == BT_STATE_ON)
    {
        sBtState = state;
        DBGMSG("BT Adapter State Changed: %d",state);
#if (defined(SPRD_WCNBT_CHISET) && SPRD_WCNBT_CHISET == marlin)
        if(state == BT_STATE_ON) {
            sBtInterface->set_adapter_property(&scan_enable_property);
            DBGMSG("set scan enable command");
        }
#endif
    } else {
         DBGMSG("err State Changed: %d",state);
    }
}
static int acquire_wake_lock(const char *) {
  /*if (!lock_count)
    lock_count = 1;
    */
  return BT_STATUS_SUCCESS;
}

static int release_wake_lock(const char *) {
 /* if (lock_count)
    lock_count = 0;
    */
  return BT_STATUS_SUCCESS;
}
static char *btaddr2str(const bt_bdaddr_t *bdaddr, bdstr_t *bdstr)
{
    char *addr = (char *) bdaddr->address;

    sprintf((char*)bdstr, "%02x:%02x:%02x:%02x:%02x:%02x",
                       (int)addr[0],(int)addr[1],(int)addr[2],
                       (int)addr[3],(int)addr[4],(int)addr[5]);
    return (char *)bdstr;
}

void btDeviceFoundCallback(int num_properties, bt_property_t *properties)
{
	int i = 0;
	int j = 0;
	char* pName = NULL;
	int nameLen = 0;
	int rssi_value = 0;
	bt_bdaddr_t * pBdaddr = NULL;
	DBGMSG("BT found device, got her (%d)  properties",num_properties);
	if(sRemoteDevNum >= MAX_SUPPORT_RMTDEV_NUM){
		DBGMSG("BT inquiry device list was full!");
		return;
	}

	for(i = 0; i < num_properties; i++){
		switch(properties[i].type){
			case BT_PROPERTY_BDNAME:{
				pName = (char*)properties[i].val;
				nameLen = properties[i].len;
			}continue;

			case BT_PROPERTY_BDADDR:{
				pBdaddr = (bt_bdaddr_t *)properties[i].val;
				for(j = 0; j < sRemoteDevNum; j++){
					if((pBdaddr->address[0] ==  sRemoteDev[j].addr_u8[0])
					&&(pBdaddr->address[1] ==  sRemoteDev[j].addr_u8[1])
					&&(pBdaddr->address[2] ==  sRemoteDev[j].addr_u8[2])
					&&(pBdaddr->address[3] ==  sRemoteDev[j].addr_u8[3])
					&&(pBdaddr->address[4] ==  sRemoteDev[j].addr_u8[4])
					&&(pBdaddr->address[5] ==  sRemoteDev[j].addr_u8[5])){
						DBGMSG("BT inquiry device was OLD:%d:%d:%d:%d:%d:%d",
							pBdaddr->address[0],pBdaddr->address[1],pBdaddr->address[2],
							pBdaddr->address[3],pBdaddr->address[4],pBdaddr->address[5]);
						return;
					}
				}
			}continue;
            case BT_PROPERTY_REMOTE_RSSI:
                rssi_value =(int) *((int8_t *)properties[i].val);
                DBGMSG("bt rssi_value = %d", rssi_value);
                continue;

			default:
				;
		}
	}
	DBGMSG("BT inquiry device was NEW:%d:%d:%d:%d:%d:%d, RSSI=%d",
	pBdaddr->address[0],pBdaddr->address[1],pBdaddr->address[2],
	pBdaddr->address[3],pBdaddr->address[4],pBdaddr->address[5], rssi_value);
	btaddr2str(pBdaddr, (bdstr_t *)(sRemoteDev[sRemoteDevNum].addr_str));
	sRemoteDev[sRemoteDevNum].addr_u8[0] = pBdaddr->address[0];
	sRemoteDev[sRemoteDevNum].addr_u8[1] = pBdaddr->address[1];
	sRemoteDev[sRemoteDevNum].addr_u8[2] = pBdaddr->address[2];
	sRemoteDev[sRemoteDevNum].addr_u8[3] = pBdaddr->address[3];
	sRemoteDev[sRemoteDevNum].addr_u8[4] = pBdaddr->address[4];
	sRemoteDev[sRemoteDevNum].addr_u8[5] = pBdaddr->address[5];

	memcpy(sRemoteDev[sRemoteDevNum].name, pName, nameLen);

	memset(&sRemoteDev[sRemoteDevNum].name[(nameLen -1)],
			0,
			(MAX_REMOTE_DEVICE_NAME_LEN - nameLen));
	sRemoteDev[sRemoteDevNum].rssi_val = rssi_value;

	sRemoteDevNum++;
	DBGMSG("BT totally found (%d) device(s).",sRemoteDevNum);
}

void btDiscoveryStateChangedCallback(bt_discovery_state_t state)
{
	switch(state){
		case BT_DISCOVERY_STOPPED:{
			sInqStatus = BT_STATUS_INQUIRE_END;
			DBGMSG("BT Discovery State Changed: BT_DISCOVERY_STOPPED");
			}break;

		case BT_DISCOVERY_STARTED:{
			sInqStatus = BT_STATUS_INQUIRING;
			DBGMSG("BT Discovery State Changed: BT_DISCOVERY_STARTED");
			}break;

		default:
			;
	}
}

//------------------------------------------------------------------------------
//BT Callbacks END
//------------------------------------------------------------------------------

static  int btHalLoad(void)
{
    int err = 0;

    hw_module_t* module;
    hw_device_t* device;

    INFMSG("Loading HAL lib + extensions");

    err = hw_get_module(BT_HARDWARE_MODULE_ID, (hw_module_t const**)&s_hwModule);
    if (err == 0)
    {
        err = s_hwModule->methods->open(s_hwModule, BT_HARDWARE_MODULE_ID, (hw_device_t**)&s_hwDev);
        if (err == 0) {
            sBtDevice = (bluetooth_device_t *)s_hwDev;
            sBtInterface = sBtDevice->get_bluetooth_interface();
        }
    }

    DBGMSG("HAL library loaded (%s)", strerror(err));

    return err;
}

static int btCheckRtnVal(bt_status_t status)
{
    if (status != BT_STATUS_SUCCESS)
    {
        DBGMSG("HAL REQUEST FAILED ");
		return -1;
    }
    else
    {
        DBGMSG("HAL REQUEST SUCCESS");
		return 0;
    }
}

static int btInit(void)
{
    INFMSG("INIT BT ");
	int retVal = (bt_status_t)sBtInterface->init(&btCallbacks);
	DBGMSG("BT init: %d", retVal);
	if((BT_STATUS_SUCCESS == retVal)||(BT_STATUS_DONE == retVal)){
		retVal = (bt_status_t)sBtInterface->set_os_callouts(&stub);
		if((BT_STATUS_SUCCESS == retVal)||(BT_STATUS_DONE == retVal))
		{
			return (0);
		}
		else
		{
		    return (-1);
		}
	}else{
		return (-1);
	}
}


int btOpen( void )
{
    int counter = 0;
    if (sBtState == BT_STATE_OFF) {
        if ( btHalLoad() < 0 ) {
            ERRMSG("BT load lib Fail");
            return -1;
        }
    } else {
        if (NULL == sBtInterface || NULL == sBtDevice) {
            ERRMSG("sBtInterface=%s,sBtDevice=%s", NULL == sBtInterface?"NULL":"Not NULL",
				NULL == sBtDevice?"NULL":"Not NULL");
            return -1;
        }
    }
    if ( btInit() < 0 ) {
        return -1;
    }

	int ret = sBtInterface->enable(false);

	if( btCheckRtnVal((bt_status_t)ret) ) {
		ERRMSG("BT enable Fail(%d)!\n", ret);
	}

	while (counter++ < 3 && BT_STATE_ON != sBtState) sleep(1);

	if(sBtState == BT_STATE_ON)
		ret = BT_STATUS_SUCCESS;
	else
		ret = -1;
	sBtState = BT_STATE_ON;
	sRemoteDevNum=0;
	memset(&sRemoteDev, 0,
		   (sizeof(bdremote_t)*MAX_SUPPORT_RMTDEV_NUM));

	sInqStatus = BT_STATUS_INQUIRE_UNK;

	return (ret);

}

//------------------------------------------------------------------------------
int btClose(void)
{
	int ret    = 0;
	int counter = 0;
	sInqStatus = BT_STATUS_INQUIRE_UNK;

	ret = sBtInterface->disable();

    while (counter++ < 3 && BT_STATE_OFF != sBtState) sleep(1);

	if(sBtState == BT_STATE_OFF)
		ret = BT_STATUS_SUCCESS;
	else
		ret = -1;
	if( ret ) {
		ERRMSG("BT disable Fail(%d)!\n", ret);
	} else {
		INFMSG("BT disable OK\n");
	}

	sBtState = BT_STATE_OFF;
	return ret;
}


int btIsOpened( void )
{
	if(!sBtInterface)
	{
		sBtState = BT_STATE_OFF;
	}
	return (sBtState);
}

int btGetLocalVersion( struct hci_ver_t *ver )
{
	AT_ASSERT( ver != NULL );

	if (BT_STATE_ON != btIsOpened()) {
		ERRMSG("Device is not available\n");
		return -1;
	}

	//did noting;

	return (BT_STATUS_SUCCESS);

}

int btGetRssi( int *rssi )
{
	AT_ASSERT( rssi != NULL );

	if (BT_STATE_ON != btIsOpened()) {
		ERRMSG("Device is not available\n");
		return -1;
	}
	//did noting
	return (BT_STATUS_SUCCESS);
}

int btAsyncInquire(void)
{
	if (BT_STATE_ON!= btIsOpened()) {
		ERRMSG("Device is not available\n");
		return -1;
	}
	sRemoteDevNum = 0;
	sBtInterface->start_discovery();

	return (BT_STATUS_SUCCESS);
}


//------------------------------------------------------------------------------
int btGetInquireStatus( void )
{
    return sInqStatus;
}

void btSetInquireStatus(int status)
{
    sInqStatus = status;
}

//------------------------------------------------------------------------------
int btGetInquireResult( bdremote_t * bdrmt, int maxnum )
{
	if( BT_STATUS_INQUIRE_END == sInqStatus || BT_STATUS_INQUIRING == sInqStatus){
		int num = ( maxnum > sRemoteDevNum ) ? sRemoteDevNum : maxnum;

		if(num > 0){
			memcpy(bdrmt, sRemoteDev, num * sizeof(bdremote_t));
		}
		DBGMSG("BT found device num = %d\n", num);
		DBGMSG("BT first found dev = %x:%x:%x:%x:%x:%x",
			bdrmt[0].addr_u8[0],bdrmt[0].addr_u8[1],bdrmt[0].addr_u8[2],
			bdrmt[0].addr_u8[3],bdrmt[0].addr_u8[4],bdrmt[0].addr_u8[5]);
		return num;
	}else{
		DBGMSG("BT get INQ result failed! sInqStatus = %d",sInqStatus);
		return sInqStatus;
	}

}