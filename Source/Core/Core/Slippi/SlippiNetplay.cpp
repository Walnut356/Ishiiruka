// Copyright 2010 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include "Core/Slippi/SlippiNetplay.h"
#include "Common/CommonTypes.h"
#include "Common/ENetUtil.h"
#include "Common/MsgHandler.h"
#include "Common/Timer.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "SlippiPremadeText.h"
#include "VideoCommon/OnScreenDisplay.h"
#include "VideoCommon/VideoConfig.h"
#include <algorithm>
#include <fstream>
#include <memory>
#include <thread>

//#include "Common/MD5.h"
//#include "Common/Common.h"
//#include "Common/CommonPaths.h"
//#include "Core/HW/EXI_DeviceIPL.h"
//#include "Core/HW/SI.h"
//#include "Core/HW/SI_DeviceGCController.h"
//#include "Core/HW/Sram.h"
//#include "Core/HW/WiimoteEmu/WiimoteEmu.h"
//#include "Core/HW/WiimoteReal/WiimoteReal.h"
//#include "Core/IPC_HLE/WII_IPC_HLE_Device_usb_bt_emu.h"
//#include "Core/Movie.h"
//#include "InputCommon/GCAdapter.h"
//#include <mbedtls/md5.h>
//#include <SlippiGame.h>

static std::mutex pad_mutex;
static std::mutex ack_mutex;

SlippiNetplayClient *SLIPPI_NETPLAY = nullptr;

// called from ---GUI--- thread
SlippiNetplayClient::~SlippiNetplayClient()
{
	m_do_loop.Clear();
	if (m_thread.joinable())
		m_thread.join();

	if (!m_server.empty())
	{
		Disconnect();
	}

	if (g_MainNetHost.get() == m_client)
	{
		g_MainNetHost.release();
	}
	if (m_client)
	{
		enet_host_destroy(m_client);
		m_client = nullptr;
	}

	SLIPPI_NETPLAY = nullptr;

	WARN_LOG(SLIPPI_ONLINE, "Netplay client cleanup complete");
}

// called from ---SLIPPI EXI--- thread
SlippiNetplayClient::SlippiNetplayClient(std::vector<std::string> addrs, std::vector<u16> ports,
                                         const u8 remotePlayerCount, const u16 localPort, bool isDecider, u8 playerIdx)
#ifdef _WIN32
    : m_qos_handle(nullptr)
    , m_qos_flow_id(0)
#endif
{
	WARN_LOG(SLIPPI_ONLINE, "Initializing Slippi Netplay for port: %d, with host: %s, player idx: %d", localPort,
	         isDecider ? "true" : "false", playerIdx);
	this->isDecider = isDecider;
	this->m_remotePlayerCount = remotePlayerCount;
	this->playerIdx = playerIdx;

	// Set up remote player data structures
	int j = 0;
	for (int i = 0; i < SLIPPI_REMOTE_PLAYER_MAX; i++, j++)
	{
		if (j == playerIdx)
			j++;
		this->matchInfo.remotePlayerSelections[i] = SlippiPlayerSelections();
		this->matchInfo.remotePlayerSelections[i].playerIdx = j;

		this->remotePadQueue[i] = std::deque<std::unique_ptr<SlippiPad>>();
		this->frameOffsetData[i] = FrameOffsetData();
		this->lastFrameTiming[i] = FrameTiming();
		this->pingUs[i] = 0;
		this->lastFrameAcked[i] = 0;
	}

	SLIPPI_NETPLAY = std::move(this);

	// Local address
	ENetAddress *localAddr = nullptr;
	ENetAddress localAddrDef;

	// It is important to be able to set the local port to listen on even in a client connection because
	// not doing so will break hole punching, the host is expecting traffic to come from a specific ip/port
	// and if the port does not match what it is expecting, it will not get through the NAT on some routers
	if (localPort > 0)
	{
		INFO_LOG(SLIPPI_ONLINE, "Setting up local address");

		localAddrDef.host = ENET_HOST_ANY;
		localAddrDef.port = localPort;

		localAddr = &localAddrDef;
	}

	// TODO: Figure out how to use a local port when not hosting without accepting incoming connections
	m_client = enet_host_create(localAddr, 10, 3, 0, 0);

	if (m_client == nullptr)
	{
		PanicAlertT("Couldn't Create Client");
	}

	for (int i = 0; i < remotePlayerCount; i++)
	{
		ENetAddress addr;
		enet_address_set_host(&addr, addrs[i].c_str());
		addr.port = ports[i];
		// INFO_LOG(SLIPPI_ONLINE, "Set ENet host, addr = %x, port = %d", addr.host, addr.port);

		ENetPeer *peer = enet_host_connect(m_client, &addr, 3, 0);
		m_server.push_back(peer);

		// Store this connection
		std::stringstream keyStrm;
		keyStrm << addr.host << "-" << addr.port;
		activeConnections[keyStrm.str()][peer] = true;
		INFO_LOG(SLIPPI_ONLINE, "New connection (constr): %s, %X", keyStrm.str().c_str(), peer);

		if (peer == nullptr)
		{
			PanicAlertT("Couldn't create peer.");
		}
		else
		{
			// INFO_LOG(SLIPPI_ONLINE, "Connecting to ENet host, addr = %x, port = %d", peer->address.host,
			//         peer->address.port);
		}
	}

	slippiConnectStatus = SlippiConnectStatus::NET_CONNECT_STATUS_INITIATED;

	m_thread = std::thread(&SlippiNetplayClient::ThreadFunc, this);
}

// Make a dummy client
SlippiNetplayClient::SlippiNetplayClient(bool isDecider)
{
	this->isDecider = isDecider;
	SLIPPI_NETPLAY = std::move(this);
	slippiConnectStatus = SlippiConnectStatus::NET_CONNECT_STATUS_FAILED;
}

u8 SlippiNetplayClient::PlayerIdxFromPort(u8 port)
{
	u8 p = port;
	if (port > playerIdx)
	{
		p--;
	}
	return p;
}

u8 SlippiNetplayClient::LocalPlayerPort()
{
	return this->playerIdx;
}

// called from ---NETPLAY--- thread
unsigned int SlippiNetplayClient::OnData(sf::Packet &packet, ENetPeer *peer)
{
	MessageId mid = 0;
	if (!(packet >> mid))
	{
		ERROR_LOG(SLIPPI_ONLINE, "Received empty netplay packet");
		return 0;
	}

	switch (mid)
	{
	case NP_MSG_SLIPPI_PAD:
	{
		// Fetch current time immediately for the most accurate timing calculations
		u64 curTime = Common::Timer::GetTimeUs();

		s32 frame;
		s32 checksumFrame;
		u32 checksum;
		if (!(packet >> frame))
		{
			ERROR_LOG(SLIPPI_ONLINE, "Netplay packet too small to read frame count");
			break;
		}
		u8 packetPlayerPort;
		if (!(packet >> packetPlayerPort))
		{
			ERROR_LOG(SLIPPI_ONLINE, "Netplay packet too small to read player index");
			break;
		}
		if (!(packet >> checksumFrame))
		{
			ERROR_LOG(SLIPPI_ONLINE, "Netplay packet too small to read checksum frame");
			break;
		}
		if (!(packet >> checksum))
		{
			ERROR_LOG(SLIPPI_ONLINE, "Netplay packet too small to read checksum value");
			break;
		}
		u8 pIdx = PlayerIdxFromPort(packetPlayerPort);
		if (pIdx >= m_remotePlayerCount)
		{
			ERROR_LOG(SLIPPI_ONLINE, "Got packet with invalid player idx %d", pIdx);
			break;
		}

		// This is the amount of bytes from the start of the packet where the pad data starts
		int padDataOffset = 14;

		//ERROR_LOG(SLIPPI_ONLINE, "Received Checksum. CurFrame: %d, ChkFrame: %d, Chk: %08x", frame, checksumFrame, checksum);

		// This fetches the m_server index that stores the connection we want to overwrite (if necessary). Note that
		// this index is not necessarily the same as the pIdx because if we have users connecting with the same
		// WAN, the m_server indices might not match
		int connIdx = 0;
		for (int i = 0; i < m_server.size(); i++)
		{
			if (peer->address.host == m_server[i]->address.host && peer->address.port == m_server[i]->address.port)
			{
				connIdx = i;
				break;
			}
		}

		// Here we check if we have more than 1 connection for a specific player, this can happen because both
		// players try to connect to each other at the same time to increase the odds that one direction might
		// work and for hole punching. That said there's no point in keeping more than 1 connection alive. I
		// think they might use bandwidth with keep alives or something. Only the lower port player will
		// initiate the disconnect
		std::stringstream keyStrm;
		keyStrm << peer->address.host << "-" << peer->address.port;
		if (activeConnections[keyStrm.str()].size() > 1 && playerIdx <= pIdx)
		{
			m_server[connIdx] = peer;
			INFO_LOG(SLIPPI_ONLINE,
			         "Multiple connections detected for single peer. %x:%d. %x. Disconnecting superfluous "
			         "connections. oppIdx: %d. pIdx: %d",
			         peer->address.host, peer->address.port, peer, pIdx, playerIdx);

			for (auto activeConn : activeConnections[keyStrm.str()])
			{
				if (activeConn.first == peer)
					continue;

				// Tell our peer to terminate this connection
				enet_peer_disconnect(activeConn.first, 0);
			}
		}

		// Pad received, try to guess what our local time was when the frame was sent by our opponent
		// before we initialized
		// We can compare this to when we sent a pad for last frame to figure out how far/behind we
		// are with respect to the opponent
		auto timing = lastFrameTiming[pIdx];
		if (!hasGameStarted)
		{
			// Handle case where opponent starts sending inputs before our game has reached frame 1. This will
			// continuously say frame 0 is now to prevent opp from getting too far ahead
			timing.frame = 0;
			timing.timeUs = curTime;
		}

		s64 opponentSendTimeUs = curTime - (pingUs[pIdx] / 2);
		s64 frameDiffOffsetUs = 16683 * (timing.frame - frame);
		s64 timeOffsetUs = opponentSendTimeUs - timing.timeUs + frameDiffOffsetUs;

		// INFO_LOG(SLIPPI_ONLINE, "[Offset] Opp Frame: %d, My Frame: %d. Time offset: %lld", frame, timing.frame,
		//         timeOffsetUs);

		// Add this offset to circular buffer for use later
		if (frameOffsetData[pIdx].buf.size() < SLIPPI_ONLINE_LOCKSTEP_INTERVAL)
			frameOffsetData[pIdx].buf.push_back(static_cast<s32>(timeOffsetUs));
		else
			frameOffsetData[pIdx].buf[frameOffsetData[pIdx].idx] = static_cast<s32>(timeOffsetUs);

		frameOffsetData[pIdx].idx = (frameOffsetData[pIdx].idx + 1) % SLIPPI_ONLINE_LOCKSTEP_INTERVAL;

		s64 inputsToCopy;
		{
			std::lock_guard<std::mutex> lk(pad_mutex); // TODO: Is this the correct lock?

			auto packetData = (u8 *)packet.getData();

			// INFO_LOG(SLIPPI_ONLINE, "Receiving a packet of inputs from player %d(%d) [%d]...", packetPlayerPort,
			// pIdx,
			//         frame);

			s64 frame64 = static_cast<s64>(frame);
			s32 headFrame = remotePadQueue[pIdx].empty() ? 0 : remotePadQueue[pIdx].front()->frame;
			// Expand int size up to 64 bits to avoid overflowing
			inputsToCopy = frame64 - static_cast<s64>(headFrame);

			// Check that the packet actually contains the data it claims to
			if ((padDataOffset + inputsToCopy * SLIPPI_PAD_DATA_SIZE) > static_cast<s64>(packet.getDataSize()))
			{
				ERROR_LOG(SLIPPI_ONLINE,
				          "Netplay packet too small to read pad buffer. Size: %d, Inputs: %d, MinSize: %d",
				          (int)packet.getDataSize(), inputsToCopy, padDataOffset + inputsToCopy * SLIPPI_PAD_DATA_SIZE);
				break;
			}

			// Not sure what the max is here. If we never ack frames it could get big...
			if (inputsToCopy > 128) {
				ERROR_LOG(SLIPPI_ONLINE,
				          "Netplay packet contained too many frames: %d",
				          inputsToCopy);
				break;
			}

			for (s64 i = inputsToCopy - 1; i >= 0; i--)
			{
				auto pad = std::make_unique<SlippiPad>(static_cast<s32>(frame64 - i),
				                                       &packetData[padDataOffset + i * SLIPPI_PAD_DATA_SIZE]);
				// INFO_LOG(SLIPPI_ONLINE, "Rcv [%d] -> %02X %02X %02X %02X %02X %02X %02X %02X", pad->frame,
				//         pad->padBuf[0], pad->padBuf[1], pad->padBuf[2], pad->padBuf[3], pad->padBuf[4],
				//         pad->padBuf[5], pad->padBuf[6], pad->padBuf[7]);

				remotePadQueue[pIdx].push_front(std::move(pad));
			}

			// Write checksum pad to keep track of latest remote checksum
			ChecksumEntry e;
			e.frame = checksumFrame;
			e.value = checksum;
			remote_checksums[pIdx] = e;
		}

		// Only ack if inputsToCopy is greater than 0. Otherwise we are receiving an old input and
		// we should have already acked something in the future. This can also happen in the case
		// where a new game starts quickly before the remote queue is reset and if we ack the early
		// inputs we will never receive them
		if (inputsToCopy > 0)
		{
			// Send Ack
			sf::Packet spac;
			spac << (MessageId)NP_MSG_SLIPPI_PAD_ACK;
			spac << frame;
			spac << playerIdx;
			// INFO_LOG(SLIPPI_ONLINE, "Sending ack packet for frame %d (player %d) to peer at %d:%d", frame,
			// packetPlayerPort,
			//         peer->address.host, peer->address.port);

			ENetPacket *epac = enet_packet_create(spac.getData(), spac.getDataSize(), ENET_PACKET_FLAG_UNSEQUENCED);
			int sendResult = enet_peer_send(peer, 2, epac);
		}
	}
	break;

	case NP_MSG_SLIPPI_PAD_ACK:
	{
		std::lock_guard<std::mutex> lk(ack_mutex); // Trying to fix rare crash on ackTimers.count

		// Store last frame acked
		int32_t frame;
		if (!(packet >> frame))
		{
			ERROR_LOG(SLIPPI_ONLINE, "Ack packet too small to read frame");
			break;
		}
		u8 packetPlayerPort;
		if (!(packet >> packetPlayerPort))
		{
			ERROR_LOG(SLIPPI_ONLINE, "Netplay ack packet too small to read player index");
			break;
		}
		u8 pIdx = PlayerIdxFromPort(packetPlayerPort);
		if (pIdx >= m_remotePlayerCount)
		{
			ERROR_LOG(SLIPPI_ONLINE, "Got ack packet with invalid player idx %d", pIdx);
			break;
		}

		// INFO_LOG(SLIPPI_ONLINE, "Received ack packet from player %d(%d) [%d]...", packetPlayerPort, pIdx, frame);

		lastFrameAcked[pIdx] = frame > lastFrameAcked[pIdx] ? frame : lastFrameAcked[pIdx];

		// Remove old timings
		while (!ackTimers[pIdx].Empty() && ackTimers[pIdx].Front().frame < frame)
		{
			ackTimers[pIdx].Pop();
		}

		// Don't get a ping if we do not have the right ack frame
		if (ackTimers[pIdx].Empty() || ackTimers[pIdx].Front().frame != frame)
		{
			break;
		}

		auto sendTime = ackTimers[pIdx].Front().timeUs;
		ackTimers[pIdx].Pop();

		pingUs[pIdx] = Common::Timer::GetTimeUs() - sendTime;
		if (g_ActiveConfig.bShowNetPlayPing && frame % SLIPPI_PING_DISPLAY_INTERVAL == 0 && pIdx == 0)
		{
			std::stringstream pingDisplay;
			pingDisplay << "Ping: " << (pingUs[0] / 1000);
			for (int i = 1; i < m_remotePlayerCount; i++)
			{
				pingDisplay << " | " << (pingUs[i] / 1000);
			}
			OSD::AddTypedMessage(OSD::MessageType::NetPlayPing, pingDisplay.str(), OSD::Duration::NORMAL,
			                     OSD::Color::CYAN);
		}
	}
	break;

	case NP_MSG_SLIPPI_MATCH_SELECTIONS:
	{
		auto s = readSelectionsFromPacket(packet);
		if (!s->error)
		{
			INFO_LOG(SLIPPI_ONLINE, "[Netplay] Received selections from opponent with player idx %d", s->playerIdx);
			u8 idx = PlayerIdxFromPort(s->playerIdx);
			if (idx >= m_remotePlayerCount)
			{
				ERROR_LOG(SLIPPI_ONLINE, "Got match selection packet with invalid player idx %d", idx);
				break;
			}
			matchInfo.remotePlayerSelections[idx].Merge(*s);

			// This might be a good place to reset some logic? Game can't start until we receive this msg
			// so this should ensure that everything is initialized before the game starts
			hasGameStarted = false;

			// Reset remote pad queue such that next inputs that we get are not compared to inputs from last game
			remotePadQueue[idx].clear();
		}
	}
	break;

	case NP_MSG_SLIPPI_CHAT_MESSAGE:
	{
		auto playerSelection = ReadChatMessageFromPacket(packet);
		INFO_LOG(SLIPPI_ONLINE, "[Netplay] Received chat message from opponent %d: %d", playerSelection->playerIdx,
		         playerSelection->messageId);

		if (!playerSelection->error)
		{
			// set message id to netplay instance
			remoteChatMessageSelection = std::move(playerSelection);
		}
	}
	break;

	case NP_MSG_SLIPPI_CONN_SELECTED:
	{
		// Currently this is unused but the intent is to support two-way simultaneous connection attempts
		isConnectionSelected = true;
	}
	break;

	case NP_MSG_SLIPPI_COMPLETE_STEP:
	{
		SlippiGamePrepStepResults results;

		packet >> results.step_idx;
		packet >> results.char_selection;
		packet >> results.char_color_selection;
		packet >> results.stage_selections[0];
		packet >> results.stage_selections[1];

		gamePrepStepQueue.push_back(results);
	}
	break;

	case NP_MSG_SLIPPI_SYNCED_STATE:
	{
		u8 packetPlayerPort;
		if (!(packet >> packetPlayerPort))
		{
			ERROR_LOG(SLIPPI_ONLINE, "Netplay packet too small to read player index");
			break;
		}
		u8 pIdx = PlayerIdxFromPort(packetPlayerPort);
		if (pIdx >= m_remotePlayerCount)
		{
			ERROR_LOG(SLIPPI_ONLINE, "Got packet with invalid player idx %d", pIdx);
			break;
		}

		SlippiSyncedGameState results;
		packet >> results.match_id;
		packet >> results.game_index;
		packet >> results.tiebreak_index;
		packet >> results.seconds_remaining;
		for (int i = 0; i < 4; i++)
		{
			packet >> results.fighters[i].stocks_remaining;
			packet >> results.fighters[i].current_health;
		}

		//ERROR_LOG(SLIPPI_ONLINE, "Received synced state from opponent. %s, %d, %d, %d. F1: %d (%d%%), F2: %d (%d%%)",
		//         results.match_id.c_str(), results.game_index, results.tiebreak_index, results.seconds_remaining,
		//         results.fighters[0].stocks_remaining, results.fighters[0].current_health,
		//         results.fighters[1].stocks_remaining, results.fighters[1].current_health);

		remote_sync_states[pIdx] = results;
	}
	break;

	default:
		WARN_LOG(SLIPPI_ONLINE, "Unknown message received with id : %d", mid);
		break;
	}

	return 0;
}

void SlippiNetplayClient::writeToPacket(sf::Packet &packet, SlippiPlayerSelections &s)
{
	packet << static_cast<MessageId>(NP_MSG_SLIPPI_MATCH_SELECTIONS);
	packet << s.characterId << s.characterColor << s.isCharacterSelected;
	packet << s.playerIdx;
	packet << s.stageId << s.isStageSelected;
	packet << s.rngOffset;
	packet << s.teamId;
}

void SlippiNetplayClient::WriteChatMessageToPacket(sf::Packet &packet, int messageId, u8 playerIdx)
{
	packet << static_cast<MessageId>(NP_MSG_SLIPPI_CHAT_MESSAGE);
	packet << messageId;
	packet << playerIdx;
}

std::unique_ptr<SlippiPlayerSelections> SlippiNetplayClient::ReadChatMessageFromPacket(sf::Packet &packet)
{
	auto s = std::make_unique<SlippiPlayerSelections>();

	if (!(packet >> s->messageId))
	{
		ERROR_LOG(SLIPPI_ONLINE, "Chat packet too small to read message ID");
		s->error = true;
		return std::move(s);
	}
	if (!(packet >> s->playerIdx))
	{
		ERROR_LOG(SLIPPI_ONLINE, "Chat packet too small to read player index");
		s->error = true;
		return std::move(s);
	}

	switch (s->messageId)
	{
	// Only these 16 message IDs are allowed
	case 136:
	case 129:
	case 130:
	case 132:
	case 34:
	case 40:
	case 33:
	case 36:
	case 72:
	case 66:
	case 68:
	case 65:
	case 24:
	case 18:
	case 20:
	case 17:
	case SlippiPremadeText::CHAT_MSG_CHAT_DISABLED: // Opponent Chat Message Disabled
	{
		// Good message ID. Do nothing
		break;
	}
	default:
	{
		ERROR_LOG(SLIPPI_ONLINE, "Received invalid chat message index: %d", s->messageId);
		s->error = true;
		break;
	}
	}

	return std::move(s);
}

std::unique_ptr<SlippiPlayerSelections> SlippiNetplayClient::readSelectionsFromPacket(sf::Packet &packet)
{
	auto s = std::make_unique<SlippiPlayerSelections>();

	if (!(packet >> s->characterId))
	{
		ERROR_LOG(SLIPPI_ONLINE, "Received invalid player selection");
		s->error = true;
	}
	if (!(packet >> s->characterColor))
	{
		ERROR_LOG(SLIPPI_ONLINE, "Received invalid player selection");
		s->error = true;
	}
	if (!(packet >> s->isCharacterSelected))
	{
		ERROR_LOG(SLIPPI_ONLINE, "Received invalid player selection");
		s->error = true;
	}
	if (!(packet >> s->playerIdx))
	{
		ERROR_LOG(SLIPPI_ONLINE, "Received invalid player selection");
		s->error = true;
	}
	if (!(packet >> s->stageId))
	{
		ERROR_LOG(SLIPPI_ONLINE, "Received invalid player selection");
		s->error = true;
	}
	if (!(packet >> s->isStageSelected))
	{
		ERROR_LOG(SLIPPI_ONLINE, "Received invalid player selection");
		s->error = true;
	}
	if (!(packet >> s->rngOffset))
	{
		ERROR_LOG(SLIPPI_ONLINE, "Received invalid player selection");
		s->error = true;
	}
	if (!(packet >> s->teamId))
	{
		ERROR_LOG(SLIPPI_ONLINE, "Received invalid player selection");
		s->error = true;
	}
	return std::move(s);
}

void SlippiNetplayClient::Send(sf::Packet &packet)
{
	enet_uint32 flags = ENET_PACKET_FLAG_RELIABLE;
	u8 channelId = 0;

	for (int i = 0; i < m_server.size(); i++)
	{
		MessageId mid = ((u8 *)packet.getData())[0];
		if (mid == NP_MSG_SLIPPI_PAD || mid == NP_MSG_SLIPPI_PAD_ACK)
		{
			// Slippi communications do not need reliable connection and do not need to
			// be received in order. Channel is changed so that other reliable communications
			// do not block anything. This may not be necessary if order is not maintained?
			flags = ENET_PACKET_FLAG_UNSEQUENCED;
			channelId = 1;
		}

		ENetPacket *epac = enet_packet_create(packet.getData(), packet.getDataSize(), flags);
		int sendResult = enet_peer_send(m_server[i], channelId, epac);
	}
}

void SlippiNetplayClient::Disconnect()
{
	ENetEvent netEvent;
	slippiConnectStatus = SlippiConnectStatus::NET_CONNECT_STATUS_DISCONNECTED;
	if (activeConnections.empty())
	{
		return;
	}

	for (auto conn : activeConnections)
	{
		for (auto peer : conn.second)
		{
			INFO_LOG(SLIPPI_ONLINE, "[Netplay] Disconnecting peer %d", peer.first->address.port);
			enet_peer_disconnect(peer.first, 0);
		}
	}

	while (enet_host_service(m_client, &netEvent, 3000) > 0)
	{
		switch (netEvent.type)
		{
		case ENET_EVENT_TYPE_RECEIVE:
			enet_packet_destroy(netEvent.packet);
			break;
		case ENET_EVENT_TYPE_DISCONNECT:
			INFO_LOG(SLIPPI_ONLINE, "[Netplay] Got disconnect from peer %d", netEvent.peer->address.port);
			break;
		default:
			break;
		}
	}

	// didn't disconnect gracefully force disconnect
	for (auto conn : activeConnections)
	{
		for (auto peer : conn.second)
		{
			enet_peer_reset(peer.first);
		}
	}
	activeConnections.clear();
	m_server.clear();
	SLIPPI_NETPLAY = nullptr;
}

void SlippiNetplayClient::SendAsync(std::unique_ptr<sf::Packet> packet)
{
	{
		std::lock_guard<std::recursive_mutex> lkq(m_crit.async_queue_write);
		m_async_queue.Push(std::move(packet));
	}
	ENetUtil::WakeupThread(m_client);
}

// called from ---NETPLAY--- thread
void SlippiNetplayClient::ThreadFunc()
{
	// Let client die 1 second before host such that after a swap, the client won't be connected to
	u64 startTime = Common::Timer::GetTimeMs();
	u64 timeout = 8000;

	std::vector<bool> connections;
	std::vector<ENetAddress> remoteAddrs;
	for (int i = 0; i < m_remotePlayerCount; i++)
	{
		remoteAddrs.push_back(m_server[i]->address);
		connections.push_back(false);
	}

	while (slippiConnectStatus == SlippiConnectStatus::NET_CONNECT_STATUS_INITIATED)
	{
		// This will confirm that connection went through successfully
		ENetEvent netEvent;
		int net = enet_host_service(m_client, &netEvent, 500);
		if (net > 0)
		{
			sf::Packet rpac;
			switch (netEvent.type)
			{
			case ENET_EVENT_TYPE_RECEIVE:
				if (!netEvent.peer)
				{
					INFO_LOG(SLIPPI_ONLINE, "[Netplay] got receive event with nil peer");
					continue;
				}
				INFO_LOG(SLIPPI_ONLINE, "[Netplay] got receive event with peer addr %x:%d", netEvent.peer->address.host,
				         netEvent.peer->address.port);
				rpac.append(netEvent.packet->data, netEvent.packet->dataLength);

				OnData(rpac, netEvent.peer);

				enet_packet_destroy(netEvent.packet);
				break;

			case ENET_EVENT_TYPE_DISCONNECT:
				if (!netEvent.peer)
				{
					INFO_LOG(SLIPPI_ONLINE, "[Netplay] got disconnect event with nil peer");
					continue;
				}
				INFO_LOG(SLIPPI_ONLINE, "[Netplay] got disconnect event with peer addr %x:%d. %x",
				         netEvent.peer->address.host, netEvent.peer->address.port, netEvent.peer);
				break;

			case ENET_EVENT_TYPE_CONNECT:
			{
				if (!netEvent.peer)
				{
					INFO_LOG(SLIPPI_ONLINE, "[Netplay] got connect event with nil peer");
					continue;
				}

				std::stringstream keyStrm;
				keyStrm << netEvent.peer->address.host << "-" << netEvent.peer->address.port;
				activeConnections[keyStrm.str()][netEvent.peer] = true;
				INFO_LOG(SLIPPI_ONLINE, "New connection (early): %s, %X", keyStrm.str().c_str(), netEvent.peer);

				for (auto pair : activeConnections)
				{
					INFO_LOG(SLIPPI_ONLINE, "%s: %d", pair.first.c_str(), pair.second.size());
				}

				INFO_LOG(SLIPPI_ONLINE, "[Netplay] got connect event with peer addr %x:%d. %x",
				         netEvent.peer->address.host, netEvent.peer->address.port, netEvent.peer);

				auto isAlreadyConnected = false;
				for (int i = 0; i < m_server.size(); i++)
				{
					if (connections[i] && netEvent.peer->address.host == m_server[i]->address.host &&
					    netEvent.peer->address.port == m_server[i]->address.port)
					{
						m_server[i] = netEvent.peer;
						isAlreadyConnected = true;
						break;
					}
				}

				if (isAlreadyConnected)
				{
					// Don't add this person again if they are already connected. Not doing this can cause one person to
					// take up 2 or more spots, denying one or more players from connecting and thus getting stuck on
					// the "Waiting" step
					INFO_LOG(SLIPPI_ONLINE, "Already connected!");
					break; // Breaks out of case
				}

				for (int i = 0; i < m_server.size(); i++)
				{
					// This check used to check for port as well as host. The problem was that for some people, their
					// internet will switch the port they're sending from. This means these people struggle to connect
					// to others but they sometimes do succeed. When we were checking for port here though we would get
					// into a state where the person they succeeded to connect to would not accept the connection with
					// them, this would lead the player with this internet issue to get stuck waiting for the other
					// player. The only downside to this that I can guess is that if you fail to connect to one person
					// out of two that are on your LAN, it might report that you failed to connect to the wrong person.
					// There might be more problems tho, not sure
					INFO_LOG(SLIPPI_ONLINE, "[Netplay] Comparing connection address: %x - %x", remoteAddrs[i].host,
					         netEvent.peer->address.host);
					if (remoteAddrs[i].host == netEvent.peer->address.host && !connections[i])
					{
						INFO_LOG(SLIPPI_ONLINE, "[Netplay] Overwriting ENetPeer for address: %x:%d",
						         netEvent.peer->address.host, netEvent.peer->address.port);
						INFO_LOG(SLIPPI_ONLINE, "[Netplay] Overwriting ENetPeer with id (%d) with new peer of id %d",
						         m_server[i]->connectID, netEvent.peer->connectID);
						m_server[i] = netEvent.peer;
						connections[i] = true;
						break;
					}
				}
				break;
			}
			}
		}

		bool allConnected = true;
		for (int i = 0; i < m_remotePlayerCount; i++)
		{
			if (!connections[i])
				allConnected = false;
		}

		if (allConnected)
		{
			m_client->intercept = ENetUtil::InterceptCallback;
			INFO_LOG(SLIPPI_ONLINE, "Slippi online connection successful!");
			slippiConnectStatus = SlippiConnectStatus::NET_CONNECT_STATUS_CONNECTED;
			break;
		}

		for (int i = 0; i < m_remotePlayerCount; i++)
		{
			INFO_LOG(SLIPPI_ONLINE, "m_client peer %d state: %d", i, m_client->peers[i].state);
		}
		INFO_LOG(SLIPPI_ONLINE, "[Netplay] Not yet connected. Res: %d, Type: %d", net, netEvent.type);

		// Time out after enough time has passed
		u64 curTime = Common::Timer::GetTimeMs();
		if ((curTime - startTime) >= timeout || !m_do_loop.IsSet())
		{
			for (int i = 0; i < m_remotePlayerCount; i++)
			{
				if (!connections[i])
				{
					failedConnections.push_back(i);
				}
			}

			slippiConnectStatus = SlippiConnectStatus::NET_CONNECT_STATUS_FAILED;
			INFO_LOG(SLIPPI_ONLINE, "Slippi online connection failed");
			return;
		}
	}

	INFO_LOG(SLIPPI_ONLINE, "Successfully initialized %d connections", m_server.size());
	for (int i = 0; i < m_server.size(); i++)
	{
		INFO_LOG(SLIPPI_ONLINE, "Connection %d: %d, %d", i, m_server[i]->address.host, m_server[i]->address.port);
	}

	bool qos_success = false;
#ifdef _WIN32
	QOS_VERSION ver = {1, 0};

	if (SConfig::GetInstance().bQoSEnabled && QOSCreateHandle(&ver, &m_qos_handle))
	{
		for (int i = 0; i < m_server.size(); i++)
		{
			// from win32.c
			struct sockaddr_in sin = {0};

			sin.sin_family = AF_INET;
			sin.sin_port = ENET_HOST_TO_NET_16(m_server[i]->host->address.port);
			sin.sin_addr.s_addr = m_server[i]->host->address.host;

			if (QOSAddSocketToFlow(m_qos_handle, m_server[i]->host->socket, reinterpret_cast<PSOCKADDR>(&sin),
			                       // this is 0x38
			                       QOSTrafficTypeControl, QOS_NON_ADAPTIVE_FLOW, &m_qos_flow_id))
			{
				DWORD dscp = 0x2e;

				// this will fail if we're not admin
				// sets DSCP to the same as linux (0x2e)
				QOSSetFlow(m_qos_handle, m_qos_flow_id, QOSSetOutgoingDSCPValue, sizeof(DWORD), &dscp, 0, nullptr);

				qos_success = true;
			}
		}
	}
#else
	if (SConfig::GetInstance().bQoSEnabled)
	{
		for (int i = 0; i < m_server.size(); i++)
		{
#ifdef __linux__
			// highest priority
			int priority = 7;
			setsockopt(m_server[i]->host->socket, SOL_SOCKET, SO_PRIORITY, &priority, sizeof(priority));
#endif

			// https://www.tucny.com/Home/dscp-tos
			// ef is better than cs7
			int tos_val = 0xb8;
			qos_success = setsockopt(m_server[i]->host->socket, IPPROTO_IP, IP_TOS, &tos_val, sizeof(tos_val)) == 0;
		}
	}
#endif

	while (m_do_loop.IsSet())
	{
		ENetEvent netEvent;
		int net;
		net = enet_host_service(m_client, &netEvent, 250);
		while (!m_async_queue.Empty())
		{
			Send(*(m_async_queue.Front().get()));
			m_async_queue.Pop();
		}

		if (net > 0)
		{
			sf::Packet rpac;
			bool isConnectedClient = false;
			switch (netEvent.type)
			{
			case ENET_EVENT_TYPE_RECEIVE:
			{
				rpac.append(netEvent.packet->data, netEvent.packet->dataLength);
				OnData(rpac, netEvent.peer);
				enet_packet_destroy(netEvent.packet);
				break;
			}
			case ENET_EVENT_TYPE_DISCONNECT:
			{
				std::stringstream keyStrm;
				keyStrm << netEvent.peer->address.host << "-" << netEvent.peer->address.port;
				activeConnections[keyStrm.str()].erase(netEvent.peer);

				for (auto pair : activeConnections)
				{
					INFO_LOG(SLIPPI_ONLINE, "%s: %d", pair.first.c_str(), pair.second.size());
				}

				// Check to make sure this address+port are one of the ones we are actually connected to.
				// When connecting to someone that randomizes ports, you can get one valid connection from
				// one port and a failed connection on another port. We don't want to cause a real disconnect
				// if we receive a disconnect message from the port we never connected to
				bool isConnectedClient = false;
				for (int i = 0; i < m_server.size(); i++)
				{
					if (netEvent.peer->address.host == m_server[i]->address.host &&
					    netEvent.peer->address.port == m_server[i]->address.port)
					{
						isConnectedClient = true;
						break;
					}
				}

				INFO_LOG(SLIPPI_ONLINE,
				         "[Netplay] Disconnect late %x:%d. %x. Remaining connections: %d. Is connected client: %s",
				         netEvent.peer->address.host, netEvent.peer->address.port, netEvent.peer,
				         activeConnections[keyStrm.str()].size(), isConnectedClient ? "true" : "false");

				// If the disconnect event doesn't come from the client we are actually listening to,
				// it can be safely ignored
				if (isConnectedClient && activeConnections[keyStrm.str()].empty())
				{
					INFO_LOG(SLIPPI_ONLINE, "[Netplay] Final disconnect received for a client.");
					m_do_loop.Clear(); // Stop the loop, will trigger a disconnect
				}
				break;
			}
			case ENET_EVENT_TYPE_CONNECT:
			{
				std::stringstream keyStrm;
				keyStrm << netEvent.peer->address.host << "-" << netEvent.peer->address.port;
				activeConnections[keyStrm.str()][netEvent.peer] = true;
				INFO_LOG(SLIPPI_ONLINE, "New connection (late): %s, %X", keyStrm.str().c_str(), netEvent.peer);
				for (auto pair : activeConnections)
				{
					INFO_LOG(SLIPPI_ONLINE, "%s: %d", pair.first.c_str(), pair.second.size());
				}
				break;
			}
			default:
				break;
			}
		}
	}

#ifdef _WIN32
	if (m_qos_handle != 0)
	{
		if (m_qos_flow_id != 0)
		{
			for (int i = 0; i < m_server.size(); i++)
			{
				QOSRemoveSocketFromFlow(m_qos_handle, m_server[i]->host->socket, m_qos_flow_id, 0);
			}
		}

		QOSCloseHandle(m_qos_handle);
	}
#endif

	Disconnect();
	return;
}

bool SlippiNetplayClient::IsDecider()
{
	return isDecider;
}

bool SlippiNetplayClient::IsConnectionSelected()
{
	return isConnectionSelected;
}

SlippiNetplayClient::SlippiConnectStatus SlippiNetplayClient::GetSlippiConnectStatus()
{
	return slippiConnectStatus;
}

std::vector<int> SlippiNetplayClient::GetFailedConnections()
{
	return failedConnections;
}

void SlippiNetplayClient::StartSlippiGame()
{
	// Reset variables to start a new game
	hasGameStarted = false;

	localPadQueue.clear();

	for (int i = 0; i < m_remotePlayerCount; i++)
	{
		FrameTiming timing;
		timing.frame = 0;
		timing.timeUs = Common::Timer::GetTimeUs();
		lastFrameTiming[i] = timing;
		lastFrameAcked[i] = 0;

		// Reset ack timers
		ackTimers[i].Clear();
	}

	is_desync_recovery = false;

	// Clear game prep queue in case anything is still lingering
	gamePrepStepQueue.clear();

	// Reset match info for next game
	matchInfo.Reset();
}

void SlippiNetplayClient::SendConnectionSelected()
{
	isConnectionSelected = true;

	auto spac = std::make_unique<sf::Packet>();
	*spac << static_cast<MessageId>(NP_MSG_SLIPPI_CONN_SELECTED);
	SendAsync(std::move(spac));
}

void SlippiNetplayClient::SendSlippiPad(std::unique_ptr<SlippiPad> pad)
{
	auto status = slippiConnectStatus;
	bool connectionFailed = status == SlippiNetplayClient::SlippiConnectStatus::NET_CONNECT_STATUS_FAILED;
	bool connectionDisconnected = status == SlippiNetplayClient::SlippiConnectStatus::NET_CONNECT_STATUS_DISCONNECTED;
	if (connectionFailed || connectionDisconnected)
	{
		return;
	}

	// if (pad && isDecider)
	//{
	//  ERROR_LOG(SLIPPI_ONLINE, "[%d] %X %X %X %X %X %X %X %X", pad->frame, pad->padBuf[0], pad->padBuf[1],
	//  pad->padBuf[2], pad->padBuf[3], pad->padBuf[4], pad->padBuf[5], pad->padBuf[6], pad->padBuf[7]);
	//}

	if (pad)
	{
		// Add latest local pad report to queue
		localPadQueue.push_front(std::move(pad));
	}

	// Remove pad reports that have been received and acked
	int minAckFrame = lastFrameAcked[0];
	for (int i = 1; i < m_remotePlayerCount; i++)
	{
		if (lastFrameAcked[i] < minAckFrame)
			minAckFrame = lastFrameAcked[i];
	}
	// INFO_LOG(SLIPPI_ONLINE, "Checking to drop local inputs, oldest frame: %d | minAckFrame: %d | %d, %d, %d",
	//         localPadQueue.back()->frame, minAckFrame, lastFrameAcked[0], lastFrameAcked[1], lastFrameAcked[2]);
	while (!localPadQueue.empty() && localPadQueue.back()->frame < minAckFrame)
	{
		// INFO_LOG(SLIPPI_ONLINE, "Dropping local input for frame %d from queue", localPadQueue.back()->frame);
		localPadQueue.pop_back();
	}

	if (localPadQueue.empty())
	{
		// If pad queue is empty now, there's no reason to send anything
		return;
	}

	auto frame = localPadQueue.front()->frame;

	auto spac = std::make_unique<sf::Packet>();
	*spac << static_cast<MessageId>(NP_MSG_SLIPPI_PAD);
	*spac << frame;
	*spac << this->playerIdx;
	*spac << localPadQueue.front()->checksumFrame;
	*spac << localPadQueue.front()->checksum;

	// INFO_LOG(SLIPPI_ONLINE, "Sending a packet of inputs [%d]...", frame);
	for (auto it = localPadQueue.begin(); it != localPadQueue.end(); ++it)
	{
		// INFO_LOG(SLIPPI_ONLINE, "Send [%d] -> %02X %02X %02X %02X %02X %02X %02X %02X", (*it)->frame,
		// (*it)->padBuf[0],
		//         (*it)->padBuf[1], (*it)->padBuf[2], (*it)->padBuf[3], (*it)->padBuf[4], (*it)->padBuf[5],
		//         (*it)->padBuf[6], (*it)->padBuf[7]);
		spac->append((*it)->padBuf, SLIPPI_PAD_DATA_SIZE); // only transfer 8 bytes per pad
	}

	SendAsync(std::move(spac));

	u64 time = Common::Timer::GetTimeUs();

	hasGameStarted = true;

	for (int i = 0; i < m_remotePlayerCount; i++)
	{
		FrameTiming timing;
		timing.frame = frame;
		timing.timeUs = time;
		lastFrameTiming[i] = timing;

		// Add send time to ack timers
		FrameTiming sendTime;
		sendTime.frame = frame;
		sendTime.timeUs = time;
		ackTimers[i].Push(sendTime);
	}
}

void SlippiNetplayClient::SetMatchSelections(SlippiPlayerSelections &s)
{
	matchInfo.localPlayerSelections.Merge(s);
	matchInfo.localPlayerSelections.playerIdx = playerIdx;

	// Send packet containing selections
	auto spac = std::make_unique<sf::Packet>();
	INFO_LOG(SLIPPI_ONLINE, "Setting match selections for %d", playerIdx);
	writeToPacket(*spac, matchInfo.localPlayerSelections);
	SendAsync(std::move(spac));
}

void SlippiNetplayClient::SendGamePrepStep(SlippiGamePrepStepResults &s)
{
	auto spac = std::make_unique<sf::Packet>();
	*spac << static_cast<MessageId>(NP_MSG_SLIPPI_COMPLETE_STEP);
	*spac << s.step_idx;
	*spac << s.char_selection;
	*spac << s.char_color_selection;
	*spac << s.stage_selections[0] << s.stage_selections[1];
	SendAsync(std::move(spac));
}

void SlippiNetplayClient::SendSyncedGameState(SlippiSyncedGameState &s) {
	//WARN_LOG(SLIPPI_ONLINE, "Sending synced state. %s, %d, %d, %d. F1: %d (%d%%), F2: %d (%d%%)",
	//          s.match_id.c_str(), s.game_index, s.tiebreak_index, s.seconds_remaining,
	//          s.fighters[0].stocks_remaining, s.fighters[0].current_health,
	//          s.fighters[1].stocks_remaining, s.fighters[1].current_health);

	is_desync_recovery = true;
	local_sync_state = s;

	auto spac = std::make_unique<sf::Packet>();
	*spac << static_cast<MessageId>(NP_MSG_SLIPPI_SYNCED_STATE);
	*spac << this->playerIdx;
	*spac << s.match_id;
	*spac << s.game_index;
	*spac << s.tiebreak_index;
	*spac << s.seconds_remaining;
	for (int i = 0; i < 4; i++)
	{
		*spac << s.fighters[i].stocks_remaining;
		*spac << s.fighters[i].current_health;
	}
	SendAsync(std::move(spac));
}

bool SlippiNetplayClient::GetGamePrepResults(u8 stepIdx, SlippiGamePrepStepResults &res)
{
	// Just pull stuff off until we find something for the right step. I think that should be fine
	while (!gamePrepStepQueue.empty())
	{
		auto front = gamePrepStepQueue.front();
		if (front.step_idx == stepIdx)
		{
			res = front;
			return true;
		}

		gamePrepStepQueue.pop_front();
	}

	return false;
}

SlippiPlayerSelections SlippiNetplayClient::GetSlippiRemoteChatMessage(bool isChatEnabled)
{
	SlippiPlayerSelections copiedSelection = SlippiPlayerSelections();

	if (remoteChatMessageSelection != nullptr && isChatEnabled)
	{
		copiedSelection.messageId = remoteChatMessageSelection->messageId;
		copiedSelection.playerIdx = remoteChatMessageSelection->playerIdx;

		// Clear it out
		remoteChatMessageSelection->messageId = 0;
		remoteChatMessageSelection->playerIdx = 0;
	}
	else
	{
		copiedSelection.messageId = 0;
		copiedSelection.playerIdx = 0;

		// if chat is not enabled, automatically send back a message saying so.
		if (remoteChatMessageSelection != nullptr && !isChatEnabled &&
		    (remoteChatMessageSelection->messageId > 0 &&
		     remoteChatMessageSelection->messageId != SlippiPremadeText::CHAT_MSG_CHAT_DISABLED))
		{
			auto packet = std::make_unique<sf::Packet>();
			remoteSentChatMessageId = SlippiPremadeText::CHAT_MSG_CHAT_DISABLED;
			WriteChatMessageToPacket(*packet, remoteSentChatMessageId, LocalPlayerPort());
			SendAsync(std::move(packet));
			remoteSentChatMessageId = 0;
			remoteChatMessageSelection = nullptr;
		}
	}

	return copiedSelection;
}

u8 SlippiNetplayClient::GetSlippiRemoteSentChatMessage(bool isChatEnabled)
{
	if (!isChatEnabled)
	{
		return 0;
	}
	u8 copiedMessageId = remoteSentChatMessageId;
	remoteSentChatMessageId = 0; // Clear it out
	return copiedMessageId;
}

std::unique_ptr<SlippiRemotePadOutput> SlippiNetplayClient::GetFakePadOutput(int frame) {
	// Used for testing purposes, will ignore the opponent's actual inputs and provide fake
	// ones to trigger rollback scenarios
	std::unique_ptr<SlippiRemotePadOutput> padOutput = std::make_unique<SlippiRemotePadOutput>();

	// Triggers rollback where the first few inputs were correctly predicted
	if (frame % 60 < 5)
	{
		// Return old inputs for a bit
		padOutput->latestFrame = frame - (frame % 60);
		padOutput->data.insert(padOutput->data.begin(), SLIPPI_PAD_FULL_SIZE, 0);
	}
	else if (frame % 60 == 5)
	{
		padOutput->latestFrame = frame;
		// Add 5 frames of 0'd inputs
		padOutput->data.insert(padOutput->data.begin(), 5*SLIPPI_PAD_FULL_SIZE, 0);

		// Press A button for 2 inputs prior to this frame causing a rollback
		padOutput->data[2*SLIPPI_PAD_FULL_SIZE] = 1;
	}
	else
	{
		padOutput->latestFrame = frame;
		padOutput->data.insert(padOutput->data.begin(), SLIPPI_PAD_FULL_SIZE, 0);
	}

	return std::move(padOutput);
}

std::unique_ptr<SlippiRemotePadOutput> SlippiNetplayClient::GetSlippiRemotePad(int index, int maxFrameCount)
{
	std::lock_guard<std::mutex> lk(pad_mutex); // TODO: Is this the correct lock?

	std::unique_ptr<SlippiRemotePadOutput> padOutput = std::make_unique<SlippiRemotePadOutput>();

	if (remotePadQueue[index].empty())
	{
		auto emptyPad = std::make_unique<SlippiPad>(0);

		padOutput->latestFrame = emptyPad->frame;

		auto emptyIt = std::begin(emptyPad->padBuf);
		padOutput->data.insert(padOutput->data.end(), emptyIt, emptyIt + SLIPPI_PAD_FULL_SIZE);

		return std::move(padOutput);
	}

	int inputCount = 0;

	padOutput->latestFrame = 0;
	padOutput->checksumFrame = remote_checksums[index].frame;
	padOutput->checksum = remote_checksums[index].value;

	// Copy inputs from the remote pad queue to the output. We iterate backwards because
	// we want to get the oldest frames possible (will have been cleared to contain the last
	// finalized frame at the back). I think it's very unlikely but I think before we
	// iterated from the front and it's possible the 7 frame limit left out an input the
	// game actually needed.
	for (auto it = remotePadQueue[index].rbegin(); it != remotePadQueue[index].rend(); ++it)
	{
		if ((*it)->frame > padOutput->latestFrame)
			padOutput->latestFrame = (*it)->frame;

		//NOTICE_LOG(SLIPPI_ONLINE, "[%d] (Remote) P%d %08X %08X %08X", (*it)->frame,
		//						index >= playerIdx ? index + 1 : index, Common::swap32(&(*it)->padBuf[0]),
		//						Common::swap32(&(*it)->padBuf[4]), Common::swap32(&(*it)->padBuf[8]));

		auto padIt = std::begin((*it)->padBuf);
		padOutput->data.insert(padOutput->data.begin(), padIt, padIt + SLIPPI_PAD_FULL_SIZE);

		// Limit max amount of inputs to send
		inputCount++;
		if (inputCount >= maxFrameCount)
			break;
	}

	return std::move(padOutput);
}

void SlippiNetplayClient::DropOldRemoteInputs(int32_t finalizedFrame)
{
	std::lock_guard<std::mutex> lk(pad_mutex);

	// INFO_LOG(SLIPPI_ONLINE, "Checking for remotePadQueue inputs to drop, lowest common: %d, [0]: %d, [1]: %d, [2]:
	// %d",
	//         lowestCommonFrame, playerFrame[0], playerFrame[1], playerFrame[2]);
	for (int i = 0; i < m_remotePlayerCount; i++)
	{
		// INFO_LOG(SLIPPI_ONLINE, "remotePadQueue[%d] size: %d", i, remotePadQueue[i].size());
		while (remotePadQueue[i].size() > 1 && remotePadQueue[i].back()->frame < finalizedFrame)
		{
			// INFO_LOG(SLIPPI_ONLINE, "Popping inputs for frame %d from back of player %d queue",
			//         remotePadQueue[i].back()->frame, i);
			remotePadQueue[i].pop_back();
		}
	}
}

SlippiMatchInfo *SlippiNetplayClient::GetMatchInfo()
{
	return &matchInfo;
}

int32_t SlippiNetplayClient::GetSlippiLatestRemoteFrame(int maxFrameCount)
{
	// Return the lowest frame among remote queues
	int lowestFrame = 0;
	bool isFrameSet = false;
	for (int i = 0; i < m_remotePlayerCount; i++)
	{
		auto rp = GetSlippiRemotePad(i, maxFrameCount);
		int f = rp->latestFrame;
		if (f < lowestFrame || !isFrameSet)
		{
			lowestFrame = f;
			isFrameSet = true;
		}
	}

	return lowestFrame;
}

// return the smallest time offset among all remote players
s32 SlippiNetplayClient::CalcTimeOffsetUs()
{
	bool empty = true;
	for (int i = 0; i < m_remotePlayerCount; i++)
	{
		if (!frameOffsetData[i].buf.empty())
		{
			empty = false;
			break;
		}
	}
	if (empty)
	{
		return 0;
	}

	std::vector<int> offsets;
	for (int i = 0; i < m_remotePlayerCount; i++)
	{
		if (frameOffsetData[i].buf.empty())
			continue;

		std::vector<s32> buf;
		std::copy(frameOffsetData[i].buf.begin(), frameOffsetData[i].buf.end(), std::back_inserter(buf));

		// TODO: Does this work?
		std::sort(buf.begin(), buf.end());

		int bufSize = (int)buf.size();
		int offset = (int)((1.0f / 3.0f) * bufSize);
		int end = bufSize - offset;

		int sum = 0;
		for (int i = offset; i < end; i++)
		{
			sum += buf[i];
		}

		int count = end - offset;
		if (count <= 0)
		{
			return 0; // What do I return here?
		}

		s32 result = sum / count;
		offsets.push_back(result);
	}

	s32 minOffset = offsets.front();
	for (int i = 1; i < offsets.size(); i++)
	{
		if (offsets[i] < minOffset)
			minOffset = offsets[i];
	}

	// INFO_LOG(SLIPPI_ONLINE, "Time offsets, [0]: %d, [1]: %d, [2]: %d", offsets[0], offsets[1], offsets[2]);
	return minOffset;
}

bool SlippiNetplayClient::IsWaitingForDesyncRecovery()
{
	// If we are not in a desync recovery state, we do not need to wait
	if (!is_desync_recovery)
		return false;

	for (int i = 0; i < m_remotePlayerCount; i++)
	{
		if (local_sync_state.game_index != remote_sync_states[i].game_index)
			return true;

		if (local_sync_state.tiebreak_index != remote_sync_states[i].tiebreak_index)
			return true;
	}

	return false;
}

SlippiDesyncRecoveryResp SlippiNetplayClient::GetDesyncRecoveryState()
{
	SlippiDesyncRecoveryResp result;

	result.is_recovering = is_desync_recovery;
	result.is_waiting = IsWaitingForDesyncRecovery();

	// If we are not recovering or if we are currently waiting, don't need to compute state, just return
	if (!result.is_recovering || result.is_waiting)
		return result;

	result.state = local_sync_state;

	// Here let's try to reconcile all the states into one. This is important to make sure
	// everyone starts at the same percent/stocks because their last synced state might be
	// a slightly different frame. There didn't seem to be an easy way to guarantee they'd
	// all be on exactly the same frame
	for (int i = 0; i < m_remotePlayerCount; i++)
	{
		auto &s = remote_sync_states[i];
		if (abs(static_cast<int>(result.state.seconds_remaining) - static_cast<int>(s.seconds_remaining)) > 1)
		{
			ERROR_LOG(SLIPPI_ONLINE, "Timer values for desync recovery too different: %d, %d",
			          result.state.seconds_remaining, s.seconds_remaining);
			result.is_error = true;
			return result;
		}

		// Use the timer with more time remaining
		if (s.seconds_remaining > result.state.seconds_remaining)
		{
			result.state.seconds_remaining = s.seconds_remaining;
		}

		for (int j = 0; j < 4; j++)
		{
			auto &fighter = result.state.fighters[i];
			auto &iFighter = s.fighters[i];

			if (fighter.stocks_remaining != iFighter.stocks_remaining)
			{
				// This might actually happen sometimes if a desync happens right as someone is KO'd... should be
				// quite rare though in a 1v1 situation.
				ERROR_LOG(SLIPPI_ONLINE, "Stocks remaining for desync recovery do not match: [Player %d] %d, %d",
				          j + 1, fighter.stocks_remaining, iFighter.stocks_remaining);
				result.is_error = true;
				return result;
			}

			if (abs(static_cast<int>(fighter.current_health) - static_cast<int>(iFighter.current_health)) > 25)
			{
				ERROR_LOG(SLIPPI_ONLINE, "Current health for desync recovery too different: [Player %d] %d, %d", j + 1,
				          fighter.current_health, iFighter.current_health);
				result.is_error = true;
				return result;
			}

			// Use the lower health value
			if (iFighter.current_health < fighter.current_health)
			{
				result.state.fighters[i].current_health = iFighter.current_health;
			}
		}
	}

	return result;
}
