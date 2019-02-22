// Copyright (c) 2019 The Crypticcoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
#include "heartbeat.h"
#include "masternodes.h"
#include "../net.h"
#include "../util.h"
#include "../init.h"
#include "../wallet/wallet.h"
#include "../snark/libsnark/common/utils.hpp"

namespace
{
std::mutex mutex{};
CHeartBeatTracker* instance{nullptr};
std::array<unsigned char, 16> salt_{0x36, 0x4D, 0x2B, 0x44, 0x58, 0x37, 0x78, 0x39, 0x7A, 0x78, 0x5E, 0x58, 0x68, 0x7A, 0x35, 0x75};

CKey getMasternodeKey()
{
    CKey rv{};
#ifdef ENABLE_WALLET
    LOCK2(cs_main, pwalletMain->cs_wallet);
    const boost::optional<CMasternodesView::CMasternodeIDs> mnId{pmasternodesview->AmIActiveOperator()};
    if (mnId != boost::none) {
        if (!pwalletMain->GetKey(mnId.get().operatorAuthAddress, rv)) {
            LogPrintf("%s: Can't read masternode operator private key", __func__);
            rv = CKey{};
        }
    }
#endif
    return rv;
}

bool checkMasternodeKey(const CKeyID& keyId)
{
    LOCK(cs_main);
    return pmasternodesview->ExistMasternode(CMasternodesView::AuthIndex::ByOperator, keyId) != boost::none;
}

}

CHeartBeatMessage::CHeartBeatMessage(const int64_t timestamp)
{
    this->timestamp = timestamp;
}

int64_t CHeartBeatMessage::GetTimestamp() const
{
    return timestamp;
}

CHeartBeatMessage::Signature CHeartBeatMessage::GetSignature() const
{
    return signature;
}

uint256 CHeartBeatMessage::GetHash() const
{
    CDataStream ss{SER_NETWORK, PROTOCOL_VERSION};
    ss << *this;
    return Hash(ss.begin(), ss.end());
}

void CHeartBeatMessage::SetNull()
{
    timestamp = 0;
    signature.clear();
}

bool CHeartBeatMessage::IsNull() const
{
    return timestamp == 0 || signature.empty();
}

bool CHeartBeatMessage::SignWithKey(const CKey& key)
{
    signature.resize(CPubKey::COMPACT_SIGNATURE_SIZE);
    if (!key.SignCompact(getSignHash(), signature)) {
        signature.clear();
    }
    return !IsNull();
}

bool CHeartBeatMessage::GetPubKey(CPubKey& pubKey) const
{
    return !IsNull() && pubKey.RecoverCompact(getSignHash(), GetSignature());
}

uint256 CHeartBeatMessage::getSignHash() const
{
    CDataStream ss{SER_GETHASH, PROTOCOL_VERSION};
    ss << timestamp << salt_;
    return Hash(ss.begin(), ss.end());
}

void CHeartBeatTracker::runTickerLoop()
{
    CKey operKey{};
    CHeartBeatTracker tracker{};
    std::int64_t delay{tracker.getMinPeriod() * 2};

//    mns::mockMasternodesDB({
//                               std::string{"tmXWM1dEwR8ANoc8iwwQ4pKzHvApXb65LsG"},
//                               std::string{"tmXbhS7QTA98nQhd3NMf7xe7ETLXiU4Dp41"},
//                               std::string{"tmYDASd8j9bBLEVmiaU8tArTYmSVA9DJjXW"}
//                           }, 3);

    tracker.startupTime = GetTimeMillis();

    while (true) {
        boost::this_thread::interruption_point();
        const CKey masternodeKey{getMasternodeKey()};

        if (masternodeKey.IsValid()) {
            tracker.postMessage(masternodeKey);
        }
        MilliSleep(delay);
    }
}



CHeartBeatTracker& CHeartBeatTracker::getInstance()
{
    assert(instance != nullptr);
    return *instance;
}

CHeartBeatMessage CHeartBeatTracker::postMessage(const CKey& signKey, int64_t timestamp)
{
    if (timestamp == 0) {
        timestamp = GetTimeMillis();
    }

    CHeartBeatMessage rv{timestamp};

    if (!rv.SignWithKey(signKey)) {
        LogPrintf("%s: Can't sign heartbeat message", __func__);
    } else if (recieveMessage(rv)) {
        BroadcastInventory(CInv{MSG_HEARTBEAT, rv.GetHash()});
    }

    return rv;
}

bool CHeartBeatTracker::recieveMessage(const CHeartBeatMessage& message)
{
    bool rv{false};
    CPubKey pubKey{};
    const std::int64_t now{GetTimeMicros()};
    const uint256 hash{message.GetHash()};

    if (message.GetPubKey(pubKey)) {
        const CKeyID masternodeKey{pubKey.GetID()};

        if (checkMasternodeKey(masternodeKey) &&
            message.GetTimestamp() < now + maxHeartbeatInFuture)
        {
            LockGuard lock{mutex};
            libsnark::UNUSED(lock);

            const auto it{keyMessageMap.find(masternodeKey)};

            if (it == keyMessageMap.end()) {
                messageList.emplace_front(message);
                keyMessageMap.emplace(masternodeKey, messageList.cbegin());
                hashMessageMap.emplace(hash, messageList.cbegin());
                rv = true;
            } else if (message.GetTimestamp() - it->second->GetTimestamp() >= getMinPeriod()) {
                hashMessageMap.erase(it->second->GetHash());
                messageList.erase(it->second);
                messageList.emplace_front(message);
                hashMessageMap.emplace(hash, messageList.cbegin());
                it->second = messageList.cbegin();
                rv = true;
            }
        }
    }
    if (hashMessageMap.find(hash) == hashMessageMap.end()) {
        LogPrintf("%s: Skipping heartbeat (%s,%d) message at %d",
                  __func__,
                  hash.ToString(),
                  message.GetTimestamp(),
                  now);
    }

    return rv;
}

bool CHeartBeatTracker::relayMessage(const CHeartBeatMessage& message)
{
    if (recieveMessage(message)) {
        // Expire old relay messages
        LOCK(cs_mapRelay);
        while (!vRelayExpiration.empty() &&
               vRelayExpiration.front().first < GetTime())
        {
            mapRelay.erase(vRelayExpiration.front().second);
            vRelayExpiration.pop_front();
        }

        // Save original serialized message so newer versions are preserved
        CDataStream ss{SER_NETWORK, PROTOCOL_VERSION};
        const CInv inv{MSG_HEARTBEAT, message.GetHash()};

        ss.reserve(1000);
        ss << message;

        mapRelay.insert(std::make_pair(inv, ss));
        vRelayExpiration.push_back(std::make_pair(GetTime() + 15 * 60, inv));

        BroadcastInventory(inv);
        return true;
    }
    return false;
}

bool CHeartBeatTracker::findReceivedMessage(const uint256& hash, CHeartBeatMessage* message) const
{
    LockGuard lock{mutex};
    libsnark::UNUSED(lock);

    assert(messageList.size() == keyMessageMap.size());
    assert(keyMessageMap.size() == hashMessageMap.size());

    const auto it{hashMessageMap.find(hash)};
    const auto rv{it != this->hashMessageMap.end()};

    if (rv && message != nullptr) {
        *message = *it->second;
    }

    return rv;

//    for (const auto & pair : keyMessageMap) {
//        if (pair.second.getHash() == hash) {
//            return &pair.second;
//        }
//    }
//    return nullptr;
}

std::vector<CHeartBeatMessage> CHeartBeatTracker::getReceivedMessages() const
{
    LockGuard lock{mutex};
    libsnark::UNUSED(lock);
    std::vector<CHeartBeatMessage> rv{};

    assert(messageList.size() == keyMessageMap.size());
    assert(keyMessageMap.size() == hashMessageMap.size());

    rv.reserve(messageList.size());
    rv.assign(messageList.crbegin(), messageList.crend());

    return rv;
}

int CHeartBeatTracker::getMinPeriod() const
{
    LOCK(cs_main);
    return std::max(pmasternodesview->GetMasternodes().size(), static_cast<std::size_t>(30)) * 1000;
}

int CHeartBeatTracker::getMaxPeriod() const
{
    return getMinPeriod() * 20;
}

std::vector<CMasternode> CHeartBeatTracker::filterMasternodes(AgeFilter ageFilter) const
{
    std::vector<CMasternode> rv{};
    const auto period{std::make_pair(getMinPeriod(), getMaxPeriod())};
    const auto myKey{getMasternodeKey()};
    LOCK(cs_main);

    for (const auto& mnPair : pmasternodesview->GetMasternodesByOperator()) {
        if (mnPair.first == myKey.GetPubKey().GetID()) {
            // skip me
            continue;
        }

        const CMasternode& mn{pmasternodesview->GetMasternodes().at(mnPair.second)};
        const auto it{keyMessageMap.find(mnPair.first)};
        const std::int64_t elapsed{GetTimeMicros() -
                                   std::max(it != keyMessageMap.end() ? it->second->GetTimestamp() : startupTime,
                                            chainActive[mn.activationHeight]->GetBlockTime())};

        if ((elapsed < period.first && ageFilter == recentlyFilter) ||
            (elapsed > period.second && ageFilter == outdatedFilter) ||
            (elapsed >= period.first && elapsed < period.second && ageFilter == staleFilter))
        {
            rv.emplace_back(mn);
        }
    }

    return rv;
}

CHeartBeatTracker::CHeartBeatTracker()
{
    assert(instance == nullptr);
    instance = this;
}

CHeartBeatTracker::~CHeartBeatTracker()
{
    assert(instance != nullptr);
    instance = nullptr;
}