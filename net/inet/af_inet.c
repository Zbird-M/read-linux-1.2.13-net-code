/*
 * INET		An implementation of the TCP/IP protocol suite for the LINUX
 *		operating system.  INET is implemented using the  BSD Socket
 *		interface as the means of communication with the user level.
 *
 *		AF_INET protocol family socket handler.
 *
 * Version:	@(#)af_inet.c	(from sock.c) 1.0.17	06/02/93
 *
 * Authors:	Ross Biro, <bir7@leland.Stanford.Edu>
 *		Fred N. van Kempen, <waltje@uWalt.NL.Mugnet.ORG>
 *		Florian La Roche, <flla@stud.uni-sb.de>
 *		Alan Cox, <A.Cox@swansea.ac.uk>
 *
 * Changes (see also sock.c)
 *
 *		A.N.Kuznetsov	:	Socket death error in accept().
 *		John Richardson :	Fix non blocking error in connect()
 *					so sockets that fail to connect
 *					don't return -EINPROGRESS.
 *		Alan Cox	:	Asynchronous I/O support
 *		Alan Cox	:	Keep correct socket pointer on sock structures
 *					when accept() ed
 *		Alan Cox	:	Semantics of SO_LINGER aren't state moved
 *					to close when you look carefully. With
 *					this fixed and the accept bug fixed 
 *					some RPC stuff seems happier.
 *		Niibe Yutaka	:	4.4BSD style write async I/O
 *		Alan Cox, 
 *		Tony Gale 	:	Fixed reuse semantics.
 *		Alan Cox	:	bind() shouldn't abort existing but dead
 *					sockets. Stops FTP netin:.. I hope.
 *		Alan Cox	:	bind() works correctly for RAW sockets. Note
 *					that FreeBSD at least is broken in this respect
 *					so be careful with compatibility tests...
 *
 *		This program is free software; you can redistribute it and/or
 *		modify it under the terms of the GNU General Public License
 *		as published by the Free Software Foundation; either version
 *		2 of the License, or (at your option) any later version.
 */

#include <linux/config.h>
#include <linux/errno.h>
#include <linux/types.h>
#include <linux/socket.h>
#include <linux/in.h>
#include <linux/kernel.h>
#include <linux/major.h>
#include <linux/sched.h>
#include <linux/timer.h>
#include <linux/string.h>
#include <linux/sockios.h>
#include <linux/net.h>
#include <linux/fcntl.h>
#include <linux/mm.h>
#include <linux/interrupt.h>

#include <asm/segment.h>
#include <asm/system.h>

#include <linux/inet.h>
#include <linux/netdevice.h>
#include "ip.h"
#include "protocol.h"
#include "arp.h"
#include "rarp.h"
#include "route.h"
#include "tcp.h"
#include "udp.h"
#include <linux/skbuff.h>
#include "sock.h"
#include "raw.h"
#include "icmp.h"

#define min(a,b)	((a)<(b)?(a):(b))

extern struct proto packet_prot;


/*
 *	See if a socket number is in use.
 */
// 看socket的端口是否在使用 
static int sk_inuse(struct proto *prot, int num)
{
	struct sock *sk;
	// 根据端口号取得哈希链表中的一个链表
	for(sk = prot->sock_array[num & (SOCK_ARRAY_SIZE -1 )];
		sk != NULL;  sk=sk->next) 
	{
		if (sk->num == num) 
			return(1);
	}
	return(0);
}


/*
 *	Pick a new socket number
 */
// 随机获取一个端口
unsigned short get_new_socknum(struct proto *prot, unsigned short base)
{
	static int start=0;

	/*
	 * Used to cycle through the port numbers so the
	 * chances of a confused connection drop.
	 */
	 
	int i, j;
	int best = 0;
	int size = 32767; /* a big num. */
	struct sock *sk;
	// 大于1024
	if (base == 0) 
		base = PROT_SOCK+1+(start % 1024);
	if (base <= PROT_SOCK) 
	{
		base += PROT_SOCK+(start % 1024);
	}

	/* Now look through the entire array and try to find an empty ptr. */
	for(i=0; i < SOCK_ARRAY_SIZE; i++) 
	{
		j = 0;
		// 找到一条链表
		sk = prot->sock_array[(i+base+1) &(SOCK_ARRAY_SIZE -1)];
		// 找到链表中的最后一个节点
		while(sk != NULL) 
		{
			sk = sk->next;
			j++;
		}
		// 该链表上还没有节点，说明这个端口还没有被使用过，返回该端口号，更新start变量
		if (j == 0) 
		{
			start =(i+1+start )%1024;
			return(i+base+1);
		}
		/*
			j为本次循环的队列的节点数，best记录新端口所属队列的索引，
			size为本次循环为止节点数最少的队列的节点数，为了避免单个队列过长，
			找可用端口的时候，不仅要找到一个可用的端口，而且尽量保证端口所对
			应的队列不会过长，避免查找的时候比较慢，所以for循环是为了找出哈希链表
			中节点数最少的队列对应的索引。然后往该队列插入一个新的端口节点
		*/
		if (j < size) 
		{
			best = i;
			size = j;
		}
	}

	/* Now make sure the one we want is not in use. */
	// 在一条队列中找到一个未使用的端口号，SOCK_ARRAY_SIZE保证哈希后对应的是同一个队列
	while(sk_inuse(prot, base +best+1)) 
	{
		best += SOCK_ARRAY_SIZE;
	}
	return(best+base+1);
}

/*
 *	Add a socket into the socket tables by number.
 */

void put_sock(unsigned short num, struct sock *sk)
{
	struct sock *sk1;
	struct sock *sk2;
	int mask;
	unsigned long flags;

	sk->num = num;
	sk->next = NULL;
	num = num &(SOCK_ARRAY_SIZE -1);

	/* We can't have an interrupt re-enter here. */
	save_flags(flags);
	cli();
	// 使用的socket数
	sk->prot->inuse += 1;
	// 最多使用的socket数
	if (sk->prot->highestinuse < sk->prot->inuse)
		sk->prot->highestinuse = sk->prot->inuse;
	// 链表为空，sk成为第一个节点
	if (sk->prot->sock_array[num] == NULL) 
	{
		sk->prot->sock_array[num] = sk;
		restore_flags(flags);
		return;
	}
	restore_flags(flags);
	// mask为0xff000000 => 0xffff0000 => 0xffffff00 => 0xffffffff
	for(mask = 0xff000000; mask != 0xffffffff; mask = (mask >> 8) | mask) 
	{
		if ((mask & sk->saddr) &&
		    (mask & sk->saddr) != (mask & 0xffffffff)) 
		{
			mask = mask << 8;
			break;
		}
	}
	cli();
	// 根据端口找到对应的链表,找到对应的位置插入队列
	sk1 = sk->prot->sock_array[num];
	for(sk2 = sk1; sk2 != NULL; sk2=sk2->next) 
	{
		if (!(sk2->saddr & mask)) 
		{
			// 当前是第一个节点
			if (sk2 == sk1) 
			{
				sk->next = sk->prot->sock_array[num];
				sk->prot->sock_array[num] = sk;
				sti();
				return;
			}
			sk->next = sk2;
			sk1->next= sk;
			sti();
			return;
		}
		sk1 = sk2;
	}

	/* Goes at the end. */
	sk->next = NULL;
	sk1->next = sk;
	sti();
}

/*
 *	Remove a socket from the socket tables.
 */

static void remove_sock(struct sock *sk1)
{
	struct sock *sk2;
	unsigned long flags;

	if (!sk1->prot) 
	{
		printk("sock.c: remove_sock: sk1->prot == NULL\n");
		return;
	}

	/* We can't have this changing out from under us. */
	save_flags(flags);
	cli();
	sk2 = sk1->prot->sock_array[sk1->num &(SOCK_ARRAY_SIZE -1)];
	// 是队列的第一个节点
	if (sk2 == sk1) 
	{
		sk1->prot->inuse -= 1;
		sk1->prot->sock_array[sk1->num &(SOCK_ARRAY_SIZE -1)] = sk1->next;
		restore_flags(flags);
		return;
	}
	// 找sk1
	while(sk2 && sk2->next != sk1) 
	{
		sk2 = sk2->next;
	}
	// 找到
	if (sk2) 
	{
		sk1->prot->inuse -= 1;
		sk2->next = sk1->next;
		restore_flags(flags);
		return;
	}
	restore_flags(flags);
}

/*
 *	Destroy an AF_INET socket
 */
 
void destroy_sock(struct sock *sk)
{
	struct sk_buff *skb;

  	sk->inuse = 1;			/* just to be safe. */

  	/* In case it's sleeping somewhere. */
  	if (!sk->dead) 
  		sk->write_space(sk);

  	remove_sock(sk);
  
  	/* Now we can no longer get new packets. */
  	delete_timer(sk);
  	/* Nor send them */
	del_timer(&sk->retransmit_timer);
	
	while ((skb = tcp_dequeue_partial(sk)) != NULL) {
		IS_SKB(skb);
		kfree_skb(skb, FREE_WRITE);
	}

	/* Cleanup up the write buffer. */
  	while((skb = skb_dequeue(&sk->write_queue)) != NULL) {
		IS_SKB(skb);
		kfree_skb(skb, FREE_WRITE);
  	}
  	
  	/*
  	 *	Don't discard received data until the user side kills its
  	 *	half of the socket.
  	 */

	if (sk->dead) 
	{
  		while((skb=skb_dequeue(&sk->receive_queue))!=NULL) 
  		{
		/*
		 * This will take care of closing sockets that were
		 * listening and didn't accept everything.
		 */
			// 处理listen型的socket，监听套接字接收队列里的skb关联的sock结构是一个新建的而不是sk
			if (skb->sk != NULL && skb->sk != sk) 
			{
				IS_SKB(skb);
				skb->sk->dead = 1;
				// 关闭连接
				skb->sk->prot->close(skb->sk, 0);
			}
			IS_SKB(skb);
			kfree_skb(skb, FREE_READ);
		}
	}	

	/* Now we need to clean up the send head. */
	cli();
	// 清空为了重传而缓存的数据包
	for(skb = sk->send_head; skb != NULL; )
	{
		struct sk_buff *skb2;

		/*
		 * We need to remove skb from the transmit queue,
		 * or maybe the arp queue.
		 */
		if (skb->next  && skb->prev) {
/*			printk("destroy_sock: unlinked skb\n");*/
			IS_SKB(skb);
			skb_unlink(skb);
		}
		skb->dev = NULL;
		// unlink后link3指针仍然指向下一个skb节点
		skb2 = skb->link3;
		kfree_skb(skb, FREE_WRITE);
		skb = skb2;
	}
	sk->send_head = NULL;
	sti();

  	/* And now the backlog. */
	// 还没来得及移到receive_queue队列的而缓存在back_log队列的skb
  	while((skb=skb_dequeue(&sk->back_log))!=NULL) 
  	{
		/* this should never happen. */
/*		printk("cleaning back_log\n");*/
		kfree_skb(skb, FREE_READ);
	}

	/* Now if it has a half accepted/ closed socket. */
	if (sk->pair) 
	{
		sk->pair->dead = 1;
		sk->pair->prot->close(sk->pair, 0);
		sk->pair = NULL;
  	}

	/*
	 * Now if everything is gone we can free the socket
	 * structure, otherwise we need to keep it around until
	 * everything is gone.
	 */

	  if (sk->dead && sk->rmem_alloc == 0 && sk->wmem_alloc == 0) 
	  {
		kfree_s((void *)sk,sizeof(*sk));
	  } 
	  else 
	  {
		/* this should never happen. */
		/* actually it can if an ack has just been sent. */
		sk->destroy = 1;
		sk->ack_backlog = 0;
		sk->inuse = 0;
		reset_timer(sk, TIME_DESTROY, SOCK_DESTROY_TIME);
  	}
}

/*
 *	The routines beyond this point handle the behaviour of an AF_INET
 *	socket object. Mostly it punts to the subprotocols of IP to do
 *	the work.
 */
 
static int inet_fcntl(struct socket *sock, unsigned int cmd, unsigned long arg)
{
	struct sock *sk;

	sk = (struct sock *) sock->data;

	switch(cmd) 
	{
		case F_SETOWN:
			/*
			 * This is a little restrictive, but it's the only
			 * way to make sure that you can't send a sigurg to
			 * another process.
			 */
			if (!suser() && current->pgrp != -arg &&
				current->pid != arg) return(-EPERM);
			// 设置进程或进程组号
			sk->proc = arg;
			return(0);
		case F_GETOWN:
			return(sk->proc);
		default:
			return(-EINVAL);
	}
}

/*
 *	Set socket options on an inet socket.
 */
 
static int inet_setsockopt(struct socket *sock, int level, int optname,
		    char *optval, int optlen)
{
  	struct sock *sk = (struct sock *) sock->data;  
	if (level == SOL_SOCKET)
		return sock_setsockopt(sk,level,optname,optval,optlen);
	if (sk->prot->setsockopt==NULL)
		return(-EOPNOTSUPP);
	else
		return sk->prot->setsockopt(sk,level,optname,optval,optlen);
}

/*
 *	Get a socket option on an AF_INET socket.
 */

static int inet_getsockopt(struct socket *sock, int level, int optname,
		    char *optval, int *optlen)
{
  	struct sock *sk = (struct sock *) sock->data;  	
  	if (level == SOL_SOCKET) 
  		return sock_getsockopt(sk,level,optname,optval,optlen);
  	if(sk->prot->getsockopt==NULL)  	
  		return(-EOPNOTSUPP);
  	else
  		return sk->prot->getsockopt(sk,level,optname,optval,optlen);
}

/*
 *	Automatically bind an unbound socket.
 */
// 绑定一个随机的端口，更新sk的源端口字段，并把sk挂载到端口对应的队列中
static int inet_autobind(struct sock *sk)
{
	/* We may need to bind the socket. */
	if (sk->num == 0) 
	{
		sk->num = get_new_socknum(sk->prot, 0);
		if (sk->num == 0) 
			return(-EAGAIN);
		put_sock(sk->num, sk);
		sk->dummy_th.source = ntohs(sk->num);
	}
	return 0;
}

/*
 *	Move a socket into listening state.
 */
 
static int inet_listen(struct socket *sock, int backlog)
{
	struct sock *sk = (struct sock *) sock->data;
	// 如果没有绑定端口则绑定一个，并把sock加到sock_array中
	if(inet_autobind(sk)!=0)
		return -EAGAIN;

	/* We might as well re use these. */ 
	/*
	 * note that the backlog is "unsigned char", so truncate it
	 * somewhere. We might as well truncate it to what everybody
	 * else does..
	 */
	if ((unsigned) backlog > 128)
		backlog = 128;
	// 设置syn+已连接队列的最大长度，在tcp.c中用到
	sk->max_ack_backlog = backlog;
	// 防止多次调用listen
	if (sk->state != TCP_LISTEN)
	{	
		// syn+已连接队列长度
		sk->ack_backlog = 0;
		sk->state = TCP_LISTEN;
	}
	return(0);
}

/*
 *	Default callbacks for user INET sockets. These just wake up
 *	the user owning the socket.
 */

static void def_callback1(struct sock *sk)
{
	if(!sk->dead)
		wake_up_interruptible(sk->sleep);
}

static void def_callback2(struct sock *sk,int len)
{
	if(!sk->dead)
	{
		wake_up_interruptible(sk->sleep);
		sock_wake_async(sk->socket, 1);
	}
}

static void def_callback3(struct sock *sk)
{
	if(!sk->dead)
	{
		wake_up_interruptible(sk->sleep);
		sock_wake_async(sk->socket, 2);
	}
}

/*
 *	Create an inet socket.
 *
 *	FIXME: Gcc would generate much better code if we set the parameters
 *	up in in-memory structure order. Gcc68K even more so
 */
// 创建一个sock结构体，和socket结构体互相关联
static int inet_create(struct socket *sock, int protocol)
{
	struct sock *sk;
	struct proto *prot;
	int err;
	// 分配一个sock结构体
	sk = (struct sock *) kmalloc(sizeof(*sk), GFP_KERNEL);
	if (sk == NULL) 
		return(-ENOBUFS);
	sk->num = 0;
	sk->reuse = 0;
	switch(sock->type) 
	{
		case SOCK_STREAM:
		case SOCK_SEQPACKET:
			if (protocol && protocol != IPPROTO_TCP) 
			{
				kfree_s((void *)sk, sizeof(*sk));
				return(-EPROTONOSUPPORT);
			}
			protocol = IPPROTO_TCP;
			sk->no_check = TCP_NO_CHECK;
			// 函数集
			prot = &tcp_prot;
			break;

		case SOCK_DGRAM:
			if (protocol && protocol != IPPROTO_UDP) 
			{
				kfree_s((void *)sk, sizeof(*sk));
				return(-EPROTONOSUPPORT);
			}
			protocol = IPPROTO_UDP;
			sk->no_check = UDP_NO_CHECK;
			prot=&udp_prot;
			break;
		// 下面两种类型需要root身份
		case SOCK_RAW:
			if (!suser()) 
			{
				kfree_s((void *)sk, sizeof(*sk));
				return(-EPERM);
			}
			if (!protocol) 
			{
				kfree_s((void *)sk, sizeof(*sk));
				return(-EPROTONOSUPPORT);
			}
			prot = &raw_prot;
			sk->reuse = 1;
			sk->no_check = 0;	/*
						 * Doesn't matter no checksum is
						 * performed anyway.
						 */
			// mac头收到数据包时对应数据包的协议是否等于protocol
			sk->num = protocol;
			break;

		case SOCK_PACKET:
			if (!suser()) 
			{
				kfree_s((void *)sk, sizeof(*sk));
				return(-EPERM);
			}
			if (!protocol) 
			{
				kfree_s((void *)sk, sizeof(*sk));
				return(-EPROTONOSUPPORT);
			}
			prot = &packet_prot;
			sk->reuse = 1;
			sk->no_check = 0;	/* Doesn't matter no checksum is
						 * performed anyway.
						 */
			sk->num = protocol;
			break;

		default:
			kfree_s((void *)sk, sizeof(*sk));
			return(-ESOCKTNOSUPPORT);
	}
	// sock结构体的socket字段指向上层的socket结构体
	sk->socket = sock;
#ifdef CONFIG_TCP_NAGLE_OFF
	sk->nonagle = 1;
#else    
	sk->nonagle = 0;
#endif  
	sk->type = sock->type;
	sk->stamp.tv_sec=0;
	sk->protocol = protocol;
	sk->wmem_alloc = 0;
	sk->rmem_alloc = 0;
	sk->sndbuf = SK_WMEM_MAX;
	sk->rcvbuf = SK_RMEM_MAX;
	sk->pair = NULL;
	sk->opt = NULL;
	sk->write_seq = 0;
	sk->acked_seq = 0;
	sk->copied_seq = 0;
	sk->fin_seq = 0;
	sk->urg_seq = 0;
	sk->urg_data = 0;
	sk->proc = 0;
	sk->rtt = 0;				/*TCP_WRITE_TIME << 3;*/
	sk->rto = TCP_TIMEOUT_INIT;		/*TCP_WRITE_TIME*/
	sk->mdev = 0;
	sk->backoff = 0;
	sk->packets_out = 0;
	sk->cong_window = 1; /* start with only sending one packet at a time. */
	sk->cong_count = 0;
	sk->ssthresh = 0;
	sk->max_window = 0;
	sk->urginline = 0;
	sk->intr = 0;
	sk->linger = 0;
	sk->destroy = 0;
	sk->priority = 1;
	sk->shutdown = 0;
	sk->keepopen = 0;
	sk->zapped = 0;
	sk->done = 0;
	sk->ack_backlog = 0;
	sk->window = 0;
	sk->bytes_rcv = 0;
	sk->state = TCP_CLOSE;
	sk->dead = 0;
	sk->ack_timed = 0;
	sk->partial = NULL;
	sk->user_mss = 0;
	sk->debug = 0;

	/* this is how many unacked bytes we will accept for this socket.  */
	sk->max_unacked = 2048; /* needs to be at most 2 full packets. */

	/* how many packets we should send before forcing an ack. 
	   if this is set to zero it is the same as sk->delay_acks = 0 */
	sk->max_ack_backlog = 0;
	sk->inuse = 0;
	sk->delay_acks = 0;
	skb_queue_head_init(&sk->write_queue);
	skb_queue_head_init(&sk->receive_queue);
	sk->mtu = 576;
	// 下层的操作函数集
	sk->prot = prot;
	// 来自socket结构体的wait字段，wait字段来自inode的wait字段
	sk->sleep = sock->wait;
	sk->daddr = 0;
	sk->saddr = 0 /* ip_my_addr() */;
	sk->err = 0;
	sk->next = NULL;
	sk->pair = NULL;
	sk->send_tail = NULL;
	sk->send_head = NULL;
	sk->timeout = 0;
	sk->broadcast = 0;
	sk->localroute = 0;
	init_timer(&sk->timer);
	init_timer(&sk->retransmit_timer);
	sk->timer.data = (unsigned long)sk;
	sk->timer.function = &net_timer;
	skb_queue_head_init(&sk->back_log);
	sk->blog = 0;
	// socket结构体的data字段指向底层的sock结构体
	sock->data =(void *) sk;
	// 初始化tcp头
	sk->dummy_th.doff = sizeof(sk->dummy_th)/4;
	sk->dummy_th.res1=0;
	sk->dummy_th.res2=0;
	sk->dummy_th.urg_ptr = 0;
	sk->dummy_th.fin = 0;
	sk->dummy_th.syn = 0;
	sk->dummy_th.rst = 0;
	sk->dummy_th.psh = 0;
	sk->dummy_th.ack = 0;
	sk->dummy_th.urg = 0;
	sk->dummy_th.dest = 0;
	sk->ip_tos=0;
	sk->ip_ttl=64;
#ifdef CONFIG_IP_MULTICAST
	sk->ip_mc_loop=1;
	sk->ip_mc_ttl=1;
	*sk->ip_mc_name=0;
	sk->ip_mc_list=NULL;
#endif
  	// 下面两个函数用于阻塞型的网络函数被阻塞时，一旦底层条件符合，则回调下面的函数通知上层，即唤醒进程
	sk->state_change = def_callback1;
	sk->data_ready = def_callback2;
	sk->write_space = def_callback3;
	sk->error_report = def_callback1;

	if (sk->num) 
	{
	/*
	 * It assumes that any protocol which allows
	 * the user to assign a number at socket
	 * creation time automatically
	 * shares.
	 */
		// 根据端口，把sock结构体放到下层协议的sock_srray数组
		put_sock(sk->num, sk);
		sk->dummy_th.source = ntohs(sk->num);
	}
	// 执行底层的初始化函数，tcp和udp都没有init函数,packet协议有，他会往mac头链表加入一个节点，用于监听感兴趣的数据
	if (sk->prot->init) 
	{
		err = sk->prot->init(sk);
		if (err != 0) 
		{
			destroy_sock(sk);
			return(err);
		}
	}
	return(0);
}


/*
 *	Duplicate a socket.
 */
 
static int inet_dup(struct socket *newsock, struct socket *oldsock)
{
	return(inet_create(newsock,((struct sock *)(oldsock->data))->protocol));
}

/*
 * Return 1 if we still have things to send in our buffers.
 */
static inline int closing(struct sock * sk)
{
	switch (sk->state) {
		case TCP_FIN_WAIT1:
		case TCP_CLOSING:
		case TCP_LAST_ACK:
			return 1;
	}
	return 0;
}


/*
 *	The peer socket should always be NULL (or else). When we call this
 *	function we are destroying the object and from then on nobody
 *	should refer to it.
 */
 
static int inet_release(struct socket *sock, struct socket *peer)
{
	struct sock *sk = (struct sock *) sock->data;
	if (sk == NULL) 
		return(0);

	sk->state_change(sk);

	/* Start closing the connection.  This may take a while. */

#ifdef CONFIG_IP_MULTICAST
	/* Applications forget to leave groups before exiting */
	ip_mc_drop_socket(sk);
#endif
	/*
	 * If linger is set, we don't return until the close
	 * is complete.  Other wise we return immediately. The
	 * actually closing is done the same either way.
	 *
	 * If the close is due to the process exiting, we never
	 * linger..
	 */

	if (sk->linger == 0 || (current->flags & PF_EXITING))
	{
		sk->prot->close(sk,0);
		sk->dead = 1;
	} 
	else 
	{
		sk->prot->close(sk, 0);
		cli();
		if (sk->lingertime)
			current->timeout = jiffies + HZ*sk->lingertime;
		while(closing(sk) && current->timeout>0) 
		{
			interruptible_sleep_on(sk->sleep);
			if (current->signal & ~current->blocked) 
			{
				break;
#if 0
				/* not working now - closes can't be restarted */
				sti();
				current->timeout=0;
				return(-ERESTARTSYS);
#endif
			}
		}
		current->timeout=0;
		sti();
		sk->dead = 1;
	}
	sk->inuse = 1;

	/* This will destroy it. */
	sock->data = NULL;
	release_sock(sk);
	sk->socket = NULL;
	return(0);
}


/* this needs to be changed to disallow
   the rebinding of sockets.   What error
   should it return? */
// 给socket绑定一个地址
static int inet_bind(struct socket *sock, struct sockaddr *uaddr,
	       int addr_len)
{
	struct sockaddr_in *addr=(struct sockaddr_in *)uaddr;
	struct sock *sk=(struct sock *)sock->data, *sk2;
	unsigned short snum = 0 /* Stoopid compiler.. this IS ok */;
	int chk_addr_ret;

	/* check this error. */
	if (sk->state != TCP_CLOSE)
		return(-EIO);
	if(addr_len<sizeof(struct sockaddr_in))
		return -EINVAL;
	// raw协议的这些数据由用户填充	
	if(sock->type != SOCK_RAW)
	{	// 已经绑定了端口
		if (sk->num != 0) 
			return(-EINVAL);
		
		snum = ntohs(addr->sin_port);

		/*
		 * We can't just leave the socket bound wherever it is, it might
		 * be bound to a privileged port. However, since there seems to
		 * be a bug here, we will leave it if the port is not privileged.
		 */
		// 端口无效则随机获取一个非root才能使用的端口
		if (snum == 0) 
		{
			snum = get_new_socknum(sk->prot, 0);
		}
		// 小于1024的端口需要超级用户权限
		if (snum < PROT_SOCK && !suser()) 
			return(-EACCES);
	}
	// 判断ip
	chk_addr_ret = ip_chk_addr(addr->sin_addr.s_addr);
	if (addr->sin_addr.s_addr != 0 && chk_addr_ret != IS_MYADDR && chk_addr_ret != IS_MULTICAST)
		return(-EADDRNOTAVAIL);	/* Source address MUST be ours! */
	  	
	if (chk_addr_ret || addr->sin_addr.s_addr == 0)
		sk->saddr = addr->sin_addr.s_addr;
	
	if(sock->type != SOCK_RAW)
	{
		/* Make sure we are allowed to bind here. */
		cli();
		for(sk2 = sk->prot->sock_array[snum & (SOCK_ARRAY_SIZE -1)];
					sk2 != NULL; sk2 = sk2->next) 
		{
		/* should be below! */
			if (sk2->num != snum) 
				continue;
			// 端口已经被使用，没有设置可重用标记，比如断开了解后在2msl内是否可以重用
			if (!sk->reuse)
			{
				sti();
				return(-EADDRINUSE);
			}
			
			if (sk2->num != snum) 
				continue;		/* more than one */
			if (sk2->saddr != sk->saddr) 
				continue;	/* socket per slot ! -FB */
			// 被监听的端口不能同时被使用
			if (!sk2->reuse || sk2->state==TCP_LISTEN) 
			{
				sti();
				return(-EADDRINUSE);
			}
		}
		sti();
		// 保证该sk不在sock_array队列里
		remove_sock(sk);
		// 挂载到sock_array里
		put_sock(snum, sk);
		// tcp头中的源端口
		sk->dummy_th.source = ntohs(sk->num);
		sk->daddr = 0;
		sk->dummy_th.dest = 0;
	}
	return(0);
}

/*
 *	Handle sk->err properly. The cli/sti matter.
 */
 
static int inet_error(struct sock *sk)
{
	unsigned long flags;
	int err;
	save_flags(flags);
	cli();	
	err=sk->err;
	sk->err=0;
	restore_flags(flags);
	return -err;
}

/*
 *	Connect to a remote host. There is regrettably still a little
 *	TCP 'magic' in here.
 */
 
static int inet_connect(struct socket *sock, struct sockaddr * uaddr,
		  int addr_len, int flags)
{
	struct sock *sk=(struct sock *)sock->data;
	int err;
	sock->conn = NULL;

	if (sock->state == SS_CONNECTING && tcp_connected(sk->state))
	{
		sock->state = SS_CONNECTED;
		/* Connection completing after a connect/EINPROGRESS/select/connect */
		return 0;	/* Rock and roll */
	}
	// 正在连接，并且是非阻塞的，直接返回
	if (sock->state == SS_CONNECTING && sk->protocol == IPPROTO_TCP && (flags & O_NONBLOCK)) {
		if (sk->err != 0)
		{
			err=sk->err;
			sk->err=0;
			return -err;
		}
		return -EALREADY;	/* Connecting is currently in progress */
	}
	
	if (sock->state != SS_CONNECTING) 
	{
		/* We may need to bind the socket. */
		if(inet_autobind(sk)!=0)
			return(-EAGAIN);
		if (sk->prot->connect == NULL) 
			return(-EOPNOTSUPP);
		// 调用底层的连接函数，发一个syn包
		err = sk->prot->connect(sk, (struct sockaddr_in *)uaddr, addr_len);
		if (err < 0) 
			return(err);
		// 发送成功设置状态为连接中
  		sock->state = SS_CONNECTING;
	}
	
	if (sk->state > TCP_FIN_WAIT2 && sock->state==SS_CONNECTING)
	{
		sock->state=SS_UNCONNECTED;
		cli();
		err=sk->err;
		sk->err=0;
		sti();
		return -err;
	}
	// 还没建立连接成功并且是非阻塞的方式，直接返回
	if (sk->state != TCP_ESTABLISHED &&(flags & O_NONBLOCK)) 
	  	return(-EINPROGRESS);

	cli(); /* avoid the race condition */
	// 连接建立中，阻塞当前进程
	while(sk->state == TCP_SYN_SENT || sk->state == TCP_SYN_RECV) 
	{
		interruptible_sleep_on(sk->sleep);
		if (current->signal & ~current->blocked) 
		{
			sti();
			return(-ERESTARTSYS);
		}
		/* This fixes a nasty in the tcp/ip code. There is a hideous hassle with
		   icmp error packets wanting to close a tcp or udp socket. */
		// 连接失败
		if(sk->err && sk->protocol == IPPROTO_TCP)
		{
			sti();
			sock->state = SS_UNCONNECTED;
			err = -sk->err;
			sk->err=0;
			return err; /* set by tcp_err() */
		}
	}
	sti();
	// 连接建立
	sock->state = SS_CONNECTED;

	if (sk->state != TCP_ESTABLISHED && sk->err) 
	{
		sock->state = SS_UNCONNECTED;
		err=sk->err;
		sk->err=0;
		return(-err);
	}
	return(0);
}


static int inet_socketpair(struct socket *sock1, struct socket *sock2)
{
	 return(-EOPNOTSUPP);
}
/*
 *	Accept a pending connection. The TCP layer now gives BSD semantics.
 */

static int inet_accept(struct socket *sock, struct socket *newsock, int flags)
{
	struct sock *sk1, *sk2;
	int err;

	sk1 = (struct sock *) sock->data;

	/*
	 * We've been passed an extra socket.
	 * We need to free it up because the tcp module creates
	 * its own when it accepts one.
	 */
	// 销毁新socket结构体中的sock结构体,调用底层的accept会返回一个新的sock
	if (newsock->data)
	{
	  	struct sock *sk=(struct sock *)newsock->data;
	  	newsock->data=NULL;
	  	sk->dead = 1;
	  	destroy_sock(sk);
	}
  
	if (sk1->prot->accept == NULL) 
		return(-EOPNOTSUPP);

	/* Restore the state if we have been interrupted, and then returned. */
	// 
	if (sk1->pair != NULL ) 
	{
		sk2 = sk1->pair;
		sk1->pair = NULL;
	} 
	else
	{	// 返回一个新的sock结构体
		sk2 = sk1->prot->accept(sk1,flags);
		if (sk2 == NULL) 
		{
			if (sk1->err <= 0)
				printk("Warning sock.c:sk1->err <= 0.  Returning non-error.\n");
			err=sk1->err;
			sk1->err=0;
			return(-err);
		}
	}
	// 互相关联
	newsock->data = (void *)sk2;
	// 复制socket结构的wait字段，用于控制进程的阻塞和唤醒
	sk2->sleep = newsock->wait;
	sk2->socket = newsock;
	newsock->conn = NULL;
	if (flags & O_NONBLOCK) 
		return(0);

	cli(); /* avoid the race. */
	// sock是接收syn状态则阻塞当前进程
	while(sk2->state == TCP_SYN_RECV) 
	{
		interruptible_sleep_on(sk2->sleep);
		if (current->signal & ~current->blocked) 
		{
			sti();
			sk1->pair = sk2;
			sk2->sleep = NULL;
			sk2->socket=NULL;
			newsock->data = NULL;
			return(-ERESTARTSYS);
		}
	}
	sti();
	
	if (sk2->state != TCP_ESTABLISHED && sk2->err > 0) 
	{
		err = -sk2->err;
		sk2->err=0;
		sk2->dead=1;	/* ANK */
		destroy_sock(sk2);
		newsock->data = NULL;
		return(err);
	}
	// 设置sock为已经建立连接状态
	newsock->state = SS_CONNECTED;
	return(0);
}


/*
 *	This does both peername and sockname.
 */
 
static int inet_getname(struct socket *sock, struct sockaddr *uaddr,
		 int *uaddr_len, int peer)
{
	struct sockaddr_in *sin=(struct sockaddr_in *)uaddr;
	struct sock *sk;
  
	sin->sin_family = AF_INET;
	sk = (struct sock *) sock->data;
	if (peer) 
	{
		if (!tcp_connected(sk->state)) 
			return(-ENOTCONN);
		sin->sin_port = sk->dummy_th.dest;
		sin->sin_addr.s_addr = sk->daddr;
	} 
	else 
	{
		sin->sin_port = sk->dummy_th.source;
		if (sk->saddr == 0) 
			sin->sin_addr.s_addr = ip_my_addr();
		else 
			sin->sin_addr.s_addr = sk->saddr;
	}
	*uaddr_len = sizeof(*sin);
	return(0);
}


/*
 *	The assorted BSD I/O operations
 */

static int inet_recvfrom(struct socket *sock, void *ubuf, int size, int noblock, 
		   unsigned flags, struct sockaddr *sin, int *addr_len )
{
	struct sock *sk = (struct sock *) sock->data;
	
	if (sk->prot->recvfrom == NULL) 
		return(-EOPNOTSUPP);
	if(sk->err)
		return inet_error(sk);
	/* We may need to bind the socket. */
	if(inet_autobind(sk)!=0)
		return(-EAGAIN);
	return(sk->prot->recvfrom(sk, (unsigned char *) ubuf, size, noblock, flags,
			     (struct sockaddr_in*)sin, addr_len));
}


static int inet_recv(struct socket *sock, void *ubuf, int size, int noblock,
	  unsigned flags)
{
	/* BSD explicitly states these are the same - so we do it this way to be sure */
	return inet_recvfrom(sock,ubuf,size,noblock,flags,NULL,NULL);
}

static int inet_read(struct socket *sock, char *ubuf, int size, int noblock)
{
	struct sock *sk = (struct sock *) sock->data;
	
	if(sk->err)
		return inet_error(sk);
	/* We may need to bind the socket. */
	if(inet_autobind(sk))
		return(-EAGAIN);	
	return(sk->prot->read(sk, (unsigned char *) ubuf, size, noblock, 0));
}

static int inet_send(struct socket *sock, void *ubuf, int size, int noblock, 
	       unsigned flags)
{
	struct sock *sk = (struct sock *) sock->data;
	if (sk->shutdown & SEND_SHUTDOWN) 
	{
		send_sig(SIGPIPE, current, 1);
		return(-EPIPE);
	}
	if(sk->err)
		return inet_error(sk);
	/* We may need to bind the socket. */
	if(inet_autobind(sk)!=0)
		return(-EAGAIN);
	return(sk->prot->write(sk, (unsigned char *) ubuf, size, noblock, flags));
}

static int inet_write(struct socket *sock, char *ubuf, int size, int noblock)
{
	return inet_send(sock,ubuf,size,noblock,0);
}

static int inet_sendto(struct socket *sock, void *ubuf, int size, int noblock, 
	    unsigned flags, struct sockaddr *sin, int addr_len)
{
	struct sock *sk = (struct sock *) sock->data;
	if (sk->shutdown & SEND_SHUTDOWN) 
	{
		send_sig(SIGPIPE, current, 1);
		return(-EPIPE);
	}
	if (sk->prot->sendto == NULL) 
		return(-EOPNOTSUPP);
	if(sk->err)
		return inet_error(sk);
	/* We may need to bind the socket. */
	if(inet_autobind(sk)!=0)
		return -EAGAIN;
	return(sk->prot->sendto(sk, (unsigned char *) ubuf, size, noblock, flags, 
			   (struct sockaddr_in *)sin, addr_len));
}


static int inet_shutdown(struct socket *sock, int how)
{
	struct sock *sk=(struct sock*)sock->data;

	/*
	 * This should really check to make sure
	 * the socket is a TCP socket. (WHY AC...)
	 */
	how++; /* maps 0->1 has the advantage of making bit 1 rcvs and
		       1->2 bit 2 snds.
		       2->3 */
	if ((how & ~SHUTDOWN_MASK) || how==0)	/* MAXINT->0 */
		return(-EINVAL);
	if (sock->state == SS_CONNECTING && sk->state == TCP_ESTABLISHED)
		sock->state = SS_CONNECTED;
	if (!tcp_connected(sk->state)) 
		return(-ENOTCONN);
	sk->shutdown |= how;
	if (sk->prot->shutdown)
		sk->prot->shutdown(sk, how);
	return(0);
}


static int inet_select(struct socket *sock, int sel_type, select_table *wait )
{
	struct sock *sk=(struct sock *) sock->data;
	if (sk->prot->select == NULL) 
	{
		return(0);
	}
	return(sk->prot->select(sk, sel_type, wait));
}

/*
 *	ioctl() calls you can issue on an INET socket. Most of these are
 *	device configuration and stuff and very rarely used. Some ioctls
 *	pass on to the socket itself.
 *
 *	NOTE: I like the idea of a module for the config stuff. ie ifconfig
 *	loads the devconfigure module does its configuring and unloads it.
 *	There's a good 20K of config code hanging around the kernel.
 */

static int inet_ioctl(struct socket *sock, unsigned int cmd, unsigned long arg)
{
	struct sock *sk=(struct sock *)sock->data;
	int err, pid;

	switch(cmd) 
	{
		case FIOSETOWN:
		case SIOCSPGRP:
			err=verify_area(VERIFY_READ,(int *)arg,sizeof(long));
			if(err)
				return err;
			pid = get_fs_long((int *) arg);
			/* see inet_fcntl */
			if (current->pid != pid && current->pgrp != -pid && !suser())
				return -EPERM;
			sk->proc = pid;
			return(0);
		case FIOGETOWN:
		case SIOCGPGRP:
			err=verify_area(VERIFY_WRITE,(void *) arg, sizeof(long));
			if(err)
				return err;
			put_fs_long(sk->proc,(int *)arg);
			return(0);			
		case SIOCGSTAMP:
			if(sk->stamp.tv_sec==0)
				return -ENOENT;
			err=verify_area(VERIFY_WRITE,(void *)arg,sizeof(struct timeval));
			if(err)
				return err;
			memcpy_tofs((void *)arg,&sk->stamp,sizeof(struct timeval));
			return 0;
		case SIOCADDRT: case SIOCADDRTOLD:
		case SIOCDELRT: case SIOCDELRTOLD:
			return(ip_rt_ioctl(cmd,(void *) arg));
		case SIOCDARP:
		case SIOCGARP:
		case SIOCSARP:
			return(arp_ioctl(cmd,(void *) arg));
#ifdef CONFIG_INET_RARP			
		case SIOCDRARP:
		case SIOCGRARP:
		case SIOCSRARP:
			return(rarp_ioctl(cmd,(void *) arg));
#endif
		case SIOCGIFCONF:
		case SIOCGIFFLAGS:
		case SIOCSIFFLAGS:
		case SIOCGIFADDR:
		case SIOCSIFADDR:

/* begin multicast support change */
		case SIOCADDMULTI:
		case SIOCDELMULTI:
/* end multicast support change */
		
		case SIOCGIFDSTADDR:
		case SIOCSIFDSTADDR:
		case SIOCGIFBRDADDR:
		case SIOCSIFBRDADDR:
		case SIOCGIFNETMASK:
		case SIOCSIFNETMASK:
		case SIOCGIFMETRIC:
		case SIOCSIFMETRIC:
		case SIOCGIFMEM:
		case SIOCSIFMEM:
		case SIOCGIFMTU:
		case SIOCSIFMTU:
		case SIOCSIFLINK:
		case SIOCGIFHWADDR:
		case SIOCSIFHWADDR:
		case OLD_SIOCGIFHWADDR:
		case SIOCSIFMAP:
		case SIOCGIFMAP:
		case SIOCSIFSLAVE:
		case SIOCGIFSLAVE:
			return(dev_ioctl(cmd,(void *) arg));

		default:
			if ((cmd >= SIOCDEVPRIVATE) &&
			   (cmd <= (SIOCDEVPRIVATE + 15)))
				return(dev_ioctl(cmd,(void *) arg));

			if (sk->prot->ioctl==NULL) 
				return(-EINVAL);
			return(sk->prot->ioctl(sk, cmd, arg));
	}
	/*NOTREACHED*/
	return(0);
}

/*
 * This routine must find a socket given a TCP or UDP header.
 * Everything is assumed to be in net order.
 *
 * We give priority to more closely bound ports: if some socket
 * is bound to a particular foreign address, it will get the packet
 * rather than somebody listening to any address..
 */

struct sock *get_sock(struct proto *prot, unsigned short num,
				unsigned long raddr,
				unsigned short rnum, unsigned long laddr)
{
	struct sock *s;
	struct sock *result = NULL;
	int badness = -1;
	unsigned short hnum;

	hnum = ntohs(num);

	/*
	 * SOCK_ARRAY_SIZE must be a power of two.  This will work better
	 * than a prime unless 3 or more sockets end up using the same
	 * array entry.  This should not be a problem because most
	 * well known sockets don't overlap that much, and for
	 * the other ones, we can just be careful about picking our
	 * socket number when we choose an arbitrary one.
	 */

	for(s = prot->sock_array[hnum & (SOCK_ARRAY_SIZE - 1)];
			s != NULL; s = s->next) 
	{
		int score = 0;

		if (s->num != hnum) 
			continue;

		if(s->dead && (s->state == TCP_CLOSE))
			continue;
		/* local address matches? */
		if (s->saddr) {
			if (s->saddr != laddr)
				continue;
			score++;
		}
		/* remote address matches? */
		if (s->daddr) {
			if (s->daddr != raddr)
				continue;
			score++;
		}
		/* remote port matches? */
		if (s->dummy_th.dest) {
			if (s->dummy_th.dest != rnum)
				continue;
			score++;
		}
		/* perfect match? */
		// 全匹配，直接返回
		if (score == 3)
			return s;
		/* no, check if this is the best so far.. */
		if (score <= badness)
			continue;
		// 记录最好的匹配项
		result = s;
		badness = score;
  	}
  	return result;
}

/*
 *	Deliver a datagram to raw sockets.
 */
 
struct sock *get_sock_raw(struct sock *sk, 
				unsigned short num,
				unsigned long raddr,
				unsigned long laddr)
{
	struct sock *s;

	s=sk;

	for(; s != NULL; s = s->next) 
	{
		if (s->num != num) 
			continue;
		if(s->dead && (s->state == TCP_CLOSE))
			continue;
		if(s->daddr && s->daddr!=raddr)
			continue;
 		if(s->saddr  && s->saddr!=laddr)
			continue;
		return(s);
  	}
  	return(NULL);
}

#ifdef CONFIG_IP_MULTICAST
/*
 *	Deliver a datagram to broadcast/multicast sockets.
 */
 
struct sock *get_sock_mcast(struct sock *sk, 
				unsigned short num,
				unsigned long raddr,
				unsigned short rnum, unsigned long laddr)
{
	struct sock *s;
	unsigned short hnum;

	hnum = ntohs(num);

	/*
	 * SOCK_ARRAY_SIZE must be a power of two.  This will work better
	 * than a prime unless 3 or more sockets end up using the same
	 * array entry.  This should not be a problem because most
	 * well known sockets don't overlap that much, and for
	 * the other ones, we can just be careful about picking our
	 * socket number when we choose an arbitrary one.
	 */
	
	s=sk;

	for(; s != NULL; s = s->next) 
	{
		if (s->num != hnum) 
			continue;
		if(s->dead && (s->state == TCP_CLOSE))
			continue;
		if(s->daddr && s->daddr!=raddr)
			continue;
		if (s->dummy_th.dest != rnum && s->dummy_th.dest != 0) 
			continue;
 		if(s->saddr  && s->saddr!=laddr)
			continue;
		return(s);
  	}
  	return(NULL);
}

#endif

static struct proto_ops inet_proto_ops = {
	AF_INET,

	inet_create,
	inet_dup,
	inet_release,
	inet_bind,
	inet_connect,
	inet_socketpair,
	inet_accept,
	inet_getname, 
	inet_read,
	inet_write,
	inet_select,
	inet_ioctl,
	inet_listen,
	inet_send,
	inet_recv,
	inet_sendto,
	inet_recvfrom,
	inet_shutdown,
	inet_setsockopt,
	inet_getsockopt,
	inet_fcntl,
};

extern unsigned long seq_offset;

/*
 *	Called by socket.c on kernel startup.  
 */
 
void inet_proto_init(struct net_proto *pro)
{
	struct inet_protocol *p;
	int i;


	printk("Swansea University Computer Society TCP/IP for NET3.019\n");

	/*
	 *	Tell SOCKET that we are alive... 
	 */
   
  	(void) sock_register(inet_proto_ops.family, &inet_proto_ops);

  	seq_offset = CURRENT_TIME*250;

	/*
	 *	Add all the protocols. 
	 */
	 
	for(i = 0; i < SOCK_ARRAY_SIZE; i++) 
	{
		tcp_prot.sock_array[i] = NULL;
		udp_prot.sock_array[i] = NULL;
		raw_prot.sock_array[i] = NULL;
  	}
	tcp_prot.inuse = 0;
	tcp_prot.highestinuse = 0;
	udp_prot.inuse = 0;
	udp_prot.highestinuse = 0;
	raw_prot.inuse = 0;
	raw_prot.highestinuse = 0;

	printk("IP Protocols: ");
	for(p = inet_protocol_base; p != NULL;) 
	{
		struct inet_protocol *tmp = (struct inet_protocol *) p->next;
		inet_add_protocol(p);
		printk("%s%s",p->name,tmp?", ":"\n");
		p = tmp;
	}
	/*
	 *	Set the ARP module up
	 */
	arp_init();
  	/*
  	 *	Set the IP module up
  	 */
	ip_init();
}

