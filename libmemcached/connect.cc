/*  vim:expandtab:shiftwidth=2:tabstop=2:smarttab:
 * 
 *  Libmemcached library
 *
 *  Copyright (C) 2011 Data Differential, http://datadifferential.com/
 *  Copyright (C) 2006-2010 Brian Aker All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions are
 *  met:
 *
 *      * Redistributions of source code must retain the above copyright
 *  notice, this list of conditions and the following disclaimer.
 *
 *      * Redistributions in binary form must reproduce the above
 *  copyright notice, this list of conditions and the following disclaimer
 *  in the documentation and/or other materials provided with the
 *  distribution.
 *
 *      * The names of its contributors may not be used to endorse or
 *  promote products derived from this software without specific prior
 *  written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *  A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *  OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *  SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *  LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *  DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *  THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *  (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *  OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 *
 * Portions Copyright 2012-2015 Amazon.com, Inc. or its affiliates. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License"). You may not use this file except in
 * compliance with the License. A copy of the License is located at
 *
 *    http://aws.amazon.com/apache2.0/
 *
 * or in the "license" file accompanying this file. This file is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the License for the
 * specific language governing permissions and limitations under the License.
 */


#include <libmemcached/common.h>

#include <cassert>
#include <ctime>
#include <sys/time.h>

static memcached_return_t connect_poll(memcached_server_st *server)
{
  struct pollfd fds[1];
  fds[0].fd= server->fd;
  fds[0].events= POLLOUT;

  size_t loop_max= 5;

  if (server->root->poll_timeout == 0)
  {
    return memcached_set_error(*server, MEMCACHED_TIMEOUT, MEMCACHED_AT);
  }

  while (--loop_max) // Should only loop on cases of ERESTART or EINTR
  {
    int number_of;
    if ((number_of= poll(fds, 1, server->root->connect_timeout)) <= 0)
    {
      if (number_of == -1)
      {
        int local_errno= get_socket_errno(); // We cache in case closesocket() modifies errno
        switch (local_errno)
        {
#ifdef TARGET_OS_LINUX
        case ERESTART:
#endif
        case EINTR:
          continue;

        case EFAULT:
        case ENOMEM:
          return memcached_set_error(*server, MEMCACHED_MEMORY_ALLOCATION_FAILURE, MEMCACHED_AT);

        case EINVAL:
          return memcached_set_error(*server, MEMCACHED_MEMORY_ALLOCATION_FAILURE, MEMCACHED_AT, memcached_literal_param("RLIMIT_NOFILE exceeded, or if OSX the timeout value was invalid"));

        default: // This should not happen
          if (fds[0].revents & POLLERR)
          {
            int err;
            socklen_t len= sizeof(err);
            if (getsockopt(server->fd, SOL_SOCKET, SO_ERROR, &err, &len) == 0)
            {
              if (err == 0)
              {
                // This should never happen, if it does? Punt.  
                continue;
              }
              local_errno= err;
            }
          }

          assert_msg(server->fd != INVALID_SOCKET, "poll() was passed an invalid file descriptor");
          (void)closesocket(server->fd);
          server->fd= INVALID_SOCKET;
          server->state= MEMCACHED_SERVER_STATE_NEW;

          return memcached_set_errno(*server, local_errno, MEMCACHED_AT);
        }
      }
      assert(number_of == 0);

      server->io_wait_count.timeouts++;
      return memcached_set_error(*server, MEMCACHED_TIMEOUT, MEMCACHED_AT);
    }

    if (fds[0].revents & POLLERR or
        fds[0].revents & POLLHUP or 
        fds[0].revents & POLLNVAL)
    {
      int err;
      socklen_t len= sizeof (err);
      if (getsockopt(fds[0].fd, SOL_SOCKET, SO_ERROR, &err, &len) == 0)
      {
        // We check the value to see what happened wth the socket.
        if (err == 0)
        {
          return MEMCACHED_SUCCESS;
        }
        errno= err;
      }

      return memcached_set_errno(*server, err, MEMCACHED_AT);
    }
    assert(fds[0].revents & POLLIN or fds[0].revents & POLLOUT);

    return MEMCACHED_SUCCESS;
  }

  // This should only be possible from ERESTART or EINTR;
  return memcached_set_errno(*server, get_socket_errno(), MEMCACHED_AT);
}

static memcached_return_t set_hostinfo(memcached_server_st *server)
{
  assert(server->type != MEMCACHED_CONNECTION_UNIX_SOCKET);
  if (server->address_info)
  {
    freeaddrinfo(server->address_info);
    server->address_info= NULL;
    server->address_info_next= NULL;
  }

  char str_port[NI_MAXSERV];
  int length= snprintf(str_port, NI_MAXSERV, "%u", (uint32_t)server->port);
  if (length >= NI_MAXSERV or length < 0)
  {
    return MEMCACHED_FAILURE;
  }

  struct addrinfo hints;
  memset(&hints, 0, sizeof(struct addrinfo));

#if 0
  hints.ai_family= AF_INET;
#endif
  if (memcached_is_udp(server->root))
  {
    hints.ai_protocol= IPPROTO_UDP;
    hints.ai_socktype= SOCK_DGRAM;
  }
  else
  {
    hints.ai_socktype= SOCK_STREAM;
    hints.ai_protocol= IPPROTO_TCP;
  }

  server->address_info= NULL;

  char *endpoint;
  if(has_memcached_server_ipaddress(server))
  {
    endpoint = server->ipaddress;
  }
  else
  {
    endpoint  = server->hostname;
  }

  int errcode;
  switch(errcode= getaddrinfo(endpoint, str_port, &hints, &server->address_info))
  {
  case 0:
    break;

  case EAI_AGAIN:
    return memcached_set_error(*server, MEMCACHED_TIMEOUT, MEMCACHED_AT, memcached_string_make_from_cstr(gai_strerror(errcode)));

  case EAI_SYSTEM:
    return memcached_set_errno(*server, errno, MEMCACHED_AT, memcached_literal_param("getaddrinfo(EAI_SYSTEM)"));

  case EAI_BADFLAGS:
    return memcached_set_error(*server, MEMCACHED_INVALID_ARGUMENTS, MEMCACHED_AT, memcached_literal_param("getaddrinfo(EAI_BADFLAGS)"));

  case EAI_MEMORY:
    return memcached_set_error(*server, MEMCACHED_MEMORY_ALLOCATION_FAILURE, MEMCACHED_AT, memcached_literal_param("getaddrinfo(EAI_MEMORY)"));

  default:
    {
      return memcached_set_error(*server, MEMCACHED_HOST_LOOKUP_FAILURE, MEMCACHED_AT, memcached_string_make_from_cstr(gai_strerror(errcode)));
    }
  }
  server->address_info_next= server->address_info;
  server->state= MEMCACHED_SERVER_STATE_ADDRINFO;

  return MEMCACHED_SUCCESS;
}

static inline void set_socket_nonblocking(memcached_server_st *server)
{
#ifdef WIN32
  u_long arg= 1;
  if (ioctlsocket(server->fd, FIONBIO, &arg) == SOCKET_ERROR)
  {
    memcached_set_errno(*server, get_socket_errno(), NULL);
  }
#else
  int flags;

  do
  {
    flags= fcntl(server->fd, F_GETFL, 0);
  } while (flags == -1 && (errno == EINTR || errno == EAGAIN));

  if (flags == -1)
  {
    memcached_set_errno(*server, errno, NULL);
  }
  else if ((flags & O_NONBLOCK) == 0)
  {
    int rval;

    do
    {
      rval= fcntl(server->fd, F_SETFL, flags | O_NONBLOCK);
    } while (rval == -1 && (errno == EINTR or errno == EAGAIN));

    if (rval == -1)
    {
      memcached_set_errno(*server, errno, NULL);
    }
  }
#endif
}

static void set_socket_options(memcached_server_st *server)
{
  assert_msg(server->fd != INVALID_SOCKET, "invalid socket was passed to set_socket_options()");

  if (memcached_is_udp(server->root))
  {
    return;
  }

#ifdef HAVE_SNDTIMEO
  if (server->root->snd_timeout > 0)
  {
    struct timeval waittime;

    waittime.tv_sec= server->root->snd_timeout / 1000000;
    waittime.tv_usec= server->root->snd_timeout % 1000000;

    int error= setsockopt(server->fd, SOL_SOCKET, SO_SNDTIMEO,
                          (char*)&waittime, (socklen_t)sizeof(struct timeval));
    (void)error;
    assert(error == 0);
  }
#endif

#ifdef HAVE_RCVTIMEO
  if (server->root->rcv_timeout > 0)
  {
    struct timeval waittime;

    waittime.tv_sec= server->root->rcv_timeout / 1000000;
    waittime.tv_usec= server->root->rcv_timeout % 1000000;

    int error= setsockopt(server->fd, SOL_SOCKET, SO_RCVTIMEO,
                          (char*)&waittime, (socklen_t)sizeof(struct timeval));
    (void)(error);
    assert(error == 0);
  }
#endif


#if defined(__MACH__) && defined(__APPLE__) || defined(__FreeBSD__)
  {
    int set= 1;
    int error= setsockopt(server->fd, SOL_SOCKET, SO_NOSIGPIPE, (void *)&set, sizeof(int));

    // This is not considered a fatal error
    if (error == -1)
    {
      WATCHPOINT_ERRNO(get_socket_errno());
      perror("setsockopt(SO_NOSIGPIPE)");
    }
  }
#endif

  if (server->root->flags.no_block)
  {
    struct linger linger;

    linger.l_onoff= 1;
    linger.l_linger= 0; /* By default on close() just drop the socket */
    int error= setsockopt(server->fd, SOL_SOCKET, SO_LINGER,
                          &linger, (socklen_t)sizeof(struct linger));
    assert(error == 0);
  }

  if (server->root->flags.tcp_nodelay)
  {
    int flag= 1;

    int error= setsockopt(server->fd, IPPROTO_TCP, TCP_NODELAY,
                          &flag, (socklen_t)sizeof(int));
    assert(error == 0);
  }

  if (server->root->flags.tcp_keepalive)
  {
    int flag= 1;

    int error= setsockopt(server->fd, SOL_SOCKET, SO_KEEPALIVE,
                      &flag, (socklen_t)sizeof(int));
    assert(error == 0);
  }

#ifdef TCP_KEEPIDLE
  if (server->root->tcp_keepidle > 0)
  {
    int error= setsockopt(server->fd, IPPROTO_TCP, TCP_KEEPIDLE,
                          &server->root->tcp_keepidle, (socklen_t)sizeof(int));
    assert(error == 0);
  }
#endif

  if (server->root->send_size > 0)
  {
    int error= setsockopt(server->fd, SOL_SOCKET, SO_SNDBUF,
                          &server->root->send_size, (socklen_t)sizeof(int));
    assert(error == 0);
  }

  if (server->root->recv_size > 0)
  {
    int error= setsockopt(server->fd, SOL_SOCKET, SO_RCVBUF,
                          &server->root->recv_size, (socklen_t)sizeof(int));
    assert(error == 0);
  }


  /* libmemcached will always use nonblocking IO to avoid write deadlocks */
  set_socket_nonblocking(server);
}

static memcached_return_t unix_socket_connect(memcached_server_st *server)
{
#ifndef WIN32
  WATCHPOINT_ASSERT(server->fd == INVALID_SOCKET);

  if ((server->fd= socket(AF_UNIX, SOCK_STREAM, 0)) < 0)
  {
    memcached_set_errno(*server, errno, NULL);
    return MEMCACHED_CONNECTION_FAILURE;
  }

  struct sockaddr_un servAddr;

  memset(&servAddr, 0, sizeof (struct sockaddr_un));
  servAddr.sun_family= AF_UNIX;
  strncpy(servAddr.sun_path, server->hostname, sizeof(servAddr.sun_path)); /* Copy filename */

  do {
    if (connect(server->fd, (struct sockaddr *)&servAddr, sizeof(servAddr)) < 0)
    {
      switch (errno)
      {
      case EINPROGRESS:
      case EALREADY:
      case EINTR:
        continue;

      case EISCONN: /* We were spinning waiting on connect */
        {
          assert(0); // Programmer error
          break;
        }

      default:
        WATCHPOINT_ERRNO(errno);
        memcached_set_errno(*server, errno, MEMCACHED_AT);
        return MEMCACHED_CONNECTION_FAILURE;
      }
    }
  } while (0);
  server->state= MEMCACHED_SERVER_STATE_CONNECTED;

  WATCHPOINT_ASSERT(server->fd != INVALID_SOCKET);

  return MEMCACHED_SUCCESS;
#else
  (void)server;
  return MEMCACHED_NOT_SUPPORTED;
#endif
}

static memcached_return_t network_connect(memcached_server_st *server)
{
  bool timeout_error_occured= false;

  WATCHPOINT_ASSERT(server->fd == INVALID_SOCKET);
  WATCHPOINT_ASSERT(server->cursor_active == 0);

  /*
    We want to check both of these because if address_info_next has been fully tried, we want to do a new lookup to make sure we have picked up on any new DNS information.
  */
  if (server->address_info == NULL or server->address_info_next == NULL)
  {
    WATCHPOINT_ASSERT(server->state == MEMCACHED_SERVER_STATE_NEW);
    server->address_info_next= NULL;
    memcached_return_t rc;
    uint32_t counter= 5;
    while (--counter)
    {
      if ((rc= set_hostinfo(server)) != MEMCACHED_TIMEOUT)
      {
        break;
      }

#ifndef WIN32
      struct timespec dream, rem;

      dream.tv_nsec= 1000;
      dream.tv_sec= 0;

      nanosleep(&dream, &rem);
#endif
    }

    if (memcached_failed(rc))
    {
      return rc;
    }
  }

  if (server->address_info_next == NULL)
  {
    server->address_info_next= server->address_info;
    server->state= MEMCACHED_SERVER_STATE_ADDRINFO;
  }

  /* Create the socket */
  while (server->address_info_next and server->fd == INVALID_SOCKET)
  {
    /* Memcache server does not support IPV6 in udp mode, so skip if not ipv4 */
    if (memcached_is_udp(server->root) and server->address_info_next->ai_family != AF_INET)
    {
      server->address_info_next= server->address_info_next->ai_next;
      continue;
    }

    if ((server->fd= socket(server->address_info_next->ai_family,
                            server->address_info_next->ai_socktype,
                            server->address_info_next->ai_protocol)) < 0)
    {
      return memcached_set_errno(*server, get_socket_errno(), NULL);
    }

    set_socket_options(server);

    /* connect to server */
    if ((connect(server->fd, server->address_info_next->ai_addr, server->address_info_next->ai_addrlen) != SOCKET_ERROR))
    {
      server->state= MEMCACHED_SERVER_STATE_CONNECTED;
      return MEMCACHED_SUCCESS;
    }

    /* An error occurred */
    switch (get_socket_errno())
    {
    case ETIMEDOUT:
      timeout_error_occured= true;
      break;

    case EAGAIN:
#if EWOULDBLOCK != EAGAIN
    case EWOULDBLOCK:
#endif
    case EINPROGRESS: // nonblocking mode - first return
    case EALREADY: // nonblocking mode - subsequent returns
      {
        server->state= MEMCACHED_SERVER_STATE_IN_PROGRESS;
        memcached_return_t rc= connect_poll(server);

        if (memcached_success(rc))
        {
          server->state= MEMCACHED_SERVER_STATE_CONNECTED;
          return MEMCACHED_SUCCESS;
        }

        // A timeout here is treated as an error, we will not retry
        if (rc == MEMCACHED_TIMEOUT)
        {
          timeout_error_occured= true;
        }
      }
      break;

    case EISCONN: // we are connected :-)
      WATCHPOINT_ASSERT(0); // This is a programmer's error
      break;

    case EINTR: // Special case, we retry ai_addr
      WATCHPOINT_ASSERT(server->fd != INVALID_SOCKET);
      (void)closesocket(server->fd);
      server->fd= INVALID_SOCKET;
      continue;

    default:
      break;
    }

    WATCHPOINT_ASSERT(server->fd != INVALID_SOCKET);
    (void)closesocket(server->fd);
    server->fd= INVALID_SOCKET;
    server->address_info_next= server->address_info_next->ai_next;
  }

  WATCHPOINT_ASSERT(server->fd == INVALID_SOCKET);

  if (timeout_error_occured)
  {
    if (server->fd != INVALID_SOCKET)
    {
      (void)closesocket(server->fd);
      server->fd= INVALID_SOCKET;
    }
  }

  WATCHPOINT_STRING("Never got a good file descriptor");

  if (memcached_has_current_error(*server))
  {
    return memcached_server_error_return(server);
  }

  if (timeout_error_occured and server->state < MEMCACHED_SERVER_STATE_IN_PROGRESS)
  {
    return memcached_set_error(*server, MEMCACHED_TIMEOUT, MEMCACHED_AT);
  }

  return memcached_set_error(*server, MEMCACHED_CONNECTION_FAILURE, MEMCACHED_AT); /* The last error should be from connect() */
}


/*
  backoff_handling()

  Based on time/failure count fail the connect without trying. This prevents waiting in a state where
  we get caught spending cycles just waiting.
*/
static memcached_return_t backoff_handling(memcached_server_write_instance_st server, bool& in_timeout)
{
  struct timeval curr_time;
  bool _gettime_success= (gettimeofday(&curr_time, NULL) == 0);

  /* 
    If we hit server_failure_limit then something is completely wrong about the server.

    1) If autoeject is enabled we do that.
    2) If not? We go into timeout again, there is much else to do :(
  */
  if (server->server_failure_counter >= server->root->server_failure_limit)
  {
    /*
      We just auto_eject if we hit this point 
    */
    if (_is_auto_eject_host(server->root))
    {
      set_last_disconnected_host(server);

      // Retry dead servers if requested
      if (_gettime_success and server->root->dead_timeout > 0)
      {
        server->next_retry= curr_time.tv_sec +server->root->dead_timeout;

        // We only retry dead servers once before assuming failure again
        server->server_failure_counter= server->root->server_failure_limit -1;
      }

      memcached_return_t rc;
      if (memcached_failed(rc= run_distribution((memcached_st *)server->root)))
      {
        return memcached_set_error(*server, rc, MEMCACHED_AT, memcached_literal_param("Backoff handling failed during run_distribution"));
      }

      return memcached_set_error(*server, MEMCACHED_SERVER_MARKED_DEAD, MEMCACHED_AT);
    }

    server->state= MEMCACHED_SERVER_STATE_IN_TIMEOUT;

    // Sanity check/setting
    if (server->next_retry == 0)
    {
      server->next_retry= 1;
    }
  }

  if (server->state == MEMCACHED_SERVER_STATE_IN_TIMEOUT)
  {
    /*
      If next_retry is less then our current time, then we reset and try everything again.
    */
    if (_gettime_success and server->next_retry < curr_time.tv_sec)
    {
      server->state= MEMCACHED_SERVER_STATE_NEW;
    }
    else
    {
      return memcached_set_error(*server, MEMCACHED_SERVER_TEMPORARILY_DISABLED, MEMCACHED_AT);
    }

    in_timeout= true;
  }

  return MEMCACHED_SUCCESS;
}

static memcached_return_t _memcached_connect(memcached_server_write_instance_st server, const bool set_last_disconnected)
{
  if (server->fd != INVALID_SOCKET)
  {
    return MEMCACHED_SUCCESS;
  }

  LIBMEMCACHED_MEMCACHED_CONNECT_START();

  bool in_timeout= false;
  memcached_return_t rc;
  if (memcached_failed(rc= backoff_handling(server, in_timeout)))
  {
    set_last_disconnected_host(server);
    return rc;
  }

  if (LIBMEMCACHED_WITH_SASL_SUPPORT and server->root->sasl.callbacks and memcached_is_udp(server->root))
  {
    return memcached_set_error(*server, MEMCACHED_INVALID_HOST_PROTOCOL, MEMCACHED_AT, memcached_literal_param("SASL is not supported for UDP connections"));
  }

  if (server->hostname[0] == '/')
  {
    server->type= MEMCACHED_CONNECTION_UNIX_SOCKET;
  }

  /* We need to clean up the multi startup piece */
  switch (server->type)
  {
  case MEMCACHED_CONNECTION_UDP:
  case MEMCACHED_CONNECTION_TCP:
    rc= network_connect(server);

    if (LIBMEMCACHED_WITH_SASL_SUPPORT)
    {
      if (server->fd != INVALID_SOCKET and server->root->sasl.callbacks)
      {
        rc= memcached_sasl_authenticate_connection(server);
        if (memcached_failed(rc) and server->fd != INVALID_SOCKET)
        {
          WATCHPOINT_ASSERT(server->fd != INVALID_SOCKET);
          (void)closesocket(server->fd);
          server->fd= INVALID_SOCKET;
        }
      }
    }
    break;

  case MEMCACHED_CONNECTION_UNIX_SOCKET:
    rc= unix_socket_connect(server);
    break;
  }

  if (memcached_success(rc))
  {
    memcached_mark_server_as_clean(server);
    return rc;
  }
  else if (set_last_disconnected)
  {
    set_last_disconnected_host(server);
    if (memcached_has_current_error(*server))
    {
      memcached_mark_server_for_timeout(server);
      assert(memcached_failed(memcached_server_error_return(server)));
    }
    else
    {
      memcached_set_error(*server, rc, MEMCACHED_AT);
      memcached_mark_server_for_timeout(server);
    }

    LIBMEMCACHED_MEMCACHED_CONNECT_END();

    if (in_timeout)
    {
      char buffer[1024];
      int snprintf_length= snprintf(buffer, sizeof(buffer), "%s:%d", server->hostname, int(server->port));
      return memcached_set_error(*server, MEMCACHED_SERVER_TEMPORARILY_DISABLED, MEMCACHED_AT, buffer, snprintf_length);
    }
  }

  return rc;
}

memcached_return_t memcached_connect_try(memcached_server_write_instance_st server)
{
  return _memcached_connect(server, false);
}

memcached_return_t memcached_connect(memcached_server_write_instance_st server)
{
  return _memcached_connect(server, true);
}

memcached_return_t memcached_connect_new_ipaddress(memcached_server_write_instance_st server)
{
  set_hostinfo(server);
  return _memcached_connect(server, true);
}
