/*
** Copyright 2005-2012  Solarflare Communications Inc.
**                      7505 Irvine Center Drive, Irvine, CA 92618, USA
** Copyright 2002-2005  Level 5 Networks Inc.
**
** This program is free software; you can redistribute it and/or modify it
** under the terms of version 2 of the GNU General Public License as
** published by the Free Software Foundation.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU General Public License for more details.
*/

/*
** Copyright 2005-2012  Solarflare Communications Inc.
**                      7505 Irvine Center Drive, Irvine, CA 92618, USA
** Copyright 2002-2005  Level 5 Networks Inc.
**
** Redistribution and use in source and binary forms, with or without
** modification, are permitted provided that the following conditions are
** met:
**
** * Redistributions of source code must retain the above copyright notice,
**   this list of conditions and the following disclaimer.
**
** * Redistributions in binary form must reproduce the above copyright
**   notice, this list of conditions and the following disclaimer in the
**   documentation and/or other materials provided with the distribution.
**
** THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS
** IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
** TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
** PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
** HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
** SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED
** TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
** PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
** LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
** NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
** SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/


/**************************************************************************\
*//*! \file
** <L5_PRIVATE L5_HEADER >
** \author  kjm
**  \brief  Onload zero-copy API
**   \date  2011/05/31
**    \cop  (c) Solarflare Communications Ltd.
** </L5_PRIVATE>
*//*
\**************************************************************************/

#ifndef __ONLOAD_ZC_H__
#define __ONLOAD_ZC_H__

#include <sys/uio.h>    // for struct iovec
#include <sys/socket.h> // for struct msghdr

#ifdef __cplusplus
extern "C" {
#endif

/* TODO :
 *  - Zero-copy UDP-TX and TCP-RX
 *  - allow application to signal that fd table checks aren't necessary
 *  - forwarding: zero-copy receive into a buffer, app can then do a
 *    zero-copy send on the same buffer.
 *  - templates: allow app to pre-send portions of a packet
 */
 

/******************************************************************************
 * Data structures
 ******************************************************************************/

/* Opaque pointer to the zc buffer metadata */
struct oo_zc_buf;
typedef struct oo_zc_buf* onload_zc_handle;

#define ONLOAD_ZC_HANDLE_NONZC ((onload_zc_handle)(uintptr_t)(-1))

/* A zc_iovec describes a single buffer */
struct onload_zc_iovec {
  void* iov_base;          /* Address within buffer */
  size_t iov_len;          /* Length of data */
  onload_zc_handle buf;    /* Corresponding (opaque) buffer handle or
                              ONLOAD_ZC_HANDLE_NONZC */ 
  unsigned iov_flags;      /* Not currently used */
};

/* A msg describes an array of iovecs that make up a datagram */
struct onload_zc_msg {
  struct onload_zc_iovec* iov; /* Array of buffers, len zc_msg.msghdr.msg_iovlen */
  struct msghdr msghdr;        /* Message metadata */
};

/* An mmsg describes a message, the socket, and its result such that
 * many can be sent in one go.
 */
struct onload_zc_mmsg {
  struct onload_zc_msg msg;    /* Message */
  int rc;                      /* Result of send/recv operation */
  int fd;                      /* socket to send on */
};


/******************************************************************************
 * Buffer management
 ******************************************************************************/

enum onload_zc_buffer_type_flags {
  ONLOAD_ZC_BUFFER_HDR_NONE = 0x0,
  ONLOAD_ZC_BUFFER_HDR_UDP = 0x1,
  ONLOAD_ZC_BUFFER_HDR_TCP = 0x2,
};

/* onload_zc_alloc_buffers will allocate 1 or more buffers and return
 * details in the supplied iovecs array.
 * 
 * fd needed to indicate stack that these buffers will be used on.
 * The buffers can be used on any socket that shares the same stack as
 * the allocating fd.
 * 
 * flags can be used to indicate what type of socket this buffer will
 * be used on.  This allows space for the relevant headers to be
 * reserved resulting in more efficient sends. With no space reserved
 * (flags=0) headers will be inserted as necessary by onload with a
 * separate buffer; this is also true if insufficient space is
 * reserved (e.g. requested UDP, but actually used for TCP).
 *
 * Returns zero on success, or <0 to indicate an error
 *
 * These functions can only be used with accelerated sockets (those
 * being handled by Onload).  If a socket has been handed over to the
 * kernel stack (e.g. because it has been bound to an address that is
 * not routed over a SFC interface) it will return -ESOCKTNOSUPPORT
 */

extern int onload_zc_alloc_buffers(int fd, struct onload_zc_iovec* iovecs,
                                   int iovecs_len, 
                                   enum onload_zc_buffer_type_flags flags);

/* onload_zc_release_buffers will release 1 or more previously
 * allocated buffers supplied in the bufs array.  This can also be
 * used to free buffers retained by setting ONLOAD_ZC_KEEP in a
 * receive callback.
 *
 * Returns zero on success, or <0 to indicate an error
 */

extern int onload_zc_release_buffers(int fd, onload_zc_handle* bufs, 
                                     int bufs_len);

/* 
 * TODO buffer re-use:
 *  - recv then send, care needed as can cross stacks
 *  - send same buffer multiple times, care needed as state could change
 */


/******************************************************************************
 * Zero-copy send/receive
 ******************************************************************************/

/* onload_zc_recv will call the supplied callback for each message
 * received. On onload_zc_recv_args should have the following fields
 * set:
 * 
 *  - cb set to the callback function pointer
 *  - user_ptr set to point to application state; this is not touched
 *  by onload
 *  - msg.msghdr.msg_control set to an appropriate buffer (if required)
 *  - msg.msghdr.msg_controllen let to length of msg_control
 *  - msg.msghdr.msg_name & msg_namelen set as you would for recvmsg
 *  - flags set to indicate behavior (e.g. ONLOAD_MSG_DONTWAIT)
 *
 * The supplied onload_zc_recv_args structure is passed through to
 * the callback every time the callback is called.
 *
 * Before calling the callback onload will fill in the args.msg.iov
 * with details of the received data, and args.msg.msghdr with the
 * relevant metadata.  args.msg.msghdr.msg_iov is not set by onload
 * and should not be used.
 *
 * When called, the callback should deal with the received data
 * (stored in onload_zc_recv_args.msg.iov) and can indicate the
 * following by setting flags on its return code:
 *
 * - onload should stop processing this set of messages and
 * return from onload_zc_recv() (rc & ONLOAD_ZC_TERMINATE).
 * - onload should transfer ownership of the buffer to the
 * application, as the application wishes to keep it for now and will
 * release or reuse it later (rc & ONLOAD_ZC_KEEP)
 *
 * As the return code is flags-based the application is free to set
 * any combination of these.  If no flags are set onload will continue
 * to process the next message and ownership of the buffer remains
 * with onload.
 * 
 * The callback can access valid cmsg data (if requested by setting
 * socket options and providing a msg_control buffer in the
 * onload_zc_recv_args) in the onload_zc_recv_args.msghdr structure.
 * 
 * args.flags can take ONLOAD_MSG_DONTWAIT to indicate that the call
 * shouldn't block.
 *
 * There are two options for handling data received via the kernel
 * rather than through Onload:
 * 1) Set ONLOAD_MSG_RECV_OS_INLINE in args.flags.  This will result
 * in Onload copying kernel data into oo_zc_bufs and delivering it to
 * the callback as if it had been received via Onload.
 * 2) Do not set ONLOAD_MSG_RECV_OS_INLINE in args.flags.  This will
 * result in Onload return -ENOTEMPTY from the call to
 * onload_zc_recv() if kernel traffic is present.  The caller can then
 * use onload_recvmsg_kernel() to access the kernel traffic.
 *
 * The callbacks are called in the same context that invoked
 * onload_zc_recv().
 * 
 * The callback's flags field will be set to ONLOAD_ZC_MSG_SHARED if
 * the msg is shared with other sockets and the caller should take
 * care not to modify the contents of the iovec.
 *
 * Timeouts are handled by setting the socket SO_RCVTIMEO value.
 * 
 * Returns 0 success or <0 to indicate an error.
 *
 * This function can only be used with accelerated sockets (those
 * being handled by Onload).  If a socket has been handed over to the
 * kernel stack (e.g. because it has been bound to an address that is
 * not routed over a SFC interface) it will return -ESOCKTNOSUPPORT
 */

enum onload_zc_callback_rc {
  ONLOAD_ZC_CONTINUE  = 0x0,
  ONLOAD_ZC_TERMINATE = 0x1,
  ONLOAD_ZC_KEEP      = 0x2, /* Receive callback only */
  ONLOAD_ZC_MODIFIED  = 0x4, /* Filter callback only */
};

/* Flags that can be set in onload_zc_recv_args.flags */
#define ONLOAD_MSG_RECV_OS_INLINE 0x1
#define ONLOAD_MSG_DONTWAIT MSG_DONTWAIT

/* Mask for supported onload_zc_recv_args.flags */
#define ONLOAD_ZC_RECV_FLAGS_MASK (ONLOAD_MSG_DONTWAIT | \
                                   ONLOAD_MSG_RECV_OS_INLINE)

/* Subset of onload_zc_recv_args.flags that are passed through to the
 * kernel when handling non-onloaded datagrams
 */
#define ONLOAD_ZC_RECV_FLAGS_PTHRU_MASK (ONLOAD_MSG_DONTWAIT)

/* Flags that can be set in the callback flags argument
 * 
 * If set then this buffer may be shared with other sockets and the
 * caller should take care not to modify the contents of the iovec
 */
#define ONLOAD_ZC_MSG_SHARED 0x1
#define ONLOAD_ZC_END_OF_BURST 0x2

struct onload_zc_recv_args;

typedef enum onload_zc_callback_rc 
(*onload_zc_recv_callback)(struct onload_zc_recv_args *args, int flags);

struct onload_zc_recv_args {
  struct onload_zc_msg msg;
  onload_zc_recv_callback cb;
  void* user_ptr;
  int flags;
};


extern int onload_zc_recv(int fd, struct onload_zc_recv_args *args);

/* Use onload_recvmsg_kernel() to access packets delivered by
 * kernel/OS rather than Onload, when onload_zc_recv() returns
 * -ENOTEMPTY
 */
extern int onload_recvmsg_kernel(int fd, struct msghdr *msg, int flags);


/* onload_zc_send will send each of the messages supplied in the msgs
 * array using the fd from struct onload_zc_mmsg.  Each message
 * consists of an array of buffers (msgs[i].msg.iov[j].iov_base,
 * buffer length msgs[i].msg.iov[j].iov_len), and the array is of
 * length msgs[i].msg.msghdr.msg_iovlen.  For UDP this array is sent
 * as a single datagram.
 *
 * Onload makes only limited checks for validity on the arguments
 * passed in, so care should be taken to ensure they are correct.
 * Cases such as NULL msgs pointer, zero mlen, or zero iov_len,
 * etc. may result in incorrect application behaviour.
 *
 * TODO flags can take a value that indicates that the send path
 * should be exercised to keep it warm, but no data actually sent.  In
 * this case the application retains ownership of the buffers.
 * 
 * Returns number of messages processed, with the status (e.g. bytes
 * sent or error) of each processed message stored in msgs[i].rc
 * Caller should check each valid msgs[i].rc and compare to expected
 * number of bytes to check how much has been done.
 *
 * For any buffer successfully sent, ownership of the corresponding
 * onload_zc_handle buffer is transferred to Onload and it must not be
 * subsequently used by the application.  For any messages that are
 * not sent (e.g. due to error) ownership of the buffers remains with
 * the application and it must either re-use or free them.
 *
 * This function can only be used with accelerated sockets (those
 * being handled by Onload).  If a socket has been handed over to the
 * kernel stack (e.g. because it has been bound to an address that is
 * not routed over a SFC interface) it will set msgs.rc to
 * -ESOCKTNOSUPPORT
 */

#define ONLOAD_MSG_MORE MSG_MORE
#define ONLOAD_MSG_NOSIGNAL MSG_NOSIGNAL

/* Mask for supported flags */
#define ONLOAD_ZC_SEND_FLAGS_MASK (ONLOAD_MSG_MORE | ONLOAD_MSG_NOSIGNAL)

/* Subset of flags that are passed through to the kernel when
 * handling non-onloaded datagrams
 */ 
#define ONLOAD_ZC_SEND_FLAGS_PTHRU_MASK (ONLOAD_MSG_MORE | ONLOAD_MSG_NOSIGNAL)

extern int onload_zc_send(struct onload_zc_mmsg* msgs, int mlen, int flags);



/******************************************************************************
 * Receive filtering 
 ******************************************************************************/

/* onload_set_recv_filter() will install a filter that can veto the
 * passing of received data to the normal recv/recvmsg/recvmmsg API.
 * This should not be used in conjunction with onload_zc_recv()
 *
 * The callback is invoked once per message and the cb_arg value is
 * passed to the callback along with the message.  The callback's
 * flags argument will be set to ONLOAD_ZC_MSG_SHARED if the msg is
 * shared with other sockets and the caller should take care not to
 * modify the contents of the iovec.
 *
 * The message can be found in msg->iov[], and the iovec is of length
 * msg->msghdr.msg_iovlen.
 *
 * The callback must return either ONLOAD_ZC_CONTINUE to allow the
 * message to be delivered to the application or ONLOAD_ZC_TERMINATE
 * to veto it.  The callback can also set the ONLOAD_ZC_MODIFIED bit
 * of the return code to indicate that it has modified the contents or
 * iovec structure.
 *
 * This function can only be used with accelerated sockets (those
 * being handled by Onload).  If a socket has been handed over to the
 * kernel stack (e.g. because it has been bound to an address that is
 * not routed over a SFC interface) it will return -ESOCKTNOSUPPORT
 */

typedef enum onload_zc_callback_rc 
(*onload_zc_recv_filter_callback)(struct onload_zc_msg *msg, void* arg, 
                                  int flags);

extern int onload_set_recv_filter(int fd, 
                                  onload_zc_recv_filter_callback filter,
                                  void* cb_arg, int flags);



/******************************************************************************
 * Send templates 
 ******************************************************************************/

/* onload_msg_template_*
 * 
 * This set of functions allows the user to specify the bulk of a
 * packet in advance (the send template), then update it and send it
 * when the complete packet contents are known.  If the updates are
 * relatively small this should result in a lower latency send.
 *
 * 
 * onload_msg_template_set takes an array of iovecs to specify the
 * bulk of the packet data. On success, the onload_template_handle
 * pointer is updated to contain the (opaque) handle used to refer to
 * this template in subsequent operations.
 *
 *
 * onload_msg_template_update takes an array of iovecs to describe
 * changes to the base packet given in onload_msg_template_set.  Each
 * of the update iovecs should describe a single change, and contain:
 *
 *  - iov_base set to the start of the new data.
 *
 *  - iov_len set to the length of the update.
 *
 *  - iov_offset set to the offset within the template to update
 *
 * ulen is the length of the updates array (i.e. the number of changes)
 * 
 * complete can optionally be set to 1 to indicate that the packet can
 * now be sent.  To complete and send a template without further
 * updates, set updates=NULL and ulen=0
 *
 * onload_msg_template_update can be called multiple times and
 * updates are cumulative but after completing a template the
 * ownership of the template transfer to onload and it must not be
 * used further by the application.
 *
 * To abort or release a template without sending it call
 * onload_msg_template_release.
 * 
 * All functions return zero on success, or <0 to indicate an error.
 *
 * These functions can only be used with accelerated sockets (those
 * being handled by Onload).  If a socket has been handed over to the
 * kernel stack (e.g. because it has been bound to an address that is
 * not routed over a SFC interface) it will return -ESOCKTNOSUPPORT
 */

/* Opaque pointer to the template metadata */
struct oo_msg_template;
typedef struct oo_msg_template* onload_template_handle;

enum onload_update_flags {
  ONLOAD_UPDATE_OVERWRITE = 0x1,
};

/* An update_iovec describes a single template update */
struct onload_msg_update {
  void* base;                     /* Pointer to new data */
  size_t len;                     /* Length of update */
  off_t offset;                   /* Offset within template to update */ 
  enum onload_update_flags flags; /* Overwrite only currently */
};

extern int onload_msg_template_set(int fd, struct iovec* base_pkt, 
                                   int blen, onload_template_handle* handle);

extern int onload_msg_template_update(onload_template_handle handle, 
                                      struct onload_msg_update* updates, 
                                      int ulen, int complete);

extern int onload_msg_template_release(onload_template_handle handle);


#ifdef __cplusplus
}
#endif

#endif /* __ONLOAD_ZC_H__ */