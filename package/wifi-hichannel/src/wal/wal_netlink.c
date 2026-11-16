/*
 * Copyright (c) Hisilicon Technologies Co., Ltd. 2020-2020. All rights reserved.
 * Description: ioctl implementatioin.
 * Author: Hisilicon
 * Create: 2020-09-28
 */
/*****************************************************************************
  1 Í·ÎÄŒþ°üº¬
*****************************************************************************/
#include <linux/netlink.h>
#include <net/sock.h>
#include "wal_netlink.h"
#include "securec.h"
#include "oal_net.h"
#include "oal_netbuf.h"
#include "hcc_host.h"
#include "hcc_comm.h"
#include "hcc_adapt.h"

#ifdef __cplusplus
#if __cplusplus
extern "C" {
#endif
#endif
/*****************************************************************************
  2 Ã¶ŸÙ¡¢œá¹¹Ìå¶šÒå
*****************************************************************************/
typedef struct {
    struct sock *netlink_sk;
    hi_u32 user_pid;
}netlink_user_s;

static hi_void oal_recieve_user_msg(struct sk_buff *skb);
/*****************************************************************************
  3 ºê¶šÒå¡¢È«ŸÖ±äÁ¿
*****************************************************************************/
#define NETLINK_CHANNEL_MODEID          28
#undef NLMSG_ALIGNTO
#define NLMSG_ALIGNTO                   1

static struct netlink_kernel_cfg cfg = {
    .input  = oal_recieve_user_msg,
};

static netlink_user_s g_netlink_user;
/*****************************************************************************
  4 º¯ÊýÊµÏÖ
*****************************************************************************/
hi_s32 oal_send_user_msg(hi_u8 *pbuf, hi_u32 len)
{
    struct sk_buff *nl_skb = HI_NULL;
    struct nlmsghdr *nlh = HI_NULL;

    if (g_netlink_user.netlink_sk == HI_NULL) {
        return HI_FAILURE;
    }

    if ((pbuf == HI_NULL) || (len == 0) || (len > MAX_USER_LONG_DATA_LEN)) {
        oam_error_log1("send_usrmsg is fail:len:%d\n", len);
        return HI_FAILURE;
    }

    /* ŽŽœšsk_buff ¿ÕŒä */
    nl_skb = nlmsg_new(len, GFP_ATOMIC);
    if (nl_skb == HI_NULL) {
        oam_error_log0("nlmsg_new is fail.\n");
        return HI_FAILURE;
    }

    /* ÉèÖÃnetlinkÏûÏ¢Í·²¿ */
    nlh = nlmsg_put(nl_skb, 0, 0, NETLINK_CHANNEL_MODEID, len, 0);
    if (nlh == HI_NULL) {
        oam_error_log0("nlmsg_put is fail.\n");
        nlmsg_free(nl_skb);
        return HI_FAILURE;
    }

    /* ¿œ±ŽÊýŸÝ·¢ËÍ */
    (hi_void)memcpy_s(nlmsg_data(nlh), nlmsg_len(nlh), pbuf, len);
    hi_s32 ret = netlink_unicast(g_netlink_user.netlink_sk, nl_skb, g_netlink_user.user_pid, MSG_DONTWAIT);
    if (ret == -1) {
        oam_error_log0("netlink_unicast is fail.\n");
        nlmsg_free(nl_skb);
        return HI_FAILURE;
    }

    return HI_SUCCESS;
}
#define MAX_USER_DATA_LEN 384   /* stock threshold: payload <= 384 -> HCC_TYPE_MSG, else DATA */

static hi_void oal_recieve_user_msg(struct sk_buff *skb)
{
    hi_u32 payload_len;
    hi_u32 type;
    hi_char *umsg = HI_NULL;
    struct nlmsghdr *nlh = HI_NULL;
    oal_netbuf_stru *netbuf = HI_NULL;
    hi_s32 ret;

    if (skb == HI_NULL) {
        return;
    }
    oam_warning_log1("DBG nl_rx skb_len=%d", skb->len);

    if (skb->len <= nlmsg_total_size(0)) {
        oam_warning_log2("DBG nl_rx too short len=%d min=%d", skb->len, nlmsg_total_size(0));
        return;
    }

    nlh = nlmsg_hdr(skb);
    umsg = NLMSG_DATA(nlh);
    payload_len = nlh->nlmsg_len - NLMSG_HDRLEN;
    g_netlink_user.user_pid = nlh->nlmsg_pid;
    oam_warning_log3("DBG nl_rx plen=%d id=0x%x pid=0x%x",
                     payload_len, (payload_len > 0) ? (hi_u8)umsg[0] : 0xff, nlh->nlmsg_pid);

    if (payload_len > MAX_USER_LONG_DATA_LEN) {
        oam_error_log1("payload len[%d] is fail\n", payload_len);
        return;
    }

    /* hexdump now that umsg/payload_len are valid (was reading them uninitialised before) */
    {
        hi_u32 k;
        for (k = 0; k < payload_len; k += 4) {
            hi_u32 w = 0; int b;
            for (b = 0; b < 4 && (k + b) < payload_len; b++)
                w |= ((hi_u32)(hi_u8)umsg[k + b]) << (8 * b);
            oam_warning_log2("DBG nlrx +%d = 0x%08x", k, w);
        }
    }

    /* match stock: small control msgs go HIGH-PRIORITY MSG, only bulk goes DATA */
    type = (payload_len <= MAX_USER_DATA_LEN) ? HCC_TYPE_MSG : HCC_TYPE_DATA;

    netbuf = (oal_netbuf_stru *)oal_netbuf_alloc(payload_len, 0, 0);
    if (netbuf == HI_NULL) {
        oam_error_log0("oal_netbuf_alloc is fail\n");
        return;
    }

    (hi_void)memcpy_s(oal_netbuf_data(netbuf), payload_len, umsg, payload_len);
    oal_netbuf_len(netbuf) = payload_len;
    oal_netbuf_next(netbuf) = HI_NULL;
    oal_netbuf_prev(netbuf) = HI_NULL;

    ret = hcc_tx_data_adapt(netbuf, type, HCC_SUB_TYPE_USER_MSG);
    oam_warning_log2("DBG nl_rx tx_adapt ret=%d type=%d", ret, type);
    if (ret != HI_SUCCESS) {
        oam_error_log0("hcc_tx_data_adapt is fail\n");
        oal_netbuf_free(netbuf);
    }
}
   

hi_s32 oal_netlink_init(hi_void)
{
    if (g_netlink_user.netlink_sk != HI_NULL) {
        oam_error_log0("oal_netlink_init is fail\n");
        return HI_FAILURE;
    }

    /* create netlink socket */
    g_netlink_user.netlink_sk = (struct sock *)netlink_kernel_create(&init_net, NETLINK_CHANNEL_MODEID, &cfg);
    if (g_netlink_user.netlink_sk == HI_NULL) {
        oam_error_log0("netlink_kernel_create is fail\n");
        return HI_FAILURE;
    }

    oam_warning_log0("oal_netlink_init is success\n");
    return HI_SUCCESS;
}

hi_s32 oal_netlink_deinit(hi_void)
{
    if (g_netlink_user.netlink_sk == HI_NULL) {
        printk("oal_netlink_deinit is fail\n");
        return HI_FAILURE;
    }

    netlink_kernel_release(g_netlink_user.netlink_sk); /* release. */
    g_netlink_user.netlink_sk = HI_NULL;
    g_netlink_user.user_pid = 0;
    oam_warning_log0("oal_netlink_deinit is success\n");
    return HI_SUCCESS;
}

#ifdef __cplusplus
#if __cplusplus
}
#endif
#endif

