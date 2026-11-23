#include <linux/module.h>
#include <linux/netdevice.h>
#include <net/sock.h>
#include <net/net_namespace.h>
#define CHECK_SIZE(t,n) _Static_assert(sizeof(struct t)==(n),#t " size")
#define CHECK_OFFSET(t,f,n) _Static_assert(offsetof(struct t,f)==(n),#t "." #f " offset")
/* Measured from the ZT30 #112 module ELF and kernel instructions. */
CHECK_SIZE(module,512);
CHECK_OFFSET(module,init,0xe8);
CHECK_OFFSET(module,exit,0x1dc);
CHECK_SIZE(net_device,1088);
CHECK_OFFSET(net_device,ifindex,192);
CHECK_OFFSET(net_device,index_hlist,564);
CHECK_OFFSET(net_device,pcpu_refcnt,644);
CHECK_OFFSET(net_device,netdev_ops,312);
CHECK_OFFSET(net,dev_index_head,220);
CHECK_SIZE(sock,528);
CHECK_OFFSET(sock,sk_socket,460);
CHECK_OFFSET(sock,sk_data_ready,496);
CHECK_OFFSET(sock,sk_write_space,500);
CHECK_OFFSET(sock,sk_destruct,512);
CHECK_OFFSET(sock,sk_rcvbuf,188);
CHECK_OFFSET(sock,sk_sndbuf,216);
CHECK_OFFSET(sock,sk_wq,196);
CHECK_OFFSET(sock,sk_refcnt,68);
CHECK_SIZE(sk_buff,168);
CHECK_OFFSET(sk_buff,len,80);
CHECK_OFFSET(sk_buff,data_len,84);
CHECK_OFFSET(sk_buff,tail,144);
CHECK_OFFSET(sk_buff,end,148);
CHECK_OFFSET(net_device_ops,ndo_get_stats64,64);
MODULE_LICENSE("GPL");
