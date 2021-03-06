/*
 * Copyright (c) 2016 Dell Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); you may
 * not use this file except in compliance with the License. You may obtain
 * a copy of the License at http://www.apache.org/licenses/LICENSE-2.0
 *
 * THIS CODE IS PROVIDED ON AN *AS IS* BASIS, WITHOUT WARRANTIES OR
 * CONDITIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING WITHOUT
 * LIMITATION ANY IMPLIED WARRANTIES OR CONDITIONS OF TITLE, FITNESS
 * FOR A PARTICULAR PURPOSE, MERCHANTABLITY OR NON-INFRINGEMENT.
 *
 * See the Apache Version 2.0 License for specific language governing
 * permissions and limitations under the License.
 */

/*!
 * \file   hal_rt_main.c
 * \brief  Hal Routing core functionality
 */

#define _GNU_SOURCE

#include "hal_rt_main.h"
#include "hal_rt_mem.h"
#include "hal_rt_route.h"
#include "hal_rt_api.h"
#include "hal_rt_debug.h"
#include "hal_rt_util.h"
#include "nas_rt_api.h"
#include "hal_rt_mpath_grp.h"
#include "hal_if_mapping.h"
#include "nas_switch.h"
#include "std_thread_tools.h"

#include "event_log.h"
#include "cps_api_object_category.h"
#include "cps_api_route.h"
#include "cps_api_operation.h"
#include "cps_api_events.h"
#include "cps_class_map.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/**************************************************************************
 *                            GLOBALS
 **************************************************************************/
static std_thread_create_param_t hal_rt_main_thr;
static std_thread_create_param_t hal_rt_dr_thr;
static std_thread_create_param_t hal_rt_nh_thr;
static std_thread_create_param_t hal_rt_cps_thr;

static t_fib_config      g_fib_config;
static t_fib_vrf        *ga_fib_vrf [FIB_MAX_VRF];

static cps_api_operation_handle_t nas_rt_cps_handle;

#define NUM_INT_NAS_RT_CPS_API_THREAD 1

static std_mutex_lock_create_static_init_fast(nas_l3_mutex);

/***************************************************************************
 *                          Private Functions
 ***************************************************************************/

int hal_rt_config_init (void)
{
    /* Init the configs to default values */
    memset (&g_fib_config, 0, sizeof (g_fib_config));
    g_fib_config.max_num_npu          = nas_switch_get_max_npus();
    g_fib_config.ecmp_max_paths       = HAL_RT_MAX_ECMP_PATH;
    g_fib_config.hw_ecmp_max_paths    = HAL_RT_MAX_ECMP_PATH;
    g_fib_config.ecmp_path_fall_back  = false;
    g_fib_config.ecmp_hash_sel        = FIB_DEFAULT_ECMP_HASH;

    return STD_ERR_OK;
}

const t_fib_config * hal_rt_access_fib_config(void)
{
    return(&g_fib_config);
}

void nas_l3_lock()
{
    std_mutex_lock(&nas_l3_mutex);
}

void nas_l3_unlock()
{
    std_mutex_unlock(&nas_l3_mutex);
}

t_fib_vrf * hal_rt_access_fib_vrf(uint32_t vrf_id)
{
    return(ga_fib_vrf[vrf_id]);
}

t_fib_vrf_info * hal_rt_access_fib_vrf_info(uint32_t vrf_id, uint8_t af_index)
{
    return(&(ga_fib_vrf[vrf_id]->info[af_index]));
}

t_fib_vrf_cntrs * hal_rt_access_fib_vrf_cntrs(uint32_t vrf_id, uint8_t af_index)
{
    return(&(ga_fib_vrf[vrf_id]->cntrs[af_index]));
}

std_rt_table * hal_rt_access_fib_vrf_dr_tree(uint32_t vrf_id, uint8_t af_index)
{
    return(ga_fib_vrf[vrf_id]->info[af_index].dr_tree);
}

std_rt_table * hal_rt_access_fib_vrf_nh_tree(uint32_t vrf_id, uint8_t af_index)
{
    return(ga_fib_vrf[vrf_id]->info[af_index].nh_tree);
}

std_rt_table * hal_rt_access_fib_vrf_mp_md5_tree(uint32_t vrf_id, uint8_t af_index)
{
    return(ga_fib_vrf[vrf_id]->info[af_index].mp_md5_tree);
}

int hal_rt_vrf_init (void)
{
    t_fib_vrf      *p_vrf = NULL;
    t_fib_vrf_info *p_vrf_info = NULL;
    hal_vrf_id_t    vrf_id = 0;
    uint8_t         af_index = 0;
    ndi_vr_entry_t  vr_entry;
    hal_mac_addr_t  ndi_mac;
    ndi_vrf_id_t    ndi_vr_id = 0;
    t_std_error     rc = STD_ERR_OK;

   /* Create a virtual router entry and get vr_id (maps to fib vrf id) */
    memset (&vr_entry, 0, sizeof (ndi_vr_entry_t));
    vr_entry.npu_id = 0;

    memset(&ndi_mac, 0, sizeof (hal_mac_addr_t));
    /* Wait for the system MAC to become ready - will also provide trace logs to indicate issues if there are any*/
    nas_switch_wait_for_sys_base_mac(&ndi_mac);

    if(hal_rt_is_mac_address_zero((const hal_mac_addr_t *)&ndi_mac)) {
        EV_LOG_ERR(ev_log_t_ROUTE, 1, "HAL-RT", "The system MAC is zero, NAS Route VR init failed!");
        return STD_ERR(ROUTE, FAIL, rc);
    }
    /*
     * Set system MAC address for the VRs
     */
    memcpy(vr_entry.src_mac, &ndi_mac, HAL_MAC_ADDR_LEN);
    vr_entry.flags |= NDI_VR_ATTR_SRC_MAC_ADDRESS;

    for (vrf_id = FIB_MIN_VRF; vrf_id < FIB_MAX_VRF; vrf_id ++) {

        /* Create default VRF and other vrfs as per FIB_MAX_VRF */
         if ((rc = ndi_route_vr_create(&vr_entry, &ndi_vr_id))!= STD_ERR_OK) {
            EV_LOG_ERR(ev_log_t_ROUTE, 3, "HAL-RT", "%s ():VR creation failed",
                         __FUNCTION__);
            return STD_ERR(ROUTE, FAIL, rc);
        }

        if ((p_vrf = FIB_VRF_MEM_MALLOC ()) == NULL) {
            EV_LOG_TRACE (ev_log_t_ROUTE, 3, "HAL-RT", "Memory alloc failed.Vrf_id: %d", vrf_id);
            continue;
        }

        memset (p_vrf, 0, sizeof (t_fib_vrf));
        p_vrf->vrf_id = vrf_id;
        p_vrf->vrf_obj_id = ndi_vr_id;
        ga_fib_vrf[vrf_id] = p_vrf;

        for (af_index = FIB_MIN_AFINDEX; af_index < FIB_MAX_AFINDEX; af_index++) {
            p_vrf_info = hal_rt_access_fib_vrf_info(vrf_id, af_index);

            p_vrf_info->vrf_id = vrf_id;
            p_vrf_info->af_index = af_index;

            /* Create the DR Tree */
            fib_create_dr_tree (p_vrf_info);

            /* Create the NH Tree */
            fib_create_nh_tree (p_vrf_info);

            /*
             *  Create the Multi-path MP MD5 Tree
             */
            fib_create_mp_md5_tree (p_vrf_info);

            if (vrf_id == FIB_DEFAULT_VRF) {
                p_vrf_info->is_vrf_created = true;
            }
            p_vrf_info->is_catch_all_disabled = false;
        }
    }
    return STD_ERR_OK;
}

int hal_rt_vrf_de_init (void)
{
    t_fib_vrf      *p_vrf = NULL;
    t_fib_vrf_info *p_vrf_info = NULL;
    uint32_t        vrf_id = 0, itr = 0;
    uint32_t        ndi_vr_id = 0;
    uint32_t        ndi_peer_routing_vr_id = 0;
    uint8_t         af_index = 0;
    npu_id_t        npu_id = 0;
    t_std_error     rc = STD_ERR_OK;

    EV_LOG_TRACE (ev_log_t_ROUTE, 3, "HAL-RT", "Vrf de-initialize");

    for (vrf_id = FIB_MIN_VRF; vrf_id < FIB_MAX_VRF; vrf_id ++) {

        p_vrf = hal_rt_access_fib_vrf(vrf_id);
        if (p_vrf == NULL) {
            EV_LOG_TRACE (ev_log_t_ROUTE, 3, "HAL-RT", "Vrf node NULL. Vrf_id: %d", vrf_id);
            continue;
        }

        for (af_index = FIB_MIN_AFINDEX; af_index < FIB_MAX_AFINDEX; af_index++) {
            p_vrf_info = hal_rt_access_fib_vrf_info(vrf_id, af_index);

            /* Destruct the DR radical walk */
            std_radical_walkdestructor(p_vrf_info->dr_tree, &p_vrf_info->dr_radical_marker);

            /* Destroy the DR Tree */
            fib_destroy_dr_tree (p_vrf_info);

            /* Destruct the NH radical walk */
            std_radical_walkdestructor(p_vrf_info->nh_tree, &p_vrf_info->nh_radical_marker);

            /* Destroy the NH Tree */
            fib_destroy_nh_tree (p_vrf_info);

            /* Destroy the MP MD5 Tree */
            fib_destroy_mp_md5_tree (p_vrf_info);
        }
        ndi_vr_id = p_vrf->vrf_obj_id;

        for (itr = 0; itr < HAL_RT_MAX_PEER_ENTRY ; itr++) {
            ndi_peer_routing_vr_id = p_vrf->peer_routing_config[itr].obj_id;
            if (ndi_peer_routing_vr_id != 0) {
                /* Destroy peer VRF information  */
                if ((rc = ndi_route_vr_delete(npu_id, ndi_peer_routing_vr_id))!= STD_ERR_OK) {
                    EV_LOG_ERR(ev_log_t_ROUTE, 3, "HAL-RT", "%s ():Peer VR Delete failed",
                               __FUNCTION__);
                }
            }
        }
        memset (p_vrf, 0, sizeof (t_fib_vrf));
        FIB_VRF_MEM_FREE (p_vrf);
        p_vrf = NULL;
        /* Destroy default VRF and other vrfs as per FIB_MAX_VRF */
         if ((rc = ndi_route_vr_delete(npu_id, ndi_vr_id))!= STD_ERR_OK) {
            EV_LOG_ERR(ev_log_t_ROUTE, 3, "HAL-RT", "%s ():VR Delete failed",
                         __FUNCTION__);
            return STD_ERR(ROUTE, FAIL, rc);
        }
    }
    return STD_ERR_OK;
}

int hal_rt_process_peer_routing_config (uint32_t vrf_id, t_peer_routing_config *p_status) {

    t_fib_vrf      *p_vrf = NULL;
    ndi_vr_entry_t  vr_entry;
    ndi_vrf_id_t    ndi_vr_id = 0;
    t_std_error     rc = STD_ERR_OK;
    npu_id_t        npu_id = 0;
    char            p_buf[HAL_RT_MAX_BUFSZ];
    bool            is_entry_found = false;
    uint32_t        itr = 0, add_indx = HAL_RT_MAX_PEER_ENTRY;

    /* Create a virtual router entry and get vr_id (maps to fib vrf id) */
    memset (&vr_entry, 0, sizeof (ndi_vr_entry_t));
    vr_entry.npu_id = 0;

    if (p_status == NULL) {
        EV_LOG_ERR (ev_log_t_ROUTE, 3, "HAL-RT", "Peer status information NULL for VRF:%d", vrf_id);
        return (STD_ERR_MK(e_std_err_ROUTE, e_std_err_code_FAIL, 0));
    }
    p_vrf = hal_rt_access_fib_vrf(vrf_id);
    if (p_vrf == NULL) {
        EV_LOG_ERR (ev_log_t_ROUTE, 3, "HAL-RT", "Vrf node NULL. Vrf_id: %d", vrf_id);
        return STD_ERR(ROUTE, FAIL, rc);
    }
    EV_LOG_TRACE (ev_log_t_ROUTE, 2, "HAL-RT", "Vrf:%d ndi-vr-id peer-mac:%s status:%d",
                  vrf_id, hal_rt_mac_to_str(&p_status->peer_mac_addr,
                                            p_buf, HAL_RT_MAX_BUFSZ),
                  p_status->status);
    /*
     * Set system MAC address for the VRs
     */
    for (itr = 0; itr < HAL_RT_MAX_PEER_ENTRY; itr++) {
        if (p_status->status) {
            /* Check if the MAC is already present */
            if (memcmp(&(p_vrf->peer_routing_config[itr].peer_mac_addr), &p_status->peer_mac_addr,
                       sizeof (p_status->peer_mac_addr)) == 0) {
                EV_LOG_ERR (ev_log_t_ROUTE, 3, "HAL-RT", "%s Duplicate MAC set",
                            __FUNCTION__);
                return STD_ERR_OK;
            }

            if ((add_indx == HAL_RT_MAX_PEER_ENTRY) &&
                (hal_rt_is_mac_address_zero((const hal_mac_addr_t*)&p_vrf->peer_routing_config[itr].peer_mac_addr))) {
                add_indx = itr;
            }
        } else {
            /* Check if the MAC present to delete it */
            if (memcmp(&(p_vrf->peer_routing_config[itr].peer_mac_addr), &p_status->peer_mac_addr,
                        sizeof (p_status->peer_mac_addr)) == 0) {
                is_entry_found = true;
                break;
            }
        }
    }
    EV_LOG_TRACE (ev_log_t_ROUTE, 2, "HAL-RT", "Vrf:%d ndi-vr-id peer-mac:%s status:%d itr:%d add_indx:%d is_entry_found:%d",
                  vrf_id, hal_rt_mac_to_str(&p_status->peer_mac_addr,
                                            p_buf, HAL_RT_MAX_BUFSZ),
                  p_status->status, itr, add_indx, is_entry_found);

    memcpy(vr_entry.src_mac, &(p_status->peer_mac_addr), HAL_MAC_ADDR_LEN);
    vr_entry.flags |= NDI_VR_ATTR_SRC_MAC_ADDRESS;

    if (p_status->status) {
        if (add_indx == HAL_RT_MAX_PEER_ENTRY) {
            EV_LOG_ERR (ev_log_t_ROUTE, 3, "HAL-RT", "MAC info. full on %d", vrf_id);
            return STD_ERR(ROUTE, FAIL, rc);
        }
        /* Create the VR entry for MAC */
        if ((rc = ndi_route_vr_create(&vr_entry, &ndi_vr_id))!= STD_ERR_OK) {
            EV_LOG_ERR (ev_log_t_ROUTE, 3, "HAL-RT", "%s ():Peer VR creation failed!",
                        __FUNCTION__);
            return STD_ERR(ROUTE, FAIL, rc);
        } else {
            memcpy(&p_vrf->peer_routing_config[add_indx], p_status, sizeof(t_peer_routing_config));
            p_vrf->peer_routing_config[add_indx].obj_id = ndi_vr_id;
        }
    } else {
        if (is_entry_found == false) {
            /* If we are trying to delete an entry which does not exist, return */
            EV_LOG_ERR (ev_log_t_ROUTE, 3, "HAL-RT", "%s Trying to delete invalid MAC entry",
                        __FUNCTION__);
            return STD_ERR_OK;
        }
        if (p_vrf->peer_routing_config[itr].obj_id) {
            /* Remove peer VLT MAC information */
            if ((rc = ndi_route_vr_delete(npu_id,
                                          p_vrf->peer_routing_config[itr].obj_id))!= STD_ERR_OK) {
                EV_LOG_ERR (ev_log_t_ROUTE, 3, "HAL-RT", "%s ():Peer VR Delete failed!",
                            __FUNCTION__);
                return STD_ERR(ROUTE, FAIL, rc);
            } else {
                memset(&p_vrf->peer_routing_config[itr], 0, sizeof(t_peer_routing_config));
            }
        }
    }
    return rc;
}

static cps_api_object_t nas_route_peer_routing_config_to_cps_object(uint32_t vrf_id,
                                                                    t_peer_routing_config *p_status){
    if(p_status == NULL){
        EV_LOG(ERR,ROUTE,0,"HAL-RT","Null Peer Status pointer passed to convert it to cps object");
        return NULL;
    }

    cps_api_object_t obj = cps_api_object_create();
    if(obj == NULL){
        EV_LOG(ERR,ROUTE,0,"HAL-RT","Failed to allocate memory to cps object");
        return NULL;
    }

    cps_api_key_t key;
    cps_api_key_from_attr_with_qual(&key, BASE_ROUTE_PEER_ROUTING_CONFIG_OBJ,
                                    cps_api_qualifier_TARGET);
    cps_api_object_set_key(obj,&key);

    cps_api_object_attr_add_u32(obj,BASE_ROUTE_PEER_ROUTING_CONFIG_VRF_ID,vrf_id);
    cps_api_object_attr_add(obj,BASE_ROUTE_PEER_ROUTING_CONFIG_PEER_MAC_ADDR,
                            (void*)p_status->peer_mac_addr,HAL_MAC_ADDR_LEN);
    return obj;
}

t_std_error nas_route_get_all_peer_routing_config(cps_api_object_list_t list){

    t_fib_vrf      *p_vrf = NULL;
    uint32_t       vrf_id = 0, itr = 0;

    for (vrf_id = FIB_MIN_VRF; vrf_id < FIB_MAX_VRF; vrf_id ++) {
        p_vrf = hal_rt_access_fib_vrf(vrf_id);
        if (p_vrf == NULL) {
            EV_LOG_ERR (ev_log_t_ROUTE, 3, "HAL-RT", "Vrf node NULL. Vrf_id: %d", vrf_id);
            continue;
        }

        for (itr = 0; itr < HAL_RT_MAX_PEER_ENTRY ; itr++) {
            if (p_vrf->peer_routing_config[itr].status == false)
                continue;

            cps_api_object_t obj = nas_route_peer_routing_config_to_cps_object(vrf_id,
                                                                               &(p_vrf->peer_routing_config[itr]));
            if(obj == NULL)
                continue;

            if (!cps_api_object_list_append(list,obj)) {
                cps_api_object_delete(obj);
                EV_LOG(ERR,ROUTE,0,"HAL-RT","Failed to append peer routing object to object list");
                return STD_ERR(ROUTE,FAIL,0);
            }
        }
    }
    return STD_ERR_OK;
}

t_std_error hal_rt_task_init (void)
{
    t_std_error rc = STD_ERR_OK;

    EV_LOG_TRACE (ev_log_t_ROUTE, 3, "HAL-RT", "Initializing HAL-Routing Core");

    hal_rt_config_init ();

    fib_create_intf_tree ();

    if ((rc = hal_rt_vrf_init ()) != STD_ERR_OK) {
        EV_LOG_ERR(ev_log_t_ROUTE, 3, "HAL-RT", "%s (): vrf_init failed", __FUNCTION__);
        return (STD_ERR_MK(e_std_err_ROUTE, e_std_err_code_FAIL, 0));
    }
    if ((rc = hal_rt_default_dr_init ()) != STD_ERR_OK) {
        EV_LOG_ERR(ev_log_t_ROUTE, 3, "HAL-RT", "%s (): default_dr_init failed", __FUNCTION__);
        return (STD_ERR_MK(e_std_err_ROUTE, e_std_err_code_FAIL, 0));
    }

    return rc;
}

void hal_rt_task_exit (void)
{
    EV_LOG_TRACE (ev_log_t_ROUTE, 3, "HAL-RT", "Exiting HAL-Routing..");

    hal_rt_vrf_de_init ();
    fib_destroy_intf_tree ();
    memset(&g_fib_config, 0, sizeof(g_fib_config));
    exit(0);

    return;
}

int hal_rt_default_dr_init (void)
{
    uint32_t    vrf_id;
    uint8_t     af_index;

    EV_LOG_TRACE (ev_log_t_ROUTE, 3, "HAL-RT", "Init Default DR");

    for (vrf_id = FIB_MIN_VRF; vrf_id < FIB_MAX_VRF; vrf_id ++) {
        for (af_index = FIB_MIN_AFINDEX; af_index < FIB_MAX_AFINDEX; af_index++) {
            fib_add_default_dr (vrf_id, af_index);
        }
    }

    return STD_ERR_OK;
}


static bool hal_rt_process_msg(cps_api_object_t obj, void *param)
{

    if (cps_api_key_get_cat(cps_api_object_key(obj)) != cps_api_obj_cat_ROUTE) {
        return true;
    }

    switch (cps_api_key_get_subcat(cps_api_object_key(obj))) {
        case cps_api_route_obj_ROUTE:
            fib_proc_dr_download(obj);
            break;

        case cps_api_route_obj_NEIBH:
            fib_proc_nbr_download(obj);
            break;

        default:
            EV_LOG_TRACE(ev_log_t_ROUTE, 3, "HAL-RT", "msg sub_class unknown %d",
                    cps_api_key_get_subcat(cps_api_object_key(obj)));
            break;
    }
    return true;
}

t_std_error hal_rt_main(void)
{

    cps_api_event_reg_t reg;

    memset(&reg,0,sizeof(reg));
    const uint_t NUM_KEYS=2;
    cps_api_key_t key[NUM_KEYS];

    cps_api_key_init(&key[0],cps_api_qualifier_TARGET,
            cps_api_obj_cat_ROUTE,cps_api_route_obj_ROUTE,0);
    cps_api_key_init(&key[1],cps_api_qualifier_TARGET,
            cps_api_obj_cat_ROUTE,cps_api_route_obj_NEIBH,0);

    reg.number_of_objects = NUM_KEYS;
    reg.objects = key;
    if (cps_api_event_thread_reg(&reg,hal_rt_process_msg,NULL)!=cps_api_ret_code_OK) {
        return STD_ERR(ROUTE,FAIL,0);
    }

    return STD_ERR_OK;
}

t_std_error hal_rt_cps_thread(void)
{
    t_std_error     rc = STD_ERR_OK;

   // CPS register for events
    cps_api_event_reg_t reg;
    memset(&reg,0,sizeof(reg));
    const uint_t NUM_EVENTS=1;

    cps_api_key_t keys[NUM_EVENTS];

    cps_api_key_init(&keys[0],cps_api_qualifier_TARGET,
                     cps_api_obj_CAT_BASE_ROUTE, BASE_ROUTE_OBJ_OBJ,0 );

    cps_api_key_init(&keys[0],cps_api_qualifier_TARGET,
                     cps_api_obj_CAT_BASE_ROUTE, BASE_ROUTE_NH_TRACK_OBJ,0 );
    reg.number_of_objects = NUM_EVENTS;
    reg.objects = keys;

    //Create a handle for CPS objects
    if (cps_api_operation_subsystem_init(&nas_rt_cps_handle,
            NUM_INT_NAS_RT_CPS_API_THREAD)!=cps_api_ret_code_OK) {
        return STD_ERR(CPSNAS,FAIL,0);
    }

    //Initialize CPS for Routing objects
    if((rc = nas_routing_cps_init(nas_rt_cps_handle)) != STD_ERR_OK) {
        EV_LOG_ERR(ev_log_t_ROUTE, 3, "HAL-RT","Initializing CPS for Routing failed");
        return rc;
    }

    return STD_ERR_OK;
}
t_std_error hal_rt_init(void)
{
    t_std_error     rc = STD_ERR_OK;

    EV_LOG_TRACE(ev_log_t_ROUTE, 3, "HAL-RT", "Initializing HAL-Routing Threads");

    rc = hal_rt_task_init ();
    if (rc != STD_ERR_OK) {
        EV_LOG_ERR(ev_log_t_ROUTE, 3, "HAL-RT", "Initialization failed.");
        hal_rt_task_exit ();
        return (STD_ERR_MK(e_std_err_ROUTE, e_std_err_code_FAIL, 0));
    }
    std_thread_init_struct(&hal_rt_main_thr);
    hal_rt_main_thr.name = "hal-rt-main";
    hal_rt_main_thr.thread_function = (std_thread_function_t)hal_rt_main;

    if (std_thread_create(&hal_rt_main_thr)!=STD_ERR_OK) {
        EV_LOG_ERR(ev_log_t_ROUTE, 3, "HAL-RT-THREAD", "Error creating thread");
        return STD_ERR(ROUTE,FAIL,0);
    }

    std_thread_init_struct(&hal_rt_dr_thr);
    hal_rt_dr_thr.name = "hal-rt-dr";
    hal_rt_dr_thr.thread_function = (std_thread_function_t)fib_dr_walker_main;
    if (std_thread_create(&hal_rt_dr_thr)!=STD_ERR_OK) {
        EV_LOG_ERR(ev_log_t_ROUTE, 3, "HAL-RT-THREAD", "Error creating dr thread");
        return STD_ERR(ROUTE,FAIL,0);
    }

    std_thread_init_struct(&hal_rt_nh_thr);
    hal_rt_nh_thr.name = "hal-rt-nh";
    hal_rt_nh_thr.thread_function = (std_thread_function_t)fib_nh_walker_main;
    if (std_thread_create(&hal_rt_nh_thr)!=STD_ERR_OK) {
        EV_LOG_ERR(ev_log_t_ROUTE, 3, "HAL-RT-THREAD", "Error creating nh thread");
        return STD_ERR(ROUTE,FAIL,0);
    }
    /*
     * New North Bound CPS Routing Thread
     */
    std_thread_init_struct(&hal_rt_cps_thr);
    hal_rt_cps_thr.name = "hal-rt-cps";
    hal_rt_cps_thr.thread_function = (std_thread_function_t)hal_rt_cps_thread;
    if (std_thread_create(&hal_rt_cps_thr)!=STD_ERR_OK) {
        EV_LOG_ERR(ev_log_t_ROUTE, 3, "HAL-RT-THREAD", "Error creating cps thread");
        return STD_ERR(ROUTE,FAIL,0);
    }
    return STD_ERR_OK;
}
