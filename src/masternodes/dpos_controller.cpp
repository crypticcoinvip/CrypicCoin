// Copyright (c) 2019 The Crypticcoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
#include "dpos_controller.h"
#include "dpos_voter.h"
#include "dpos_validator.h"
#include "../chainparams.h"
#include "../init.h"
#include "../key.h"
#include "../main.h"
#include "../net.h"
#include "../txdb.h"
#include "../wallet/wallet.h"
#include "../snark/libsnark/common/utils.hpp"
#include <mutex>
#include <future>

namespace dpos
{

using LockGuard = std::lock_guard<std::mutex>;
std::mutex mutex_{};
CDposController* dposControllerInstance_{nullptr};
std::array<unsigned char, 16> salt_{0x4D, 0x48, 0x7A, 0x52, 0x5D, 0x4D, 0x37, 0x78, 0x42, 0x36, 0x5B, 0x64, 0x44, 0x79, 0x59, 0x4F};

uint256 getTipBlockHash()
{
    LOCK(cs_main);
    return chainActive.Tip()->GetBlockHash();
}

int getTipBlockHeight()
{
    LOCK(cs_main);
    assert(chainActive.Tip()->nHeight == chainActive.Height());
    return chainActive.Height();
}

std::size_t getActiveMasternodeCount()
{
    LOCK(cs_main);
    return pmasternodesview->GetActiveMasternodes().size();
}

CKey getMasternodeKey()
{
    CKey rv{};
#ifdef ENABLE_WALLET
    LOCK(cs_main);
    const auto mnId{pmasternodesview->AmIActiveOperator()};
    if (mnId == boost::none ||
        !pwalletMain->GetKey(mnId.get().operatorAuthAddress, rv))
    {
        rv = CKey{};
    }
#endif
    return rv;
}

bool checkIsTeamMember(const BlockHash& tipHash, const CKeyID& operatorKey)
{
    int height{-100};
    LOCK(cs_main);

    for (CBlockIndex* index{chainActive.Tip()}; index != nullptr && height < 0; index = index->pprev, height++) {
        if (index->GetBlockHash() == tipHash) {
            height = index->nHeight;
            break;
        }
    }

    if (height > 0) {
        return pmasternodesview->IsTeamMember(height, operatorKey);
    }

    return false;
}

boost::optional<CMasternode::ID> findMasternodeId(const CKeyID& operatorKeyId = CKeyID{})
{
    CKeyID keyId{operatorKeyId};

    if (operatorKeyId.IsNull()) {
        const CKey key{getMasternodeKey()};
        if (key.IsValid()) {
            keyId = key.GetPubKey().GetID();
        }
    }

    if (!keyId.IsNull()) {
        LOCK(cs_main);
        const auto authIndex{CMasternodesView::AuthIndex::ByOperator};
        const auto mnIt{pmasternodesview->ExistMasternode(authIndex, keyId)};
        if (mnIt != boost::none) {
            return mnIt.get()->second;
        }
    }

    return boost::none;
}

boost::optional<CMasternode::ID> extractMasternodeId(const CRoundVote_p2p& vote)
{
    CPubKey pubKey{};
    if (pubKey.RecoverCompact(vote.GetSignatureHash(), vote.signature) &&
        checkIsTeamMember(vote.tip, pubKey.GetID()))
    {
        return findMasternodeId(pubKey.GetID());
    }
    return boost::none;
}

boost::optional<CMasternode::ID> extractMasternodeId(const CTxVote_p2p& vote)
{
    CPubKey pubKey{};
    if (pubKey.RecoverCompact(vote.GetSignatureHash(), vote.signature) &&
        checkIsTeamMember(vote.tip, pubKey.GetID()))
    {
        return findMasternodeId(pubKey.GetID());
    }
    return boost::none;
}


template<typename T>
void relayEntity(const T& entity, int type)
{
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
    const CInv inv{type, entity.GetHash()};

    ss.reserve(1024);
    ss << entity;

    mapRelay.insert(std::make_pair(inv, ss));
    vRelayExpiration.push_back(std::make_pair(GetTime() + 15 * 60, inv));
    BroadcastInventory(inv);
}


template<typename T, typename StoreMethod>
void storeEntity(const T& entity, StoreMethod storeMethod)
{
    (pdposdb->*storeMethod)(getTipBlockHash(), entity);
}

CDposController& CDposController::getInstance()
{
    if (dposControllerInstance_ == nullptr) {
        LockGuard lock{mutex_};
        libsnark::UNUSED(lock);
        if (dposControllerInstance_ == nullptr) {
            dposControllerInstance_ = new CDposController{};
            std::make_shared<CDposVoter>().swap(dposControllerInstance_->voter);
            std::make_shared<Validator>().swap(dposControllerInstance_->validator);
        }
    }
    assert(dposControllerInstance_ != nullptr);
    return *dposControllerInstance_;
}

void CDposController::runEventLoop()
{
    using std::placeholders::_1;
    using std::placeholders::_2;
    using std::placeholders::_3;
    using std::chrono::duration_cast;
    using std::chrono::seconds;
    using std::chrono::steady_clock;

    auto lastTime{steady_clock::now()};
    const auto startTime{lastTime};

    while (true) {
        boost::this_thread::interruption_point();
        if (duration_cast<seconds>(steady_clock::now() - startTime).count() > 60 * 60) {
            if (!getController()->voter->checkAmIVoter() && IsInitialBlockDownload()) {
                const auto mnId{findMasternodeId()};
                if (mnId != boost::none) {
                    LogPrintf("%s: Enabling dpos voter\n", __func__);
                    CDposVoter::Callbacks callbacks{};
                    Validator* validator{getController()->validator.get()};
                    callbacks.validateTxs = std::bind(&Validator::validateTx, validator, _1);
                    callbacks.validateBlock = std::bind(&Validator::validateBlock, validator, _1, _2, _3);
                    callbacks.allowArchiving = std::bind(&Validator::allowArchiving, validator, _1);
                    getController()->voter->setVoting(getTipBlockHash(), callbacks, true, mnId.get());
                }
            }
        }

        if (duration_cast<seconds>(steady_clock::now() - lastTime).count() > 30) {
            lastTime = steady_clock::now();
            LOCK(cs_vNodes);
            for (auto&& node : vNodes) {
                node->PushMessage("get_round_votes");
                node->PushMessage("get_tx_votes", getController()->listTxVotes());
            }
        }

        MilliSleep(500);
    }
}

bool CDposController::isEnabled() const
{
    const CChainParams& params{Params()};
    return NetworkUpgradeActive(getTipBlockHeight(), params.GetConsensus(), Consensus::UPGRADE_SAPLING) &&
           getActiveMasternodeCount() >= params.GetMinimalMasternodeCount();
}

CValidationInterface* CDposController::getValidator()
{
    return this->validator.get();
}

void CDposController::initialize()
{
    assert(pdposdb != nullptr);

    pdposdb->LoadViceBlocks([this](const uint256& tip, const CBlock& block) {
        assert(!this->voter->checkAmIVoter());
        this->voter->v[tip].viceBlocks.emplace(block.GetHash(), block);
    });
    pdposdb->LoadRoundVotes([this](const uint256& tip, const CRoundVote_p2p& vote) {
        assert(!this->voter->checkAmIVoter());
        const auto mnId{extractMasternodeId(vote)};
        if (mnId != boost::none) {
            CRoundVote roundVote{};
            roundVote.tip = vote.tip;
            roundVote.voter = mnId.get();
            roundVote.nRound = vote.nRound;
            roundVote.choice = vote.choice;

            this->recievedRoundVotes.emplace(vote.GetHash(), vote);
            this->voter->v[tip].roundVotes[roundVote.nRound].emplace(roundVote.voter, roundVote);
        }
    });
    pdposdb->LoadTxVotes([this](const uint256& tip, const CTxVote_p2p& vote) {
        assert(!this->voter->checkAmIVoter());
        const auto mnId{extractMasternodeId(vote)};

        if (mnId != boost::none) {
            for (const auto& choice : vote.choices) {
                CTxVote txVote{};
                txVote.tip = vote.tip;
                txVote.voter = mnId.get();
                txVote.nRound = vote.nRound;
                txVote.choice = choice;
                this->voter->v[tip].txVotes[txVote.nRound][choice.subject].emplace(txVote.voter, txVote);
            }
            this->recievedTxVotes.emplace(vote.GetHash(), vote);
        }
    });
}

void CDposController::updateChainTip()
{
    this->voter->updateTip(getTipBlockHash());
}

void CDposController::proceedViceBlock(const CBlock& viceBlock)
{
    if (!findViceBlock(viceBlock.GetHash())) {
        LockGuard lock{mutex_};
        libsnark::UNUSED(lock);
        const CDposVoterOutput out{voter->applyViceBlock(viceBlock)};

        if (handleVoterOutput(out)) {
            storeEntity(viceBlock, &CDposDB::WriteViceBlock);
            relayEntity(viceBlock, MSG_VICE_BLOCK);
        }
    }
}

void CDposController::proceedTransaction(const CTransaction& tx)
{
    LockGuard lock{mutex_};
    libsnark::UNUSED(lock);

    for (const auto& pair : this->voter->v) {
        if (pair.second.txs.count(tx.GetHash()) == 0) {
            handleVoterOutput(voter->applyTx(tx));
            break;
        }
    }
}

void CDposController::proceedRoundVote(const CRoundVote_p2p& vote)
{
    if (!findRoundVote(vote.GetHash())) {
        LockGuard lock{mutex_};
        libsnark::UNUSED(lock);

        if (acceptRoundVote(vote)) {
            this->recievedRoundVotes.emplace(vote.GetHash(), vote);
            storeEntity(vote, &CDposDB::WriteRoundVote);
            relayEntity(vote, MSG_ROUND_VOTE);
        }
    }
}

void CDposController::proceedTxVote(const CTxVote_p2p& vote)
{
    if (!findTxVote(vote.GetHash())) {
        LockGuard lock{mutex_};
        libsnark::UNUSED(lock);

        if (acceptTxVote(vote)) {
            this->recievedTxVotes.emplace(vote.GetHash(), vote);
            storeEntity(vote, &CDposDB::WriteTxVote);
            relayEntity(vote, MSG_TX_VOTE);
        }
    }
}

bool CDposController::findViceBlock(const uint256& hash, CBlock* block) const
{
    LockGuard lock{mutex_};
    libsnark::UNUSED(lock);

    for (const auto& pair : this->voter->v) {
        const auto it{pair.second.viceBlocks.find(hash)};
        if (it != pair.second.viceBlocks.end()) {
            if (block != nullptr) {
                *block = it->second;
            }
            return true;
        }
    }

    return false;
}

bool CDposController::findRoundVote(const uint256& hash, CRoundVote_p2p* vote) const
{
    LockGuard lock{mutex_};
    libsnark::UNUSED(lock);
    const auto it{this->recievedRoundVotes.find(hash)};
    const auto rv{it != this->recievedRoundVotes.end()};

    if (rv && vote != nullptr) {
        *vote = it->second;
    }

    return rv;
}

bool CDposController::findTxVote(const uint256& hash, CTxVote_p2p* vote) const
{
    LockGuard lock{mutex_};
    libsnark::UNUSED(lock);
    const auto it{this->recievedTxVotes.find(hash)};
    const auto rv{it != this->recievedTxVotes.end()};

    if (rv && vote != nullptr) {
        *vote = it->second;
    }

    return rv;
}

std::vector<CBlock> CDposController::listViceBlocks() const
{
    std::vector<CBlock> rv{};
    LockGuard lock{mutex_};
    libsnark::UNUSED(lock);

    for (const auto& pair1 : this->voter->v) {
        for (const auto& pair2 : pair1.second.viceBlocks) {
            rv.push_back(pair2.second);
        }
    }

    return rv;
}

std::vector<CRoundVote_p2p> CDposController::listRoundVotes() const
{
    std::vector<CRoundVote_p2p> rv{};
    LockGuard lock{mutex_};
    libsnark::UNUSED(lock);

    rv.reserve(this->recievedRoundVotes.size());

    for (const auto& pair : this->recievedRoundVotes) {
        assert(pair.first == pair.second.GetHash());
        rv.emplace_back(pair.second);
    }

    return rv;
}

std::vector<CTxVote_p2p> CDposController::listTxVotes() const
{
    std::vector<CTxVote_p2p> rv{};
    LockGuard lock{mutex_};
    libsnark::UNUSED(lock);

    rv.reserve(this->recievedTxVotes.size());

    for (const auto& pair : this->recievedTxVotes) {
        assert(pair.first == pair.second.GetHash());
        rv.emplace_back(pair.second);
    }

    return rv;
}

std::vector<CTransaction> CDposController::listCommittedTransactions() const
{
    std::vector<CTransaction> rv{};
    std::map<TxIdSorted, CTransaction> txs{};

    {
        LockGuard lock{mutex_};
        libsnark::UNUSED(lock);
        txs = this->voter->listCommittedTxs();
    }

    rv.reserve(txs.size());

    for (const auto& pair : txs) {
        rv.emplace_back(pair.second);
    }

    return rv;
}

bool CDposController::handleVoterOutput(const CDposVoterOutput& out)
{
    if (out.vErrors.empty()) {
        for (const auto& error : out.vErrors) {
            LogPrintf("%s: %s\n", __func__, error);
        }
        return false;
    }

    if (!out.empty()) {
        const CKey masternodeKey{getMasternodeKey()};

        if (masternodeKey.IsValid()) {
            for (const auto& roundVote : out.vRoundVotes) {
                CRoundVote_p2p vote{};
                vote.tip = roundVote.tip;
                vote.nRound = roundVote.nRound;
                vote.choice = roundVote.choice;
                if (!masternodeKey.SignCompact(vote.GetSignatureHash(), vote.signature)) {
                    LogPrintf("%s: Can't sign round vote\n", __func__);
                } else {
                    this->recievedRoundVotes.emplace(vote.GetHash(), vote);
                    storeEntity(vote, &CDposDB::WriteRoundVote);
                    relayEntity(vote, MSG_ROUND_VOTE);
                }
            }
            for (const auto& txVote : out.vTxVotes) {
                CTxVote_p2p vote{};
                vote.tip = txVote.tip;
                vote.nRound = txVote.nRound;
                vote.choices.push_back(txVote.choice);
                if (!masternodeKey.SignCompact(vote.GetSignatureHash(), vote.signature)) {
                    LogPrintf("%s: Can't sign tx vote\n", __func__);
                } else {
                    this->recievedTxVotes.emplace(vote.GetHash(), vote);
                    storeEntity(vote, &CDposDB::WriteTxVote);
                    relayEntity(vote, MSG_TX_VOTE);
                }
            }

            if (out.blockToSubmit != boost::none) {
                CValidationState state{};
                const CBlock* pblock{&out.blockToSubmit.get().block};
                if (!ProcessNewBlock(state, NULL, const_cast<CBlock*>(pblock), true, NULL)) {
                    LogPrintf("%s: Can't ProcessNewBlock\n", __func__);
                }
            }
        }
    }

    return true;
}

bool CDposController::acceptRoundVote(const CRoundVote_p2p& vote)
{
    bool rv{true};
    const auto mnId{extractMasternodeId(vote)};

    if (mnId == boost::none) {
        rv = false;
    } else {
        CRoundVote roundVote{};
        roundVote.tip = vote.tip;
        roundVote.voter = mnId.get();
        roundVote.nRound = vote.nRound;
        roundVote.choice = vote.choice;

        rv = handleVoterOutput(voter->applyRoundVote(roundVote));
    }

    return rv;
}

bool CDposController::acceptTxVote(const CTxVote_p2p& vote)
{
    bool rv{true};
    const auto mnId{extractMasternodeId(vote)};

    if (mnId == boost::none) {
        rv = false;
    } else {
        CTxVote txVote{};
        txVote.tip = vote.tip;
        txVote.voter = mnId.get();
        txVote.nRound = vote.nRound;

        for (const auto& choice : vote.choices) {
            txVote.choice = choice;

            if (!handleVoterOutput(voter->applyTxVote(txVote))) {
                rv = false;
                this->voter->pruneTxVote(txVote);
            }
        }
    }

    return rv;
}

} //namespace dpos
