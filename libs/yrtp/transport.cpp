/**
 * transport.cpp
 * Yet Another RTP Stack
 * This file is part of the YATE Project http://YATE.null.ro
 *
 * Yet Another Telephony Engine - a fully featured software PBX and IVR
 * Copyright (C) 2004-2006 Null Team
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA.
 */

#include <yatertp.h>

#define BUF_SIZE 1500

using namespace TelEngine;

static unsigned long s_sleep = 5;


RTPGroup::RTPGroup(int msec, Priority prio)
    : Mutex(true,"RTPGroup"),
      Thread("RTP Group",prio), m_listChanged(false)
{
    DDebug(DebugInfo,"RTPGroup::RTPGroup() [%p]",this);
    if (msec < 1)
	msec = 1;
    if (msec > 50)
	msec = 50;
    m_sleep = msec;
}

RTPGroup::~RTPGroup()
{
    DDebug(DebugInfo,"RTPGroup::~RTPGroup() [%p]",this);
}

void RTPGroup::cleanup()
{
    DDebug(DebugInfo,"RTPGroup::cleanup() [%p]",this);
    lock();
    m_listChanged = true;
    ObjList* l = &m_processors;
    while (l) {
	RTPProcessor* p = static_cast<RTPProcessor*>(l->get());
	if (p) {
	    p->group(0);
	    if (p != static_cast<RTPProcessor*>(l->get()))
		continue;
	}
	l = l->next();
    }
    m_processors.clear();
    unlock();
}

void RTPGroup::run()
{
    DDebug(DebugInfo,"RTPGroup::run() [%p]",this);
    bool ok = true;
    while (ok) {
	unsigned long msec = m_sleep;
	if (msec < s_sleep)
	    msec = s_sleep;
	lock();
	Time t;
	ObjList* l = &m_processors;
	m_listChanged = false;
	for (ok = false;l;l = l->next()) {
	    RTPProcessor* p = static_cast<RTPProcessor*>(l->get());
	    if (p) {
		ok = true;
		p->timerTick(t);
		// the list is protected from other threads but can be changed
		//  from this one so if it happened we just break out and try
		//  again later rather than using an expensive ListIterator
		if (m_listChanged)
		    break;
	    }
	}
	unlock();
	Thread::msleep(msec,true);
    }
    DDebug(DebugInfo,"RTPGroup::run() ran out of processors [%p]",this);
}

void RTPGroup::join(RTPProcessor* proc)
{
    DDebug(DebugAll,"RTPGroup::join(%p) [%p]",proc,this);
    lock();
    m_listChanged = true;
    m_processors.append(proc)->setDelete(false);
    startup();
    unlock();
}

void RTPGroup::part(RTPProcessor* proc)
{
    DDebug(DebugAll,"RTPGroup::part(%p) [%p]",proc,this);
    lock();
    m_listChanged = true;
    m_processors.remove(proc,false);
    unlock();
}

void RTPGroup::setMinSleep(int msec)
{
    if (msec < 1)
	msec = 1;
    if (msec > 20)
	msec = 20;
    s_sleep = msec;
}


RTPProcessor::RTPProcessor()
    : m_wrongSrc(0), m_group(0) 
{
    DDebug(DebugAll,"RTPProcessor::RTPProcessor() [%p]",this);
}

RTPProcessor::~RTPProcessor()
{
    DDebug(DebugAll,"RTPProcessor::~RTPProcessor() [%p]",this);
    group(0);
}

void RTPProcessor::group(RTPGroup* newgrp)
{
    DDebug(DebugAll,"RTPProcessor::group(%p) old=%p [%p]",newgrp,m_group,this);
    if (newgrp == m_group)
	return;
    if (m_group)
	m_group->part(this);
    m_group = newgrp;
    if (m_group)
	m_group->join(this);
}

void RTPProcessor::rtpData(const void* data, int len)
{
}

void RTPProcessor::rtcpData(const void* data, int len)
{
}

void RTPProcessor::getStats(String& stats) const
{
}


RTPTransport::RTPTransport(RTPTransport::Type type)
    : RTPProcessor(),
      m_type(type), m_processor(0), m_monitor(0), m_autoRemote(false)
{
    DDebug(DebugAll,"RTPTransport::RTPTransport(%d) [%p]",type,this);
}

RTPTransport::~RTPTransport()
{
    DDebug(DebugAll,"RTPTransport::~RTPTransport() [%p]",this);
    setProcessor();
    group(0);
}

void RTPTransport::timerTick(const Time& when)
{
    XDebug(DebugAll,"RTPTransport::timerTick() group=%p [%p]",group(),this);
    if (m_rtpSock.valid()) {
	char buf[BUF_SIZE];
	int len;
	while ((len = m_rtpSock.recvFrom(buf,sizeof(buf),m_rxAddrRTP)) > 0) {
	    switch (m_type) {
		case RTP:
		    if (len < 12)
			continue;
		    if (((unsigned char)buf[0] & 0xc0) != 0x80)
			continue;
		    break;
		case UDPTL:
		    if (len < 6)
			continue;
		    break;
		default:
		    break;
	    }
	    if (!m_remoteAddr.valid())
		continue;
	    // looks like it's RTP or UDPTL, at least by length and version
	    bool preferred = false;
	    if ((m_autoRemote || (preferred = (m_rxAddrRTP == m_remotePref))) && (m_rxAddrRTP != m_remoteAddr)) {
		Debug(DebugInfo,"Auto changing RTP address from %s:%d to%s %s:%d",
		    m_remoteAddr.host().c_str(),m_remoteAddr.port(),
		    (preferred ? " preferred" : ""),
		    m_rxAddrRTP.host().c_str(),m_rxAddrRTP.port());
		// if we received from the preferred address don't auto change any more
		if (preferred)
		    m_remotePref.clear();
		remoteAddr(m_rxAddrRTP);
	    }
	    m_autoRemote = false;
	    if (m_rxAddrRTP == m_remoteAddr) {
		if (m_processor)
		    m_processor->rtpData(buf,len);
		if (m_monitor)
		    m_monitor->rtpData(buf,len);
	    }
	    else
		m_processor->incWrongSrc();
	}
	m_rtpSock.timerTick(when);
    }
    if (m_rtcpSock.valid()) {
	char buf[BUF_SIZE];
	int len;
	while (((len = m_rtcpSock.recvFrom(buf,sizeof(buf),m_rxAddrRTCP)) >= 8) && (m_rxAddrRTCP == m_remoteRTCP)) {
	    if (m_processor)
		m_processor->rtcpData(buf,len);
	    if (m_monitor)
		m_monitor->rtcpData(buf,len);
	}
	m_rtcpSock.timerTick(when);
    }
}

void RTPTransport::rtpData(const void* data, int len)
{
    if (!data)
	return;
    switch (m_type) {
	case RTP:
	    if (len < 12)
		return;
	    break;
	case UDPTL:
	    if (len < 6)
		return;
	    break;
	default:
	    break;
    }
    if (m_rtpSock.valid() && m_remoteAddr.valid())
	m_rtpSock.sendTo(data,len,m_remoteAddr);
}

void RTPTransport::rtcpData(const void* data, int len)
{
    if ((len < 8) || !data)
	return;
    if (m_rtcpSock.valid() && m_remoteRTCP.valid())
	m_rtcpSock.sendTo(data,len,m_remoteRTCP);
}

void RTPTransport::setProcessor(RTPProcessor* processor)
{
    if (processor) {
	// both should run in the same RTP group
	if (group())
	    processor->group(group());
	else
	    group(processor->group());
    }
    m_processor = processor;
}

void RTPTransport::setMonitor(RTPProcessor* monitor)
{
    m_monitor = monitor;
}

bool RTPTransport::localAddr(SocketAddr& addr, bool rtcp)
{
    // check if sockets are already created and bound
    if (m_rtpSock.valid())
	return false;
    int p = addr.port();
    // for RTCP make sure we don't have a port or it's an even one
    if (rtcp && (p & 1))
	return false;
    if (m_rtpSock.create(addr.family(),SOCK_DGRAM) && m_rtpSock.bind(addr)) {
	m_rtpSock.setBlocking(false);
	if (!rtcp) {
	    // RTCP not requested - we are done
	    m_rtpSock.getSockName(addr);
	    m_localAddr = addr;
	    return true;
	}
	if (!p) {
	    m_rtpSock.getSockName(addr);
	    p = addr.port();
	    if (p & 1) {
		// allocated odd port - have to swap sockets
		m_rtcpSock.attach(m_rtpSock.detach());
		addr.port(p-1);
		if (m_rtpSock.create(addr.family(),SOCK_DGRAM) && m_rtpSock.bind(addr)) {
		    m_rtpSock.setBlocking(false);
		    m_localAddr = addr;
		    return true;
		}
		DDebug(DebugMild,"RTP Socket failed with code %d",m_rtpSock.error());
		m_rtpSock.terminate();
		m_rtcpSock.terminate();
		return false;
	    }
	}
	addr.port(p+1);
	if (m_rtcpSock.create(addr.family(),SOCK_DGRAM) && m_rtcpSock.bind(addr)) {
	    m_rtcpSock.setBlocking(false);
	    addr.port(p);
	    m_localAddr = addr;
	    return true;
	}
#ifdef DEBUG
	else
	    Debug(DebugMild,"RTCP Socket failed with code %d",m_rtcpSock.error());
#endif
    }
#ifdef DEBUG
    else
	Debug(DebugMild,"RTP Socket failed with code %d",m_rtpSock.error());
#endif
    m_rtpSock.terminate();
    m_rtcpSock.terminate();
    return false;
}

bool RTPTransport::remoteAddr(SocketAddr& addr, bool sniff)
{
    Lock lock(group());
    m_autoRemote = sniff;
    int p = addr.port();
    // make sure we have a valid address and a port
    // we do not check that it's even numbered as many NAPTs will break that
    if (p && addr.valid()) {
	m_remoteAddr = addr;
	m_remoteRTCP = addr;
	m_remoteRTCP.port(addr.port()+1);
	// if sniffing packets from other sources remember preferred address
	if (sniff)
	    m_remotePref = addr;
	return true;
    }
    return false;
}

bool RTPTransport::drillHole()
{
    if (m_rtpSock.valid() && m_remoteAddr.valid()) {
	static const char buf[4] = { 0, 0, 0, 0 };
	if (m_rtpSock.sendTo(buf,sizeof(buf),m_remoteAddr) == sizeof(buf)) {
	    if (m_rtcpSock.valid() && m_remoteRTCP.valid())
		m_rtcpSock.sendTo(buf,sizeof(buf),m_remoteRTCP);
	    return true;
	}
    }
    return false;
}

/* vi: set ts=8 sw=4 sts=4 noet: */
