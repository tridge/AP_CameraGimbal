#include <linux/module.h>
#include <linux/netdevice.h>
#include <net/sock.h>
#include <net/net_namespace.h>
#define S(t,n) _Static_assert(sizeof(struct t)==(n),#t " size")
#define O(t,f,n) _Static_assert(offsetof(struct t,f)==(n),#t "." #f " offset")
S(module,640); O(module,init,336); O(module,exit,576);
S(net_device,2048); O(net_device,ifindex,264); O(net_device,index_hlist,840);
O(net_device,pcpu_refcnt,992); O(net_device,netdev_ops,488);
O(net_device,nd_net,1048); O(net_device,dev,1064); O(net_device,gso_max_size,1864);
O(net,dev_index_head,296); O(net,user_ns,64);
S(sock,704); O(sock,sk_socket,608); O(sock,sk_data_ready,640);
O(sock,sk_write_space,648); O(sock,sk_destruct,672);
O(sock,sk_rcvbuf,260); O(sock,sk_sndbuf,316); O(sock,sk_wq,272); O(sock,sk_refcnt,128);
S(sk_buff,208); O(sk_buff,len,112); O(sk_buff,data_len,116);
O(sk_buff,tail,176); O(sk_buff,end,180); O(sk_buff,head,184); O(sk_buff,data,192);
S(socket,48); O(socket,sk,32); O(socket,wq,16); O(socket,ops,40);
O(file,private_data,192); O(file,f_flags,64); O(file,f_op,40);
O(net_device_ops,ndo_get_stats64,128);
MODULE_LICENSE("GPL");
