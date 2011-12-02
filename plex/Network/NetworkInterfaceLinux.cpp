/*
 *  Copyright (C) 2011 Plex, Inc.   
 *
 *  Created on: Apr 25, 2011
 *      Author: Elan Feingold
 */

#ifdef __linux__
#include <sys/uio.h>
#include <netinet/in.h>

#include <asm/types.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <sys/socket.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <arpa/inet.h>
#include <string.h>

#include <boost/asio.hpp>
#include <boost/thread.hpp>

#include "log.h"
#include "NetworkInterface.h"

using namespace boost;

vector<NetworkInterface::callback_function> NetworkInterface::g_observers;
vector<NetworkInterface> NetworkInterface::g_interfaces;
boost::mutex NetworkInterface::g_mutex;

///////////////////////////////////////////////////////////////////////////////////////////////////
void NetworkInterface::GetAll(vector<NetworkInterface>& interfaces)
{
  struct ifaddrs *ifa;
  getifaddrs(&ifa);

  for (struct ifaddrs* pInterface = ifa; pInterface; pInterface = pInterface->ifa_next)
  {
    // Look for IPv4 interfaces which are UP and capable of multicast.
    if (pInterface->ifa_flags & IFF_UP &&
        //pInterface->ifa_flags & IFF_MULTICAST &&
        !(pInterface->ifa_flags & IFF_POINTOPOINT) &&
        pInterface->ifa_addr->sa_family == AF_INET)
    {
      // Get the address as a string.
      char str[INET_ADDRSTRLEN];
      struct sockaddr_in *ifa_addr = (struct sockaddr_in *)pInterface->ifa_addr;
      inet_ntop(AF_INET, &ifa_addr->sin_addr, str, INET_ADDRSTRLEN);

      // The index.
      int index = if_nametoindex(pInterface->ifa_name);

      interfaces.push_back(NetworkInterface(index, pInterface->ifa_name, str, pInterface->ifa_flags & IFF_LOOPBACK));
    }
  }
}

///////////////////////////////////////////////////////////////////////////////////////////////////
static void PrintNetLinkMsg(const struct nlmsghdr *pNLMsg)
{
  const char *kNLMsgTypes[] =
  { "", "NLMSG_NOOP", "NLMSG_ERROR", "NLMSG_DONE", "NLMSG_OVERRUN"};
  const char *kNLRtMsgTypes[] =
  { "RTM_NEWLINK", "RTM_DELLINK", "RTM_GETLINK", "RTM_NEWADDR", "RTM_DELADDR", "RTM_GETADDR"};

  dprintf("NetworkInterface: received Netlink message len=%d, type=%s, flags=0x%x",
      pNLMsg->nlmsg_len, pNLMsg->nlmsg_type < RTM_BASE ? kNLMsgTypes[pNLMsg->nlmsg_type] : kNLRtMsgTypes[pNLMsg->nlmsg_type - RTM_BASE], pNLMsg->nlmsg_flags);

  if (RTM_NEWLINK <= pNLMsg->nlmsg_type && pNLMsg->nlmsg_type <= RTM_GETLINK)
  {
    struct ifinfomsg* pIfInfo = (struct ifinfomsg*) NLMSG_DATA(pNLMsg);
    dprintf("NetworkInterface: Netlink information message family=%d, type=%d, index=%d, flags=0x%x, change=0x%x",
        pIfInfo->ifi_family, pIfInfo->ifi_type, pIfInfo->ifi_index, pIfInfo->ifi_flags, pIfInfo->ifi_change);
  }
  else if (RTM_NEWADDR <= pNLMsg->nlmsg_type && pNLMsg->nlmsg_type <= RTM_GETADDR)
  {
    struct ifaddrmsg* pIfAddr = (struct ifaddrmsg*) NLMSG_DATA(pNLMsg);
    dprintf("NetworkInterface: Netlink address message family=%d, index=%d, flags=0x%x\n", pIfAddr->ifa_family, pIfAddr->ifa_index, pIfAddr->ifa_flags);
  }
}

///////////////////////////////////////////////////////////////////////////////////////////////////
void NetworkChanged()
{
  dprintf("Network change.");
  NetworkInterface::NotifyOfNetworkChange();
}

///////////////////////////////////////////////////////////////////////////////////////////////////
void RunWatchingForChanges()
{
  dprintf("NetworkInterface: Watching for changes on the interfaces.");
  
  // Create the socket that's going to watch for interface changes, and make it non-blocking.
  int sock = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
  if (sock == -1)
    eprintf("Error creating NetLink socket: %d", errno);

  // Subscribe the socket to Link & IP addr notifications.
  struct sockaddr_nl snl;
  memset(&snl, 0, sizeof(snl));
  snl.nl_family = AF_NETLINK;
  snl.nl_groups = RTMGRP_LINK | RTMGRP_IPV4_IFADDR;

  int ret = ::bind(sock, (struct sockaddr *) &snl, sizeof snl);
  if (ret != 0)
    eprintf("NetworkInterface: Error seting up netlink socket (%d).", errno);

  // Now sit in a loop waiting for messages.
  while (ret == 0)
  {
    // Read through the messages on sd and if any indicate that any interface records should be rebuilt.
    char buff[4096];
    struct nlmsghdr* pNLMsg = (struct nlmsghdr* )buff;

    // The structure here is more complex than it really ought to be because,
    // unfortunately, there's no good way to size a buffer in advance large
    // enough to hold all pending data and so avoid message fragmentation.
    // (Note that FIONREAD is not supported on AF_NETLINK.)
    //
    ssize_t readCount = read(sock, buff, sizeof(buff));
    while (1)
    {
      // Make sure we've got an entire nlmsghdr in the buffer, and payload, too.
      // If not, discard already-processed messages in buffer and read more data.
      //
      if (((char* )&pNLMsg[1] > (buff + readCount)) || ((char* )pNLMsg + pNLMsg->nlmsg_len > (buff + readCount)))
      {
        // See if we have space to shuffle.
        if (buff < (char*) pNLMsg)
        {
          // discard processed data
          readCount -= ((char*) pNLMsg - buff);
          memmove(buff, pNLMsg, readCount);
          pNLMsg = (struct nlmsghdr*) buff;

          // read more data
          readCount += read(sock, buff + readCount, sizeof buff - readCount);

          // spin around and revalidate with new readCount.
          continue;
        }
        else
        {
          // Otherwise message does not fit in buffer.
          break;
        }
      }

      // Dump the message.
      PrintNetLinkMsg(pNLMsg);

      // See if something notable changed.
      if (pNLMsg->nlmsg_type == RTM_GETLINK || pNLMsg->nlmsg_type == RTM_NEWLINK ||
          pNLMsg->nlmsg_type == RTM_DELADDR || pNLMsg->nlmsg_type == RTM_NEWADDR)
      {
        // Notify about it.
        NetworkChanged();
      }

      // Advance pNLMsg to the next message in the buffer
      if ((pNLMsg->nlmsg_flags & NLM_F_MULTI) != 0 && pNLMsg->nlmsg_type != NLMSG_DONE)
      {
        ssize_t len = readCount - ((char*)pNLMsg - buff);
        pNLMsg = NLMSG_NEXT(pNLMsg, len);
      }
      else
      {
        // all done!
        break;
      }
    }
  }
}

///////////////////////////////////////////////////////////////////////////////////////////////////
void NetworkInterface::WatchForChanges()
{
  // Start the thread.
  dprintf("NetworkInterface: Starting watch thread.");
  thread t = thread(boost::bind(&RunWatchingForChanges));
  t.detach();
  
  // Start with a change, because otherwise we're in steady state.
  NetworkChanged();
}

#endif
