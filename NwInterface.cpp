/*
 *
 * (C) 2021-22 - ntop.org
 *
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 */

#include "include.h"

/* Forward */
int netfilter_callback(struct nfq_q_handle *qh, struct nfgenmsg *nfmsg,
		       struct nfq_data *nfa, void *data);

/* **************************************************** */

NwInterface::NwInterface(u_int nf_device_id,
				       Configuration *_c,
				       GeoIP *_g) {
  conf = _c, geoip = _g;

  queueId = nf_device_id, nfHandle = nfq_open();

  if(nfHandle == NULL) {
    trace->traceEvent(TRACE_ERROR, "Unable to get netfilter handle [queueId=%d]", queueId);
    throw 1;
  }

  if(nfq_unbind_pf(nfHandle, AF_INET) < 0) {
    trace->traceEvent(TRACE_ERROR, "Unable to unbind [queueId=%d]: are you root ?", queueId);
    throw 1;
  }

  if(nfq_bind_pf(nfHandle, AF_INET) < 0) {
    trace->traceEvent(TRACE_ERROR, "Unable to bind [queueId=%d]", queueId);
    throw 1;
  }

  if((queueHandle = nfq_create_queue(nfHandle, queueId, &netfilter_callback, this)) == NULL) {
    trace->traceEvent(TRACE_ERROR, "Unable to attach to NF_QUEUE %d: is it already in use?", queueId);
    throw 1;
  } else
    trace->traceEvent(TRACE_NORMAL, "Succesfully connected to NF_QUEUE %d", queueId);

#if !defined(__mips__)
  nfnl_rcvbufsiz(nfq_nfnlh(nfHandle), NF_BUFFER_SIZE);
#endif

  if(nfq_set_mode(queueHandle, NFQNL_COPY_PACKET, 0XFFFF) < 0) {
    trace->traceEvent(TRACE_ERROR, "Unable to set packet_copy mode");
    throw 1;
  }

  if(nfq_set_queue_maxlen(queueHandle, NF_MAX_QUEUE_LEN) < 0) {
    trace->traceEvent(TRACE_ERROR, "Unable to set queue len");
    throw 1;
  }

  nf_fd = nfq_fd(nfHandle);
}

/* **************************************************** */

NwInterface::~NwInterface() {
  if(queueHandle) nfq_destroy_queue(queueHandle);
  if(nfHandle)    nfq_close(nfHandle);

  nf_fd = 0;
}

/* **************************************************** */

int netfilter_callback(struct nfq_q_handle *qh,
		       struct nfgenmsg *nfmsg,
		       struct nfq_data *nfa,
		       void *data) {
  const u_char *payload;
  struct nfqnl_msg_packet_hdr *ph = nfq_get_msg_packet_hdr(nfa);
  NwInterface *iface = (NwInterface *)data;
  u_int payload_len;
  u_int32_t id = ntohl(ph->packet_id);
  u_int16_t marker;

  if(!ph) return(-1);

#ifdef HAVE_NFQ_SET_VERDICT2
  payload_len = nfq_get_payload(nfa, (unsigned char **)&payload);
#else
  payload_len = nfq_get_payload(nfa, (char **)&payload);
#endif

  marker = iface->dissectPacket(payload, payload_len);

  return(nfq_set_verdict2(qh, id, NF_ACCEPT, marker, 0, NULL));
}

/* **************************************************** */

void NwInterface::packetPollLoop() {
  struct nfq_handle *h;
  int fd;

  ifaceRunning = true;

  h = get_nfHandle();
  fd = get_fd();

  while(isRunning()) {
    fd_set mask;
    struct timeval wait_time;

    FD_ZERO(&mask);
    FD_SET(fd, &mask);
    wait_time.tv_sec = 1, wait_time.tv_usec = 0;

    if(select(fd+1, &mask, 0, 0, &wait_time) > 0) {
      char pktBuf[8192] __attribute__ ((aligned));
      int len = recv(fd, pktBuf, sizeof(pktBuf), 0);

      // trace->traceEvent(TRACE_INFO, "Pkt len %d", len);

      if(len >= 0) {
	int rc = nfq_handle_packet(h, pktBuf, len);

	if(rc < 0)
	  trace->traceEvent(TRACE_ERROR, "nfq_handle_packet() failed: [len: %d][rc: %d][errno: %d]", len, rc, errno);
      } else {
	trace->traceEvent(TRACE_ERROR, "NF_QUEUE receive error: [len: %d][errno: %d]", len, errno);
	break;
      }
    }
  }

  trace->traceEvent(TRACE_NORMAL, "Leaving netfilter packet poll loop");

  ifaceRunning = false;
}

/* **************************************************** */

Marker NwInterface::dissectPacket(const u_char *payload, u_int payload_len) {
  /* We can see only IP addresses */
  u_int16_t ip_offset = 0, vlan_id = 0 /* FIX */;

  if (payload_len >= ip_offset) {
    struct iphdr *iph = (struct iphdr *)&payload[ip_offset];
    bool ipv4 = false, ipv6 = false;

    struct tcphdr *tcph = NULL;
    struct udphdr *udph = NULL;
    u_int16_t src_port, dst_port;
    u_int8_t proto, ip_payload_offset = 40 /* ipv6 is 40B long */;
    char src[INET6_ADDRSTRLEN] = {}, dst[INET6_ADDRSTRLEN] = {};

    if (iph->version == 6) {
      ipv6 = true;
      struct ip6_hdr *ip6h = (struct ip6_hdr *)&payload[ip_offset];
      proto = ip6h->ip6_nxt;

      // ipv6 address stringification
      inet_ntop(AF_INET6, &(ip6h->ip6_src), src, sizeof(src));
      inet_ntop(AF_INET6, &(ip6h->ip6_dst), dst, sizeof(dst));

    } else if (iph->version == 4) {
      ipv4 = true;
      u_int8_t frag_off = ntohs(iph->frag_off);
      struct in_addr a;

      if ((iph->protocol == IPPROTO_UDP) && ((frag_off & 0x3FFF /* IP_MF | IP_OFFSET */) != 0))
        return (MARKER_UNKNOWN); /* Don't block it */

      // get protocol and offset
      proto = iph->protocol;
      ip_payload_offset = iph->ihl * 4;
      // ipv4 address stringification
      a.s_addr = iph->saddr, inet_ntop(AF_INET, &a, src, sizeof(src));
      a.s_addr = iph->daddr, inet_ntop(AF_INET, &a, dst, sizeof(dst));
    } else { // Neither ipv4 or ipv6...unlikely to be evaluated
      return (MARKER_PASS);
    }

    u_int8_t *nxt = ((u_int8_t *)iph + ip_payload_offset);

    switch (proto) {
      case IPPROTO_TCP:
        tcph = (struct tcphdr *)(nxt);
        src_port = tcph->source, dst_port = tcph->dest;
        break;
      case IPPROTO_UDP:
        udph = (struct udphdr *)(nxt);
        src_port = udph->source, dst_port = udph->dest;
        break;
      default:
        // we do not care about ports in other protocols
        src_port = dst_port = 0;
    }
    return (makeVerdict(proto, vlan_id,
                        src_port, dst_port,
                        src,dst, ipv4,ipv6));
  }
  return (MARKER_PASS);
}

/* **************************************************** */

const char* NwInterface::getProtoName(u_int8_t proto) {
  switch(proto) {
  case IPPROTO_TCP:  return("TCP");
  case IPPROTO_UDP:  return("UDP");
  case IPPROTO_ICMP: return("ICMP");
  default:           return("???");
  }
}

/* **************************************************** */

bool NwInterface::isPrivateIPv4(u_int32_t addr /* network byte order */) {

  if(((addr & 0xFF000000) == 0x0A000000 /* 10.0.0.0/8 */)
     || ((addr & 0xFFF00000) == 0xAC100000 /* 172.16.0.0/12 */)
     || ((addr & 0xFFFF0000) == 0xC0A80000 /* 192.168.0.0/16 */)
     || ((addr & 0xFF000000) == 0x7F000000 /* 127.0.0.0/8 */)
     || ((addr & 0xFFFF0000) == 0xA9FE0000 /* 169.254.0.0/16 Link-Local communication rfc3927 */)
     || (addr == 0xFFFFFFFF /* 255.255.255.255 */)
     || (addr == 0x0        /* 0.0.0.0 */)
     || ((addr & 0xF0000000) == 0xE0000000 /* 224.0.0.0/4 */))
    return(true);
  else
    return(false);
}

bool NwInterface::isPrivateIPv6(const char *ip6addr) {
  struct in6_addr a;
  inet_pton(AF_INET6,ip6addr,&a);

  // We use only the 32bit structure
  for(size_t l=0; l < 4; l++){ // change byte ordering
      a.s6_addr32[l] = ntohl(a.s6_addr32[l]);
  }
  
  bool is_link_local = (a.s6_addr32[0] & (0xffc00000)) == (0xfe800000); // check the first 10 bits
  bool is_unique_local = (a.s6_addr32[0] & (0xfe000000)) == (0xfc000000); // check the first 7 bits 
  
  return is_unique_local || is_link_local;
}


/* **************************************************** */

void NwInterface::logFlow(const char *proto_name,
			  char *src_host, u_int16_t sport, char *src_country, char *src_continent, bool src_blacklisted,
			  char *dst_host, u_int16_t dport, char *dst_country, char *dst_continent, bool dst_blacklisted,
			  bool pass_verdict) {
  Json::Value root;
  std::string json_txt;
  Json::FastWriter writer;
  
  root["proto"] = proto_name;

  root["src"]["host"] = src_host;
  root["src"]["port"] = sport;
  if(src_country && (src_country[0] != '\0')) root["src"]["country"] = src_country;
  if(src_continent && (src_continent[0] != '\0')) root["src"]["continent"] = src_continent;
  if(src_blacklisted) root["src"]["blacklisted"] = src_blacklisted;

  root["dst"]["host"] = dst_host;
  root["dst"]["port"] = dport;
  if(dst_country && (dst_country[0] != '\0')) root["dst"]["country"] = dst_country;
  if(dst_continent && (dst_continent[0] != '\0')) root["dst"]["continent"] = dst_continent;
  if(dst_blacklisted) root["dst"]["blacklisted"] = dst_blacklisted;
  
  root["verdict"] = pass_verdict ? "pass" : "drop";

  json_txt = writer.write(root);

  if(pass_verdict)
    trace->traceEvent(TRACE_INFO, "%s", json_txt.c_str());
  else
    trace->traceEvent(TRACE_WARNING, "%s", json_txt.c_str());
}

/* **************************************************** */

Marker NwInterface::makeVerdict(u_int8_t proto, u_int16_t vlanId,
				u_int16_t sport /* network byte order */,
				u_int16_t dport /* network byte order */,
        char *src_host, char *dst_host,
        bool ipv4, bool ipv6) {
  // Step 0 - Check ip protocol
  if (!(ipv4 || ipv6)) return (conf->getDefaultPolicy());

  struct in_addr in;
  char src_ctry[3]={}, dst_ctry[3]={}, src_cont[3]={}, dst_cont[3]={} ;
  const char *proto_name = getProtoName(proto);

  // trace->traceEvent(TRACE_DEBUG, "%s %s %s : %u -> %s : %u",ipv4 ? "IPv4" : (ipv6 ? "IPv6" : "???"),
  //   proto_name, src_host, sport, dst_host, dport);

  u_int32_t saddr = ipv4 ? inet_addr(src_host) : 0;
  u_int32_t daddr = ipv4 ? inet_addr(dst_host) : 0;
  bool pass_local = true,
    saddr_private = (ipv4 ? isPrivateIPv4(saddr) : isPrivateIPv6(src_host)),
    daddr_private = (ipv4 ? isPrivateIPv4(saddr) : isPrivateIPv6(dst_host));
  Marker m, src_marker, dst_marker;
  sport = ntohs(sport), dport = ntohs(dport);

  /* Check if sender/recipient are blacklisted */
  if (ipv4){
  in.s_addr = saddr;
  /* Step 1 - For all ports/protocols, check if sender/recipient are blacklisted and if so, block this flow */
  if((!saddr_private) && conf->isBlacklistedIPv4(&in)) {
    logFlow(proto_name,
	    src_host, sport, src_ctry, src_cont, true,
	    dst_host, dport, dst_ctry, dst_cont, false,
	    false /* drop */);

    return(MARKER_DROP);
  }

  in.s_addr = daddr;
  if((!daddr_private) && conf->isBlacklistedIPv4(&in)) {
    logFlow(proto_name,
	    src_host, sport, src_ctry, src_cont, false,
	    dst_host, dport, dst_ctry, dst_cont, true,
	    false /* drop */);

    return(MARKER_DROP);
  }
  }


  /* Step 2 - For TCP/UDP ignore traffic for non-monitored ports */
  switch(proto) {
  case IPPROTO_TCP:
    if((conf->isMonitoredTCPPort(sport)) || conf->isMonitoredTCPPort(dport))
      ;
    else {
      trace->traceEvent(TRACE_INFO, "Ignoring TCP ports %u/%u", sport, dport);
      return(MARKER_PASS);
    }
    break;

  case IPPROTO_UDP:
    if((conf->isMonitoredUDPPort(sport)) || conf->isMonitoredUDPPort(dport))
      ;
    else {
      trace->traceEvent(TRACE_INFO, "Ignoring UDP ports %u/%u", sport, dport);
      return(MARKER_PASS);
    }
    break;
  }

  m = src_marker = dst_marker = conf->getDefaultPolicy();

  /* Step 3 - For monitored TCP/UDP ports (and ICMP) check the country blacklist */
  if((!saddr_private) && (geoip->lookup(src_host, src_ctry, sizeof(src_ctry), src_cont, sizeof(src_cont)))) {
    src_marker = conf->getMarker(src_ctry,src_cont);
    pass_local = false;
  } else {
    /* Unknown or private IP address  */
    src_marker = MARKER_PASS;
  }

  if((!daddr_private) && (geoip->lookup(dst_host, dst_ctry, sizeof(dst_ctry), dst_cont, sizeof(dst_cont)))) {
    dst_marker = conf->getMarker(dst_ctry, dst_cont);
    pass_local = false;
  } else {
    /* Unknown or private IP address  */
    dst_marker = MARKER_PASS;
  }

  /* Final step: compute the flow verdict */

  if((conf->isIgnoredPort(sport) || conf->isIgnoredPort(dport))
     || ((src_marker == MARKER_PASS) && (dst_marker == MARKER_PASS))) {
    m = MARKER_PASS;

    logFlow(proto_name,
	    src_host, sport, src_ctry, src_cont, false,
	    dst_host, dport, dst_ctry, dst_cont, false,
	    true /* pass */);
  } else {
    m = MARKER_DROP;

    logFlow(proto_name,
	    src_host, sport, src_ctry, src_cont, false,
	    dst_host, dport, dst_ctry, dst_cont, false,
	    false /* drop */);
  }

  return(m);
}
